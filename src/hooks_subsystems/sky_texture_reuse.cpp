// ============================================================================
// Module: sky_texture_reuse.cpp
//
// The cloud texture build, sub_7EFD00. In a tester's 2026-09-19 profile it is
// 10.29% of executing main-thread time, the largest single entry, reached once
// a frame from the sky and fog setup sub_7816F0.
//
// What it is. The client keeps a square cloud texture and rebuilds it a few
// rows at a time, cycling through the whole thing over several frames. For
// every texel it sums octaves of value noise out of the 256-byte table at
// 0x00AF4A70, turns the sum into a coverage byte through 0x00D38588, and where
// there is coverage it lights the texel - three multiply-adds, two reciprocal
// square roots by the 0x5F3997BB approximation, three clamps - and writes four
// bytes. Then it locks one of two textures and uploads the rows it just wrote.
//
// The object, with the offsets this module reads:
//
//   +04 density, float      +08 density byte      +09 noise table index
//   +0A rebuild-all flag    +0B which texture     +0C phase scale
//   +10 rows per pass       +14 next row          +1C width and height
//   +20 row shift           +28 octaves           +2C the gate
//   +38 the RGBA buffer     +44 the coverage buffer  +50 one row of noise
//   +88 phase word          +8C seconds            +90/+94 the two textures
//
// The observation. The texel content depends on the phase word at +88, the
// density byte at +08 and which rows are being built - and the phase word is
// quantised: it is `(int)(+0C * +8C)` truncated to sixteen bits, recomputed
// only when a cycle finishes. The seconds at +8C advance by one frame time a
// pass. So whenever a whole cycle passes without that truncated value changing,
// every pass of the new cycle writes the same bytes into the same buffer it
// wrote them into last cycle, and then uploads them again.
//
// What this does. It remembers, per starting row, the inputs of the pass that
// last built those rows and a hash of the bytes that came out. When the inputs
// repeat it does the client's bookkeeping - the seconds, the density byte, the
// lock, the upload, the row advance, the phase and buffer flip - and skips the
// texel loops, because the buffer already holds exactly what they would write.
//
// Whether that ever happens depends on how fast the clouds drift against how
// fast the texture cycles, which is runtime data - a CVar and a frame time -
// and is not knowable from the disassembly. So this measures it rather than
// assuming it: while learning it changes nothing, and the report says how many
// passes could have been skipped. If the answer is none, the log says so and
// nothing is skipped, which is a measurement and not a failure.
//
// Verification, two independent parts, both against the client on live data:
//
//   - The skip itself. On a pass whose inputs repeat, the client still runs and
//     the bytes are hashed before and after. Identical means the skip would
//     have been safe, proven on the real thing rather than argued. Different
//     means the inputs are not the whole story, and this retires for the
//     session rather than skipping on a guess.
//   - The bookkeeping. Predict-then-compare: the eight fields the function
//     updates are worked out beforehand and compared with what the client left.
//     The first disagreement retires it, because that bookkeeping is what the
//     armed path does itself.
//
// Arming needs both, and one pass in 64 goes back through the client afterwards.
// ============================================================================

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <cstdint>
#include <cstring>

#include "sky_texture_reuse.h"
#include "sky_cloud_texels.h"
#include "MinHook.h"
#include "version.h"
#include "config.h"
#include "ab_test.h"
#include "session_verdict.h"
#include "sampling_profiler.h"

extern "C" void Log(const char* fmt, ...);
MH_STATUS WineSafe_CreateHook(void* target, void* detour, void** original);
MH_STATUS WO_EnableHook(void* target);

