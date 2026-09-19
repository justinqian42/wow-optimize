// ============================================================================
// Module: m2_batch_sort.cpp
//
// The opaque M2 batch sort in sub_821A20 (M2_DrawBatchBuilder). The render
// list is sorted with the client's heapsort, sub_83DCF0, which takes its
// comparator as a pointer:
//
//   sub_83DCF0(cmp, int* indices, unsigned count, ctx)
//
// indices name 68-byte records at [ctx+3Ch]. The opaque list is sorted with
// cmp = sub_81EEA0, which orders by record type and hands types 0 and 1 to
// sub_81EAD0. In the 2026-09-19 uncapped profile sub_81EAD0 is the largest
// wow.exe entry, 4.09% of executing time, and its weight sits on the first
// instruction after `mov ecx, [ecx+150h]`: every comparison starts by walking
// record -> +4 -> +2Ch -> +150h for both records before it compares anything.
// A heapsort makes about n log n comparisons, so each record's chain is walked
// that many times over for a value that cannot change while the sort runs.
//
// This keeps the client's heapsort and replaces only the comparator. Before the
// sort, every field sub_81EAD0 reads is gathered once per record into a flat
// array; the sort then runs with a comparator that reads that array and makes
// exactly the comparisons sub_81EAD0 makes, in its order and with its
// signedness. The heapsort consults only the sign of the answer and the
// comparator is pure, so equal answers give the same permutation, ties
// included.
//
// Transcribed from the disassembly of sub_81EAD0, record r, v45 =
// [[r+4]+2Ch]+150h:
//
//   type 0 only, in this order:
//     u16 [[r+28h]+0Ch]                                     unsigned
//     only when [r+30h] is non-zero in BOTH records:
//       [[r+30h] + 4*[r+34h] + 2Ch], then [[r+30h] + 4*[r+38h] + 194h]
//     [[r+4]+2Ch]                                           unsigned
//     [r+8] & 4                                             signed
//     [r+4] itself                                          unsigned
//     u16 [[r+2Ch]+0Eh]                                     unsigned
//   types 0 and 1, e = [v45+74h] + 4*u16[[r+28h]+0Ah]:
//     u16 [e+2], then byte [e] & 1Fh                        unsigned
//     for j below the smaller u16 [[r+28h]+0Eh] of the two:
//       idx = u16 [[v45+84h] + 2*u16[[r+28h]+10h] + 2j]
//       tex = idx < [v45+50h] ? [[r+4]+0A4h + 4*idx] : 0
//       (texA - texB) >> 2, arithmetic                      signed
//     u16 [[r+28h]+0Eh]                                     unsigned
//     [r+28h] itself: -1 when B's is above, else B's below A's
//
// sub_47BF20 is `(a - b) >> 2`, so two texture pointers less than four bytes
// apart compare equal; the difference is taken the same way here. Up to four
// texture entries are kept a record. A comparison that reaches a longer list
// goes to the client's own sub_81EAD0.
//
// Types 3 and 4 are compared by the client's sub_81ED10 and sub_81EDF0, called
// directly; any other type compares equal, as the client's switch has it.
//
// Reading ahead. The gather reads, for every record, fields the client reads
// only when a comparison gets that far. They belong to the same record and
// are live for the whole draw, but that is not proven, so the gather runs
// inside one exception frame per sort - not per comparison - and a fault
// hands that sort back to the client untouched.
//
// Verification. For the first 512 sorts and one in 64 after, every comparison
// runs both ways, the client's answer is the one returned, and the first
// difference retires this for the session.
// ============================================================================

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <intrin.h>
#include <cstdint>
#include <cstring>

#include "m2_batch_sort.h"
#include "MinHook.h"
#include "version.h"
#include "config.h"
#include "ab_test.h"
#include "session_verdict.h"
#include "sampling_profiler.h"

extern "C" void Log(const char* fmt, ...);
MH_STATUS WineSafe_CreateHook(void* target, void* detour, void** original);
MH_STATUS WO_EnableHook(void* target);