namespace SkyTextureReuse {

namespace {

constexpr uintptr_t kTarget     = 0x007EFD00;
constexpr uintptr_t kLockTex    = 0x004B6CB0;   // returns the surface to upload into
constexpr uintptr_t kUploadRows = 0x00681F20;

// The globals the head of the function reads: the frame time it adds to the
// seconds, the fallback density, the 0.99 it compares layer alphas against,
// and the layer array it scans for one that hides the sky.
constexpr uintptr_t kFrameTime    = 0x00D38B48;
constexpr uintptr_t kDefDensity   = 0x00D38C34;
constexpr uintptr_t kOpaqueLimit  = 0x009E2FF4;   // 0.99
constexpr uintptr_t kGlobalFlag   = 0x00D38B5C;
constexpr uintptr_t kGlobalAlpha  = 0x00D38B60;
constexpr uintptr_t kLayerAlpha0  = 0x00D38B70;
constexpr uintptr_t kLayerAlphaEnd= 0x00D38B7C;

// push ebp / mov ebp,esp / sub esp,240h / push esi / push edi / mov esi,ecx /
// xor edi,edi / cmp [esi+2Ch],edi
const unsigned char kPrologue[17] = {
    0x55, 0x8B, 0xEC, 0x81, 0xEC, 0x40, 0x02, 0x00, 0x00,
    0x56, 0x57, 0x8B, 0xF1, 0x33, 0xFF, 0x39, 0x7E
};

// Field offsets in the cloud object.
constexpr unsigned kF_density    = 0x04;
constexpr unsigned kF_densByte   = 0x08;
constexpr unsigned kF_tableIdx   = 0x09;
constexpr unsigned kF_rebuildAll = 0x0A;
constexpr unsigned kF_whichTex   = 0x0B;
constexpr unsigned kF_phaseScale = 0x0C;
constexpr unsigned kF_rowsPass   = 0x10;
constexpr unsigned kF_nextRow    = 0x14;
constexpr unsigned kF_size       = 0x1C;
constexpr unsigned kF_shift      = 0x20;
constexpr unsigned kF_octaves    = 0x28;
constexpr unsigned kF_gate       = 0x2C;
constexpr unsigned kF_rgba       = 0x38;
constexpr unsigned kF_coverage   = 0x44;
constexpr unsigned kF_noiseRow   = 0x50;
constexpr unsigned kF_phase      = 0x88;
constexpr unsigned kF_seconds    = 0x8C;
constexpr unsigned kF_textures   = 0x90;

// Sanity bounds. Anything outside these is handed to the client untouched
// rather than indexed into.
constexpr uint32_t kMaxSize  = 2048;
constexpr uint32_t kMaxShift = 12;

constexpr unsigned kSlots         = 64;     // one per starting row seen
constexpr unsigned long kProveRepeats = 16; // proven identical rebuilds before arming
constexpr unsigned long kProveBooks   = 64; // proven bookkeeping predictions
constexpr unsigned long kResampleMask = 63;

typedef void (__fastcall* BuildFn)(void* self, void* edx);
typedef void* (__cdecl* LockFn)(void* tex, int a, int b);
typedef void (__cdecl* UploadFn)(void* surface, int a, int firstRow, int size,
                                 int lastRow, int b);

BuildFn  g_orig = nullptr;
const LockFn   g_lock   = (LockFn)kLockTex;
const UploadFn g_upload = (UploadFn)kUploadRows;

struct Key {
    uint32_t phase, densByte, firstRow, rows, size, shift, octaves, tableIdx;
    uint32_t rgba, coverage, noiseRow;
    bool operator==(const Key& o) const {
        return memcmp(this, &o, sizeof(Key)) == 0;
    }
};

struct Slot {
    bool     used;
    uint32_t firstRow;
    Key      key;
    uint64_t hash;
};

Slot g_slots[kSlots];

bool g_installed = false;
bool g_dead = false;
bool g_abSubject = false;
DWORD g_owner = 0;

// Main thread only, so plain counters; lower bounds if that ever stops being true.
unsigned long long g_calls = 0;
unsigned long long g_skipped = 0;
unsigned long long g_built = 0;
unsigned long long g_control = 0;
unsigned long long g_odd = 0;
unsigned long g_repeatsSeen = 0;
unsigned long g_repeatsProven = 0;
unsigned long g_booksProven = 0;
unsigned g_mismatches = 0;
unsigned long long g_phaseChanges = 0;
unsigned long long g_cycles = 0;

// What the client's own build costs, in milliseconds.
//
// Two field sessions disagree about this function by a factor of ten: it is
// 10.29% of executing main-thread time in one and 1.10% in the other, and the
// first of those machines spends 39% of its main thread blocked at a frame time
// pinned near 142 fps, so its shares are shares of a much smaller budget.
// Moving this build to a worker thread means transcribing about 250
// instructions of noise and lighting, and the number that decides whether that
// is worth building is milliseconds a pass on a machine that is busy. Nothing
// has measured that. The counter is a performance-counter pair around the call,
// which costs tens of nanoseconds against a build measured in milliseconds.
long long g_buildTicks = 0;
unsigned long long g_buildTimed = 0;
long long g_qpcFreq = 0;

inline uint32_t RD32(const void* p, unsigned off) {
    return *(const uint32_t*)((const char*)p + off);
}
inline uint8_t RD8(const void* p, unsigned off) {
    return *(const uint8_t*)((const char*)p + off);
}
inline float RDF(const void* p, unsigned off) {
    return *(const float*)((const char*)p + off);
}
inline void WR32(void* p, unsigned off, uint32_t v) {
    *(uint32_t*)((char*)p + off) = v;
}
inline void WR8(void* p, unsigned off, uint8_t v) {
    *(uint8_t*)((char*)p + off) = v;
}
inline void WR16(void* p, unsigned off, uint16_t v) {
    *(uint16_t*)((char*)p + off) = v;
}
inline void WRF(void* p, unsigned off, float v) {
    *(float*)((char*)p + off) = v;
}

// The two early returns at the head of the function: no buffer, or a sky layer
// opaque enough to hide the clouds. Both leave the rebuild flag clear and do
// nothing else.
bool SkyIsHidden() {
    const float limit = *(const float*)kOpaqueLimit;
    if (*(const uint32_t*)kGlobalFlag != 0 && limit < *(const float*)kGlobalAlpha)
        return true;
    for (uintptr_t p = kLayerAlpha0; p < kLayerAlphaEnd; p += 4) {
        if (*(const uint32_t*)(p - 0x0C) != 0 &&
            limit < *(const float*)p &&
            *(const uint32_t*)(p + 0x0C) == 0)
            return true;
    }
    return false;
}

uint64_t Fnv(const void* data, size_t n, uint64_t h) {
    const unsigned char* p = (const unsigned char*)data;
    for (size_t i = 0; i < n; ++i) { h ^= p[i]; h *= 0x100000001b3ULL; }
    return h;
}

// Exactly the bytes the texel loops write: for each row of the pass, four bytes
// a texel in the RGBA buffer and one in the coverage buffer, plus the single
// row of noise that carries the vertical difference to the next row.
uint64_t HashRows(const void* self, uint32_t firstRow, uint32_t rows) {
    const uint32_t size  = RD32(self, kF_size);
    const uint32_t shift = RD32(self, kF_shift);
    const char* rgba = (const char*)(uintptr_t)RD32(self, kF_rgba);
    const char* cov  = (const char*)(uintptr_t)RD32(self, kF_coverage);
    const char* row  = (const char*)(uintptr_t)RD32(self, kF_noiseRow);
    uint64_t h = 0xcbf29ce484222325ULL;
    for (uint32_t i = 0; i < rows; ++i) {
        const uint32_t r = firstRow + i;
        h = Fnv(rgba + 4 * ((size_t)r << shift), (size_t)size * 4, h);
        h = Fnv(cov + ((size_t)r << shift), size, h);
    }
    return Fnv(row, (size_t)size * 4, h);
}

Key MakeKey(const void* self, uint32_t firstRow, uint32_t rows, uint32_t densByte) {
    Key k;
    k.phase    = *(const uint16_t*)((const char*)self + kF_phase);
    k.densByte = densByte;
    k.firstRow = firstRow;
    k.rows     = rows;
    k.size     = RD32(self, kF_size);
    k.shift    = RD32(self, kF_shift);
    k.octaves  = RD32(self, kF_octaves);
    k.tableIdx = RD8(self, kF_tableIdx);
    k.rgba     = RD32(self, kF_rgba);
    k.coverage = RD32(self, kF_coverage);
    k.noiseRow = RD32(self, kF_noiseRow);
    return k;
}

Slot* FindSlot(uint32_t firstRow) {
    for (unsigned i = 0; i < kSlots; ++i)
        if (g_slots[i].used && g_slots[i].firstRow == firstRow) return &g_slots[i];
    for (unsigned i = 0; i < kSlots; ++i)
        if (!g_slots[i].used) { g_slots[i].used = true; g_slots[i].firstRow = firstRow;
                                g_slots[i].hash = 0; return &g_slots[i]; }
    return nullptr;
}

// What the function leaves behind, worked out before it runs.
struct Book {
    uint32_t rowsPass;     // +10 on exit
    uint32_t nextRow;      // +14
    uint8_t  densByte;     // +08
    uint8_t  rebuildAll;   // +0A
    uint8_t  whichTex;     // +0B
    uint16_t phase;        // +88
    float    seconds;      // +8C
    // What the upload was called with, so the armed path repeats it exactly.
    uint32_t uploadFirst, uploadLast, uploadTex;
};

// The seconds, the density byte and the rebuild-all adjustment, in the order the
// client does them. float + float is computed in double and rounded once on the
// store, which is what the client's x87 at 53-bit precision does.
void BookStart(const void* self, Book* b, uint32_t* firstRow, uint32_t* rows) {
    const uint32_t saved = RD32(self, kF_rowsPass);
    b->rebuildAll = RD8(self, kF_rebuildAll);
    uint32_t useRows = saved;
    uint32_t useFirst = RD32(self, kF_nextRow);
    if (b->rebuildAll) { useFirst = 0; useRows = RD32(self, kF_size); }

    b->seconds = (float)((double)RDF(self, kF_seconds) + (double)*(const float*)kFrameTime);

    double density = *(const float*)kDefDensity;
    const float own = RDF(self, kF_density);
    if (!(0.0f == own)) density = own;
    b->densByte = (uint8_t)(int)((1.0 - density) * 255.0);

    b->rowsPass = saved;     // restored at the end when rebuild-all was set
    *firstRow = useFirst;
    *rows = useRows;
}

// The tail: which texture is uploaded into, the row advance, and the phase and
// buffer flip at the end of a cycle.
void BookEnd(const void* self, Book* b, uint32_t firstRow, uint32_t rows) {
    const uint8_t which = RD8(self, kF_whichTex);
    b->whichTex = which;
    b->uploadTex = RD32(self, kF_textures + 4 * (uint32_t)((uint8_t)(which - 1) & 1));
    b->uploadFirst = firstRow;
    b->uploadLast = firstRow + rows;

    uint32_t next = firstRow + rows;
    const uint32_t size = RD32(self, kF_size);
    uint16_t phase = *(const uint16_t*)((const char*)self + kF_phase);
    if (next >= size) {
        const uint16_t want = (uint16_t)(int)((double)RDF(self, kF_phaseScale) *
                                              (double)b->seconds);
        if (want != phase) {
            b->whichTex = (uint8_t)((which - 1) & 1);
            phase = want;
        } else if (b->rebuildAll) {
            b->whichTex = (uint8_t)((which - 1) & 1);
        }
        next = 0;
    }
    b->nextRow = next;
    b->phase = phase;
}

__declspec(noinline) void Retire(const char* why) {
    if (g_dead) return;
    ++g_mismatches;
    g_dead = true;
    Log("[SkyTextureReuse] RETIRED: %s. Every cloud texture pass from here on is "
        "the client's own.", why);
    Verdict::Add(Verdict::Bad, "SkyTextureReuse disagreed with the client on the "
                 "cloud texture build and retired itself for this session");
}

__declspec(noinline) bool BookMatches(const void* self, const Book& b) {
    uint32_t sec;
    memcpy(&sec, &b.seconds, 4);
    const bool ok =
        RD32(self, kF_rowsPass) == b.rowsPass &&
        RD32(self, kF_nextRow)  == b.nextRow &&
        RD8(self, kF_densByte)  == b.densByte &&
        RD8(self, kF_rebuildAll) == 0 &&
        RD8(self, kF_whichTex)  == b.whichTex &&
        *(const uint16_t*)((const char*)self + kF_phase) == b.phase &&
        RD32(self, kF_seconds)  == sec;
    if (!ok) {
        Log("[SkyTextureReuse] the client left rows/pass %u next row %u density %u "
            "texture %u phase %u seconds %08X; this predicted %u, %u, %u, %u, %u, "
            "%08X.", RD32(self, kF_rowsPass), RD32(self, kF_nextRow),
            RD8(self, kF_densByte), RD8(self, kF_whichTex),
            *(const uint16_t*)((const char*)self + kF_phase), RD32(self, kF_seconds),
            b.rowsPass, b.nextRow, b.densByte, b.whichTex, b.phase, sec);
    }
    return ok;
}

// The client's own tail, done here when the texel loops are skipped.
void ApplyBook(void* self, const Book& b) {
    WRF(self, kF_seconds, b.seconds);
    WR8(self, kF_densByte, b.densByte);
    void* surface = g_lock((void*)(uintptr_t)b.uploadTex, 1, 0);
    if (surface) {
        g_upload(surface, 0, (int)b.uploadFirst, (int)RD32(self, kF_size),
                 (int)b.uploadLast, 1);
    }
    WR32(self, kF_rowsPass, b.rowsPass);
    WR32(self, kF_nextRow, b.nextRow);
    WR16(self, kF_phase, b.phase);
    WR8(self, kF_whichTex, b.whichTex);
    WR8(self, kF_rebuildAll, 0);
}

// The client's own build, timed, with the texel comparison bracketing it. Every
// path that reaches the loops goes through here so the milliseconds and the
// comparison both count the same passes.
void RunClientBuild(void* self, void* edx) {
    SkyCloudTexels::Before(self);
    LARGE_INTEGER tA, tB;
    QueryPerformanceCounter(&tA);
    g_orig(self, edx);
    QueryPerformanceCounter(&tB);
    g_buildTicks += tB.QuadPart - tA.QuadPart;
    ++g_buildTimed;
    ++g_built;
    SkyCloudTexels::After(self);
}

void __fastcall Detour(void* self, void* edx) {
    ++g_calls;
    if (g_dead || !self) { g_orig(self, edx); return; }
    if (!Config::g_settings.OptSkyTextureReuse) {
        // Only the texel comparison wanted this hook. Nothing is remembered and
        // nothing is skipped; the client builds every pass.
        RunClientBuild(self, edx);
        return;
    }
    if (g_owner == 0) g_owner = GetCurrentThreadId();
    if (GetCurrentThreadId() != g_owner) { ++g_odd; g_orig(self, edx); return; }

    if (RD32(self, kF_gate) == 0 || SkyIsHidden()) { g_orig(self, edx); return; }

    const uint32_t size  = RD32(self, kF_size);
    const uint32_t shift = RD32(self, kF_shift);
    const uint32_t rowsPass = RD32(self, kF_rowsPass);
    if (size == 0 || size > kMaxSize || shift > kMaxShift ||
        rowsPass == 0 || rowsPass > size ||
        RD32(self, kF_rgba) == 0 || RD32(self, kF_coverage) == 0 ||
        RD32(self, kF_noiseRow) == 0 || (uint32_t)(1u << shift) < size) {
        ++g_odd;
        g_orig(self, edx);
        return;
    }
    if (g_abSubject && AbTest::StandAside()) { ++g_control; g_orig(self, edx); return; }

    Book book;
    uint32_t firstRow = 0, rows = 0;
    BookStart(self, &book, &firstRow, &rows);
    if (firstRow + rows > size) { ++g_odd; g_orig(self, edx); return; }
    BookEnd(self, &book, firstRow, rows);

    const Key key = MakeKey(self, firstRow, rows, book.densByte);
    Slot* slot = FindSlot(firstRow);
    const bool repeat = slot && slot->hash != 0 && slot->key == key;
    if (repeat) ++g_repeatsSeen;

    const bool armed = (g_repeatsProven >= kProveRepeats) &&
                       (g_booksProven >= kProveBooks);
    const bool check = !armed || ((unsigned long)g_calls & kResampleMask) == 0;

    if (armed && repeat && !check) {
        ApplyBook(self, book);
        ++g_skipped;
        return;
    }

    const uint64_t before = repeat ? HashRows(self, firstRow, rows) : 0;
    RunClientBuild(self, edx);

    if (!BookMatches(self, book)) {
        Retire("the fields the function leaves behind are not what this predicted");
        return;
    }
    ++g_booksProven;

    const uint64_t after = HashRows(self, firstRow, rows);
    if (repeat) {
        if (before != after) {
            Retire("a pass with the same inputs wrote different bytes, so the inputs "
                   "this keys on are not the whole story");
            return;
        }
        ++g_repeatsProven;
    }
    if (slot) { slot->key = key; slot->hash = after; }

    if (book.phase != key.phase) ++g_phaseChanges;
    if (book.nextRow == 0) ++g_cycles;
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
    // The hook is shared. SkyCloudTexels reads the same call and two modules
    // cannot hook one address, so this installs it whenever either wants it, and
    // the reuse logic below stays gated on its own switch.
    if (!Config::g_settings.OptSkyTextureReuse &&
        !Config::g_settings.OptSkyCloudTexels) return true;

    if (!BytesMatch(kTarget, kPrologue, sizeof(kPrologue))) {
        Log("[SkyTextureReuse] NOT active: the bytes at 0x%08X are not the cloud "
            "texture build this was read from, so nothing was hooked.",
            (unsigned)kTarget);
        return false;
    }
    if (!WowOpt_ClientPatchAllowed((const void*)kTarget)) {
        Log("[SkyTextureReuse] NOT active: No Client Patches is on, and this hooks a "
            "function inside wow.exe.");
        return false;
    }
    if (WineSafe_CreateHook((void*)kTarget, (void*)&Detour, (void**)&g_orig) != MH_OK) {
        Log("[SkyTextureReuse] NOT active: the hook on 0x%08X could not be created.",
            (unsigned)kTarget);
        return false;
    }
    if (WO_EnableHook((void*)kTarget) != MH_OK) {
        MH_RemoveHook((void*)kTarget);
        Log("[SkyTextureReuse] NOT active: the hook on 0x%08X could not be enabled.",
            (unsigned)kTarget);
        return false;
    }
    g_installed = true;
    {
        LARGE_INTEGER f;
        if (QueryPerformanceFrequency(&f)) g_qpcFreq = f.QuadPart;
    }
    g_abSubject = AbTest::IsSubject("SkyTextureReuse", &g_abSubject);
    SamplingProfiler::RegisterSelfSymbol("SkyTextureReuse", (const void*)&Detour);

    Log("[SkyTextureReuse] ACTIVE on the cloud texture build (sub_7EFD00 @ 0x%08X), "
        "10.29%% of executing time in an uncapped tester session. The texture is "
        "rebuilt a few rows a frame from a phase that only changes when a whole "
        "cycle has gone by, so a cycle that repeats its phase rebuilds bytes that "
        "are already in the buffer. Whether that happens depends on the cloud speed "
        "and the frame rate, so it is measured: while learning nothing is skipped, "
        "and the report says how many passes could have been. Arming needs %lu "
        "rebuilds that came out byte for byte identical and %lu bookkeeping "
        "predictions the client agreed with, and one pass in %lu is rechecked after.",
        (unsigned)kTarget, kProveRepeats, kProveBooks, kResampleMask + 1);
    if (g_abSubject)
        Log("[SkyTextureReuse]   under A/B test: the control half runs the client's "
            "function through the same hook.");
    return true;
}

void Shutdown() {
    if (!g_installed) return;
    MH_DisableHook((void*)kTarget);
    g_installed = false;
}

void LogStats() {
    if (!Config::g_settings.OptSkyTextureReuse &&
        !Config::g_settings.OptSkyCloudTexels) return;
    if (!g_installed) {
        Log("[SkyTextureReuse] not installed - the reason is at the top of this log");
        return;
    }
    if (g_calls == 0) {
        Log("[SkyTextureReuse] hooked, and no cloud texture pass has been reached "
            "yet. That is a measurement: no sky has been drawn since it went in.");
        return;
    }
    if (g_buildTimed && g_qpcFreq > 0) {
        const double totalMs = 1000.0 * (double)g_buildTicks / (double)g_qpcFreq;
        const double perPass = totalMs / (double)g_buildTimed;
        Log("[SkyTextureReuse] the client's own build took %.1f ms over %llu timed "
            "pass(es), %.3f ms each. A pass runs at most once a frame, so that per-pass "
            "figure is what moving this build off the main thread could be worth here; "
            "read it against the frame time in the FrameBench report.",
            totalMs, g_buildTimed, perPass);
        if (g_skipped)
            Log("[SkyTextureReuse]   the %llu skipped pass(es) are worth about %.1f ms "
                "at that rate, which is an estimate from the timed passes and not a "
                "measurement of the skips themselves.",
                g_skipped, perPass * (double)g_skipped);
    } else if (g_buildTimed) {
        Log("[SkyTextureReuse] %llu build(s) ran but the performance counter frequency "
            "was not readable, so there is no millisecond figure.", g_buildTimed);
    } else {
        Log("[SkyTextureReuse] no build was timed, so this session says nothing about "
            "what the build costs.");
    }
    Log("[SkyTextureReuse] %llu pass(es): %llu built by the client, %llu skipped "
        "here. %llu full cycles, %llu of them changed the cloud phase. Plain "
        "counters, lower bounds.",
        g_calls, g_built, g_skipped, g_cycles, g_phaseChanges);
    if (g_mismatches) {
        Log("[SkyTextureReuse]   DISABLED after a disagreement with the client; the "
            "line that says which is earlier in this log.");
        return;
    }
    Log("[SkyTextureReuse]   %lu pass(es) arrived with the inputs of a pass that "
        "built the same rows before, and %lu of those rebuilt byte for byte "
        "identical bytes. %lu bookkeeping predictions the client agreed with.",
        g_repeatsSeen, g_repeatsProven, g_booksProven);
    if (g_repeatsSeen == 0) {
        Log("[SkyTextureReuse]   no pass ever repeated its inputs, so there is "
            "nothing here to skip on this machine. That is the measurement this was "
            "built to take: the clouds move faster than the texture cycles.");
    } else if (g_skipped == 0) {
        Log("[SkyTextureReuse]   still learning, so nothing has been skipped yet; "
            "every pass so far ran the client's own build.");
    }
    if (g_odd)
        Log("[SkyTextureReuse]   %llu pass(es) had a shape outside what this reads "
            "and went straight to the client.", g_odd);
    if (g_abSubject)
        Log("[SkyTextureReuse]   %llu pass(es) ran the client's build as the A/B "
            "control half.", g_control);
}

}  // namespace SkyTextureReuse