namespace M2BatchSort {

namespace {

constexpr uintptr_t kSort       = 0x0083DCF0;
constexpr uintptr_t kCmpOpaque  = 0x0081EEA0;
constexpr uintptr_t kCmpType01  = 0x0081EAD0;
constexpr uintptr_t kCmpType3   = 0x0081ED10;
constexpr uintptr_t kCmpType4   = 0x0081EDF0;

// push ebp / mov ebp,esp / sub esp,18h / push ebx / mov ebx,[ebp+10h] /
// cmp ebx,1 / jbe
const unsigned char kSortPrologue[16] = {
    0x55, 0x8B, 0xEC, 0x83, 0xEC, 0x18, 0x53, 0x8B,
    0x5D, 0x10, 0x83, 0xFB, 0x01, 0x0F, 0x86, 0x3B
};
// push ebp / mov ebp,esp / mov ecx,[ebp+8] / mov eax,[ebp+10h] /
// mov eax,[eax+3Ch] / mov edx,ecx / shl edx,4
const unsigned char kCmpPrologue[16] = {
    0x55, 0x8B, 0xEC, 0x8B, 0x4D, 0x08, 0x8B, 0x45,
    0x10, 0x8B, 0x40, 0x3C, 0x8B, 0xD1, 0xC1, 0xE2
};
// push ebp / mov ebp,esp / sub esp,14h / push ebx / mov ebx,[ebp+0Ch] /
// mov ecx,[ebx+4] / mov ecx,[ecx+2Ch] / mov ecx,[ecx+150h]
const unsigned char kType01Prologue[22] = {
    0x55, 0x8B, 0xEC, 0x83, 0xEC, 0x14, 0x53, 0x8B, 0x5D, 0x0C, 0x8B,
    0x4B, 0x04, 0x8B, 0x49, 0x2C, 0x8B, 0x89, 0x50, 0x01, 0x00, 0x00
};

constexpr uint32_t kRecordSize   = 68;
constexpr uint32_t kTex          = 4;
constexpr uint32_t kCapacity     = 16384;
constexpr uint32_t kLearnSorts   = 512;
constexpr uint32_t kResampleMask = 63;

typedef int  (__cdecl *CmpFn)(int, int, int);
typedef void (__cdecl *SortFn)(CmpFn, int32_t*, uint32_t, int);
typedef int  (__cdecl *PairFn)(const void*, const void*);

SortFn g_orig = nullptr;
const CmpFn  g_clientCmp = (CmpFn)kCmpOpaque;
const PairFn g_client01  = (PairFn)kCmpType01;
const PairFn g_client3   = (PairFn)kCmpType3;
const PairFn g_client4   = (PairFn)kCmpType4;

struct Key {
    int32_t  type;
    uint32_t model;       // [r+4]
    uint32_t modelData;   // [[r+4]+2Ch]
    uint32_t batch;       // [r+28h]
    uint32_t has30;
    uint32_t x2C;
    uint32_t x194;
    uint32_t flag4;       // [r+8] & 4
    uint32_t e1F;         // byte [e] & 1Fh
    uint16_t k0C;         // u16 [[r+28h]+0Ch]
    uint16_t w0E;         // u16 [[r+2Ch]+0Eh]
    uint16_t e2;          // u16 [e+2]
    uint16_t texCount;    // u16 [[r+28h]+0Eh]
    uint32_t tex[kTex];
};

Key*           g_keys = nullptr;
const uint8_t* g_base = nullptr;   // the record array of the sort in progress

bool g_installed = false;
bool g_dead = false;
bool g_busy = false;
bool g_abSubject = false;

// Main thread only, so plain counters; lower bounds if that ever stops being true.
unsigned long long g_sorts = 0;
unsigned long long g_armedSorts = 0;
unsigned long long g_records = 0;
unsigned long long g_verifiedSorts = 0;
unsigned long long g_verifiedCompares = 0;
unsigned long long g_longTex = 0;
unsigned long long g_tooLarge = 0;
unsigned long long g_faults = 0;
unsigned long long g_control = 0;
unsigned long long g_type01 = 0;
unsigned long long g_typeOther = 0;
uint32_t g_largest = 0;
unsigned g_mismatches = 0;

// Cycle samples: one armed sort and one client sort in every 64 after learning,
// each divided by n*log2(n) so sorts of different sizes can be added up.
unsigned long long g_mineCycles = 0, g_mineWork = 0, g_mineTimed = 0;
unsigned long long g_clientCycles = 0, g_clientWork = 0, g_clientTimed = 0;

inline uint32_t U32(uint32_t a) { return *(const uint32_t*)(uintptr_t)a; }
inline uint16_t U16(uint32_t a) { return *(const uint16_t*)(uintptr_t)a; }
inline uint8_t  U8(uint32_t a)  { return *(const uint8_t*)(uintptr_t)a; }

inline const uint8_t* Record(int i) {
    return g_base + (uint32_t)i * kRecordSize;
}

void GatherOne(const uint8_t* rp, Key* k) {
    const uint32_t r = (uint32_t)(uintptr_t)rp;
    k->type = *(const int32_t*)rp;
    if (k->type != 0 && k->type != 1) return;

    k->model = U32(r + 0x04);
    k->modelData = U32(k->model + 0x2C);
    const uint32_t v45 = U32(k->modelData + 0x150);
    k->batch = U32(r + 0x28);

    if (k->type == 0) {
        k->k0C = U16(k->batch + 0x0C);
        const uint32_t p30 = U32(r + 0x30);
        k->has30 = p30 != 0;
        if (p30) {
            k->x2C  = U32(p30 + 4 * U32(r + 0x34) + 0x2C);
            k->x194 = U32(p30 + 4 * U32(r + 0x38) + 0x194);
        }
        k->flag4 = U32(r + 0x08) & 4;
        k->w0E = U16(U32(r + 0x2C) + 0x0E);
    }

    const uint32_t e = U32(v45 + 0x74) + 4 * (uint32_t)U16(k->batch + 0x0A);
    k->e2 = U16(e + 2);
    k->e1F = U8(e) & 0x1F;
    k->texCount = U16(k->batch + 0x0E);

    const uint32_t list = U32(v45 + 0x84) + 2 * (uint32_t)U16(k->batch + 0x10);
    const uint32_t bound = U32(v45 + 0x50);
    const uint32_t texArr = U32(k->model + 0xA4);
    const uint32_t n = k->texCount < kTex ? k->texCount : kTex;
    for (uint32_t j = 0; j < n; ++j) {
        const uint32_t idx = U16(list + 2 * j);
        k->tex[j] = idx < bound ? U32(texArr + 4 * idx) : 0;
    }
}

// One exception frame per sort. Returns false on a fault; nothing has been
// written anywhere but the key array, so the client's sort can still run.
__declspec(noinline) bool Gather(const int32_t* idx, uint32_t n) {
    __try {
        for (uint32_t i = 0; i < n; ++i) {
            Key* k = &g_keys[idx[i]];
            GatherOne(Record(idx[i]), k);
            if (k->type == 0 || k->type == 1) ++g_type01;
            else ++g_typeOther;
        }
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

#define WO_ORDER(a, b) do { if ((a) < (b)) return -1; if ((a) > (b)) return 1; } while (0)

__forceinline int Compare01(int ia, int ib) {
    const Key& a = g_keys[ia];
    const Key& b = g_keys[ib];
    if (a.type == 0) {
        WO_ORDER(a.k0C, b.k0C);
        if (a.has30 && b.has30) {
            WO_ORDER(a.x2C, b.x2C);
            WO_ORDER(a.x194, b.x194);
        }
        WO_ORDER(a.modelData, b.modelData);
        WO_ORDER((int32_t)a.flag4, (int32_t)b.flag4);
        WO_ORDER(a.model, b.model);
        WO_ORDER(a.w0E, b.w0E);
    }
    WO_ORDER(a.e2, b.e2);
    WO_ORDER(a.e1F, b.e1F);
    const uint32_t n = a.texCount < b.texCount ? a.texCount : b.texCount;
    if (n > kTex) {
        ++g_longTex;
        return g_client01(Record(ia), Record(ib));
    }
    for (uint32_t j = 0; j < n; ++j) {
        const int32_t d = (int32_t)(a.tex[j] - b.tex[j]) >> 2;
        if (d < 0) return -1;
        if (d > 0) return 1;
    }
    WO_ORDER(a.texCount, b.texCount);
    if (b.batch > a.batch) return -1;
    return b.batch < a.batch;
}

#undef WO_ORDER

__forceinline int CompareMine(int ia, int ib) {
    const int32_t ta = g_keys[ia].type;
    const int32_t tb = g_keys[ib].type;
    if (ta < tb) return -1;
    if (ta > tb) return 1;
    switch ((uint32_t)ta) {
    case 0:
    case 1:  return Compare01(ia, ib);
    case 3:  return g_client3(Record(ia), Record(ib));
    case 4:  return g_client4(Record(ia), Record(ib));
    default: return 0;
    }
}

int __cdecl CmpArmed(int ia, int ib, int) {
    return CompareMine(ia, ib);
}

__declspec(noinline) void Retire(int ia, int ib, int mine, int theirs) {
    ++g_mismatches;
    g_dead = true;
    const Key& a = g_keys[ia];
    const Key& b = g_keys[ib];
    Log("[M2BatchSort] RETIRED: records %d and %d (types %d and %d) compared %d here "
        "and %d in the client. Texture counts %u and %u, batches 0x%08X and 0x%08X. "
        "The client's answer was the one used, and every sort from here on is the "
        "client's.", ia, ib, a.type, b.type, mine, theirs, a.texCount, b.texCount,
        a.batch, b.batch);
    Verdict::Add(Verdict::Bad, "M2BatchSort compared two render batches differently "
                 "from the client and retired itself for this session");
}

int __cdecl CmpVerify(int ia, int ib, int ctx) {
    const int theirs = g_clientCmp(ia, ib, ctx);
    if (!g_dead) {
        const int mine = CompareMine(ia, ib);
        ++g_verifiedCompares;
        if (mine != theirs) Retire(ia, ib, mine, theirs);
    }
    return theirs;
}

inline unsigned long long Work(uint32_t n) {
    unsigned long bit = 0;
    _BitScanReverse(&bit, n);
    return (unsigned long long)n * (bit ? bit : 1);
}

void __cdecl Hooked_Sort(CmpFn cmp, int32_t* idx, uint32_t n, int ctx) {
    if ((uintptr_t)cmp != kCmpOpaque || g_dead || g_busy || n < 2) {
        g_orig(cmp, idx, n, ctx);
        return;
    }
    ++g_sorts;
    if (g_abSubject && AbTest::StandAside()) {
        ++g_control;
        g_orig(cmp, idx, n, ctx);
        return;
    }
    if (n > g_largest) g_largest = n;

    uint32_t maxIdx = 0;
    for (uint32_t i = 0; i < n; ++i)
        if ((uint32_t)idx[i] > maxIdx) maxIdx = (uint32_t)idx[i];
    if (maxIdx >= kCapacity) {
        ++g_tooLarge;
        g_orig(cmp, idx, n, ctx);
        return;
    }

    const unsigned long long s = g_sorts;
    const bool verify = g_verifiedSorts < kLearnSorts || (s & kResampleMask) == 0;
    const bool timeClient = !verify && (s & kResampleMask) == 32;
    const bool timeMine = !verify && (s & kResampleMask) == 16;

    if (timeClient) {
        const unsigned long long t0 = __rdtsc();
        g_orig(cmp, idx, n, ctx);
        g_clientCycles += __rdtsc() - t0;
        g_clientWork += Work(n);
        ++g_clientTimed;
        return;
    }

    g_busy = true;
    const unsigned long long t0 = timeMine ? __rdtsc() : 0;
    g_base = *(const uint8_t* const*)((uintptr_t)ctx + 0x3C);
    if (!Gather(idx, n)) {
        ++g_faults;
        g_busy = false;
        g_orig(cmp, idx, n, ctx);
        return;
    }
    g_records += n;
    if (verify) {
        g_orig(&CmpVerify, idx, n, ctx);
        if (!g_dead) ++g_verifiedSorts;
    } else {
        g_orig(&CmpArmed, idx, n, ctx);
        ++g_armedSorts;
        if (timeMine) {
            g_mineCycles += __rdtsc() - t0;
            g_mineWork += Work(n);
            ++g_mineTimed;
        }
    }
    g_busy = false;
}

bool BytesMatch(uintptr_t addr, const unsigned char* want, size_t n) {
    __try {
        return memcmp((const void*)addr, want, n) == 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

}  // namespace

bool Init() {
    if (!Config::g_settings.OptM2BatchSort) return true;

    if (!BytesMatch(kSort, kSortPrologue, sizeof(kSortPrologue)) ||
        !BytesMatch(kCmpOpaque, kCmpPrologue, sizeof(kCmpPrologue)) ||
        !BytesMatch(kCmpType01, kType01Prologue, sizeof(kType01Prologue))) {
        Log("[M2BatchSort] NOT active: the bytes at 0x%08X, 0x%08X or 0x%08X are not "
            "the heapsort and comparators this was read from, so nothing was hooked.",
            (unsigned)kSort, (unsigned)kCmpOpaque, (unsigned)kCmpType01);
        return false;
    }
    if (!WowOpt_ClientPatchAllowed((const void*)kSort)) {
        Log("[M2BatchSort] NOT active: No Client Patches is on, and this hooks a "
            "function inside wow.exe.");
        return false;
    }

    g_keys = (Key*)VirtualAlloc(nullptr, (SIZE_T)kCapacity * sizeof(Key),
                                MEM_COMMIT | MEM_RESERVE | MEM_TOP_DOWN, PAGE_READWRITE);
    if (!g_keys) {
        Log("[M2BatchSort] NOT active: the %u KB key array could not be committed, "
            "error %lu", (unsigned)((SIZE_T)kCapacity * sizeof(Key) / 1024),
            GetLastError());
        return false;
    }

    if (WineSafe_CreateHook((void*)kSort, (void*)&Hooked_Sort, (void**)&g_orig) != MH_OK) {
        Log("[M2BatchSort] NOT active: the hook on 0x%08X could not be created.",
            (unsigned)kSort);
        return false;
    }
    if (WO_EnableHook((void*)kSort) != MH_OK) {
        MH_RemoveHook((void*)kSort);
        Log("[M2BatchSort] NOT active: the hook on 0x%08X could not be enabled.",
            (unsigned)kSort);
        return false;
    }
    g_installed = true;
    g_abSubject = AbTest::IsSubject("M2BatchSort", &g_abSubject);
    SamplingProfiler::RegisterSelfSymbol("M2BatchSort_CmpArmed", (const void*)&CmpArmed);
    SamplingProfiler::RegisterSelfSymbol("M2BatchSort_CmpVerify", (const void*)&CmpVerify);

    Log("[M2BatchSort] ACTIVE on the opaque M2 batch sort (heapsort sub_83DCF0 with "
        "comparator sub_81EEA0). sub_81EAD0 was 4.09%% of executing time in an "
        "uncapped profile, spent walking the same pointer chain for both records on "
        "every comparison. The chain is walked once per record before the sort and "
        "the client's heapsort runs on the gathered fields. The first %u sorts, and "
        "one in %u after, compare every answer with the client's; the first "
        "difference switches this off.", kLearnSorts, kResampleMask + 1);
    if (g_abSubject)
        Log("[M2BatchSort]   under A/B test: the control half runs the client's "
            "comparator through the same hook.");
    return true;
}

void Shutdown() {
    if (!g_installed) return;
    MH_DisableHook((void*)kSort);
    g_installed = false;
}

void LogStats() {
    if (!Config::g_settings.OptM2BatchSort) return;
    if (!g_installed) {
        Log("[M2BatchSort] not installed - the reason is at the top of this log");
        return;
    }
    if (g_sorts == 0) {
        Log("[M2BatchSort] hooked, and no opaque batch sort has been reached yet. That "
            "is a measurement: nothing has drawn M2 batches since it went in.");
        return;
    }
    Log("[M2BatchSort] %llu opaque sort(s), %llu record(s) gathered, largest %u. "
        "%llu sort(s) ran on gathered keys unchecked. Records of type 0 or 1: %llu, "
        "other types: %llu. Plain counters, lower bounds.",
        g_sorts, g_records, g_largest, g_armedSorts, g_type01, g_typeOther);
    if (g_mismatches) {
        Log("[M2BatchSort]   DISABLED after a comparison that differed from the "
            "client's; the line that says which is earlier in this log.");
    } else {
        Log("[M2BatchSort]   %llu sort(s) verified, %llu comparisons, every one equal "
            "to the client's.%s", g_verifiedSorts, g_verifiedCompares,
            g_verifiedSorts < kLearnSorts ? " Still learning; every sort is the "
            "client's answer." : "");
    }
    if (g_longTex)
        Log("[M2BatchSort]   %llu comparison(s) reached more than %u texture entries "
            "and went to the client's sub_81EAD0.", g_longTex, kTex);
    if (g_tooLarge || g_faults)
        Log("[M2BatchSort]   left to the client: %llu sort(s) with a record index past "
            "%u, %llu where gathering the fields faulted.", g_tooLarge, kCapacity,
            g_faults);
    if (g_mineTimed && g_clientTimed) {
        const double mine = (double)g_mineCycles / (double)g_mineWork;
        const double theirs = (double)g_clientCycles / (double)g_clientWork;
        Log("[M2BatchSort]   timed: %.1f cycles per n*log2(n) here over %llu sorts, "
            "%.1f with the client's comparator over %llu sorts, %.2fx. Sampled sorts "
            "of whatever size came up; this says how much faster the sort is, not "
            "how much of a frame it is worth.", mine, g_mineTimed, theirs,
            g_clientTimed, mine > 0.0 ? theirs / mine : 0.0);
    } else {
        Log("[M2BatchSort]   timed: not measured yet (%llu sorts here, %llu with the "
            "client's comparator).", g_mineTimed, g_clientTimed);
    }
    if (g_abSubject)
        Log("[M2BatchSort]   %llu sort(s) ran the client's comparator as the A/B "
            "control half.", g_control);
}

}  // namespace M2BatchSort
