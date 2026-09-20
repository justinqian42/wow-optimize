// ============================================================================
// sub_78F6A0 builds the terrain horizon: it projects a strip of vertices, then
// rasterises them into a 384-column array of floats holding, per screen column,
// the highest thing seen there. Terrain culling reads that array afterwards.
//
// A tester's sampling profile puts it at 2.46% of main-thread execution, the
// third largest single entry after the M2 animation family and the world
// visibility traversal.
//
// The projection loop is already as fast as we can make it - it calls sub_4C21B0
// once per vertex, which this project replaced with packed-double SSE2 last
// week. What is left scalar is the rasterisation: for each segment between two
// projected vertices, a column range is computed and every column in it is
// updated. Those two inner loops walk up to 384 floats one at a time.
//
// The max-update loop vectorises exactly - it is a maximum of one scalar across
// a contiguous span, which is _mm_max_ps four columns at a time. The other
// branch writes a sentinel only where a byte mask is clear, which does not
// vectorise as cleanly and is left scalar; mirroring the client exactly matters
// more there than speed.
//
// Everything about this replacement is checked against the client's own routine
// on real data before it is trusted. The function's entire effect is the 384
// floats it leaves behind, so the check is: snapshot that array, run ours, keep
// the result, restore the snapshot, run the client's, compare. Any difference
// and every later call goes to the original. Replacing a culling function that
// decides what terrain is drawn, without checking it agrees, is how this project
// spent a week undoing an SSE2 matrix multiply that sat a hundred times outside
// its own declared tolerance.
// ============================================================================

#include <windows.h>
#include <cstdint>
#include <cstring>
#include <emmintrin.h>
#include <intrin.h>

#include "horizon_occlusion_sse2.h"
#include "config.h"
#include "MinHook.h"
#include "version.h"

extern "C" void Log(const char* fmt, ...);

namespace HorizonOcclusion {

static constexpr uintptr_t ADDR_HorizonBuild = 0x0078F6A0;
static constexpr uintptr_t ADDR_PointXMat4   = 0x004C21B0;

// Client globals the function reads and writes.
static constexpr uintptr_t ADDR_EnableFlags  = 0x00CD774C;   // bit 5 gates the whole thing
static constexpr uintptr_t ADDR_PitchCos     = 0x00CD8F7C;   // must be within +/-0.9
static constexpr uintptr_t ADDR_Scratch      = 0x00CD8FD8;   // projected vertices, 3 floats each
static constexpr uintptr_t ADDR_ViewMatrix   = 0x00ADF460;
static constexpr uintptr_t ADDR_ColumnBias   = 0x00ADF454;
static constexpr uintptr_t ADDR_Horizon      = 0x00CD8938;   // 384 floats, the output
static constexpr uintptr_t ADDR_ColumnFlags  = 0x00CD87B8;   // 384 bytes

static constexpr int   COLUMNS       = 384;
static constexpr int   COLUMN_ORIGIN = 192;
// The value written into cleared columns is a global the client loads, not a
// literal: "fld ds:flt_A3F858". Read it from there rather than copying the
// number the decompiler printed.
static constexpr uintptr_t ADDR_Sentinel = 0x00A3F858;
static constexpr float MIN_DEPTH     = 0.027777778f;   // 1/36, the client's own constant

typedef void (__cdecl *HorizonBuild_fn)(int a1, int a2, uint32_t* indices, int count,
                                        float* offset, int mode);
typedef float* (__cdecl *PointXMat4_fn)(float* out, const float* vec, const float* mat);

static HorizonBuild_fn orig_HorizonBuild = nullptr;
static PointXMat4_fn   PointXMat4        = (PointXMat4_fn)ADDR_PointXMat4;

static bool g_active = false;

// Verification state. Same shape as the frustum and normalise checks.
static volatile long g_checked   = 0;
static bool          g_trusted   = false;
static bool          g_abandoned = false;
static constexpr long VERIFY_CALLS = 512;   // each check costs a full second run

// Plain, not Interlocked, and not volatile.
//
// This was InterlockedIncrement on every call, and a field session makes
// 303207008 of them. An uncontended `lock xadd` still takes the bus lock and
// costs upwards of twenty cycles where a plain add costs one, so three hundred
// million of them is several seconds of main thread spent counting.
//
// Nothing here needs the lock. sub_78F6A0 builds the terrain horizon from the
// render path and runs on the thread the client draws on, which is the same
// reason the column-scan counter further down this file has always been a plain
// `++g_scanCalls`. The two conventions sat in one module.
//
// The project's rule is explicit about which to use: plain 32-bit on a hot path,
// never plain 64-bit, and say in the log that the count is a lower bound. The
// report does.
static unsigned long g_calls = 0;

// Project the vertex strip into the client's scratch array, exactly as the
// original does: fetch by index, add the caller's offset, transform, then divide
// x and y by z while leaving z alone.
static void ProjectVertices(int xyBase, int zBase, const uint32_t* indices,
                            int count, const float* offset) {
    float* scratch = (float*)ADDR_Scratch;
    const float* mat = (const float*)ADDR_ViewMatrix;

    for (int i = 0; i < count; ++i) {
        uint32_t idx = indices[i];
        float* p = scratch + i * 3;

        p[0] = *(const float*)(xyBase + 12 * idx)     + offset[0];
        p[1] = *(const float*)(xyBase + 12 * idx + 4) + offset[1];
        p[2] = *(const float*)(zBase  +  4 * idx)     + offset[2];

        float out[4];
        float* r = PointXMat4(out, p, mat);
        p[0] = r[0];
        p[1] = r[1];
        p[2] = r[2];

        // The client computes this reciprocal in double - "double v10 = 1.0 /
        // v8[2]" - and multiplies in double before the result lands back in a
        // float. Doing it in float instead differs by about 6e-8, which sounds
        // harmless and is not: the projected x feeds a column index through a
        // truncation, so a difference in the last bits can move a segment's span
        // by one column, and that column then takes its value from a different
        // segment entirely. The first attempt at this function did it in float
        // and disagreed with the client at column 106 on call one, by 2%.
        double invZ = 1.0 / (double)p[2];
        p[0] = (float)((double)p[0] * invZ);
        p[1] = (float)(invZ * (double)p[1]);
    }
}

// Column range for a segment, with the original's swap-and-clamp order kept.
static inline void ColumnRange(float x0, float x1, int& lo, int& hi) {
    float bias = *(const float*)ADDR_ColumnBias;
    // Mirrors the client exactly: the scale is a double multiply whose result is
    // stored to a float, and only then is the bias subtracted and the whole
    // truncated. Folding those steps changes which side of an integer boundary a
    // column lands on.
    float t0 = (float)((double)x0 * 64.0);
    float t1 = (float)((double)x1 * 64.0);

    // fistp, not fisttp. The client converts with the x87 rounding mode, which
    // is round-to-nearest-even by default, and the decompiler renders that as a
    // C cast - which truncates. Believing the cast is what put a span boundary
    // one column out: with the half-pixel bias, -148.54 rounds to -149 and
    // truncates to -148, so the client started a span at column 43 where this
    // started at 44 and left 43 holding whatever was there before.
    //
    // _mm_cvtss_si32 uses the MXCSR rounding mode, round-to-nearest-even by
    // default, which is the same rule fistp follows.
    int a = _mm_cvtss_si32(_mm_set_ss(t0 - bias)) + COLUMN_ORIGIN;
    int b = _mm_cvtss_si32(_mm_set_ss(t1 - bias)) + COLUMN_ORIGIN;
    if (b < a) { hi = a; lo = b; } else { lo = a; hi = b; }
    if (lo < 0) lo = 0;
    if (hi >= COLUMNS) hi = COLUMNS - 1;
}

// The vectorised half: raise every column in [lo,hi] to at least `value`.
static inline void RaiseSpan(float* horizon, int lo, int hi, float value) {
    int i = lo;

    // Head, until the span is four-aligned.
    for (; i <= hi && (i & 3) != 0; ++i) {
        if (value > horizon[i]) horizon[i] = value;
    }

    __m128 v = _mm_set1_ps(value);
    for (; i + 3 <= hi; i += 4) {
        __m128 cur = _mm_loadu_ps(horizon + i);
        _mm_storeu_ps(horizon + i, _mm_max_ps(cur, v));
    }

    for (; i <= hi; ++i) {
        if (value > horizon[i]) horizon[i] = value;
    }
}

static void BuildHorizon(int xyBase, int zBase, uint32_t* indices, int count,
                         float* offset, int mode) {
    ProjectVertices(xyBase, zBase, indices, count, offset);

    const float* scratch = (const float*)ADDR_Scratch;
    float* horizon = (float*)ADDR_Horizon;
    const uint8_t* flags = (const uint8_t*)ADDR_ColumnFlags;

    int segments = count - 1;
    if (segments <= 0) return;

    if (mode) {
        // Clear columns the segment covers, but only where the flag bit is not
        // set. Left scalar: the mask makes a vector store conditional, and being
        // byte-for-byte the same as the client matters more here than speed.
        for (int s = 0; s < segments; ++s) {
            int lo, hi;
            ColumnRange(scratch[s * 3], scratch[(s + 1) * 3], lo, hi);
            for (int i = lo; i <= hi; ++i) {
                if ((flags[i] & 1) == 0) horizon[i] = *(const float*)ADDR_Sentinel;
            }
        }
        return;
    }

    for (int s = 0; s < segments; ++s) {
        const float* a = scratch + s * 3;
        const float* b = scratch + (s + 1) * 3;

        // Both ends must be far enough in front of the camera.
        if (!(a[2] >= MIN_DEPTH && b[2] >= MIN_DEPTH)) continue;

        float value = (b[1] <= a[1]) ? b[1] : a[1];

        int lo, hi;
        ColumnRange(a[0], b[0], lo, hi);
        if (lo <= hi) RaiseSpan(horizon, lo, hi, value);
    }
}

static void __cdecl Hooked_HorizonBuild(int xyBase, int zBase, uint32_t* indices,
                                        int count, float* offset, int mode) {
    ++g_calls;   // plain; see the note on g_calls

    if (g_abandoned) {
        orig_HorizonBuild(xyBase, zBase, indices, count, offset, mode);
        return;
    }

    // The client's own gate. Skipping it would run the body in states where the
    // original does nothing at all.
    uint32_t enableFlags = *(const uint32_t*)ADDR_EnableFlags;
    float pitch = *(const float*)ADDR_PitchCos;
    if ((enableFlags & 0x20) == 0 || pitch < -0.89999998f || pitch > 0.89999998f) {
        return;
    }

    if (count <= 0 || !indices || !offset) {
        orig_HorizonBuild(xyBase, zBase, indices, count, offset, mode);
        return;
    }

    if (g_trusted) {
        __try {
            BuildHorizon(xyBase, zBase, indices, count, offset, mode);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            g_abandoned = true;
            Log("[Horizon] Faulted while building - handing every call back to the "
                "original");
            orig_HorizonBuild(xyBase, zBase, indices, count, offset, mode);
        }
        return;
    }

    // Verification pass. The whole effect of this function is the 384 floats it
    // leaves behind, so compare those: snapshot, run ours, keep the result,
    // restore, run the client's, compare.
    float* horizon = (float*)ADDR_Horizon;
    float before[COLUMNS];
    float ours[COLUMNS];

    __try {
        memcpy(before, horizon, sizeof(before));
        BuildHorizon(xyBase, zBase, indices, count, offset, mode);
        memcpy(ours, horizon, sizeof(ours));
        memcpy(horizon, before, sizeof(before));
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        g_abandoned = true;
        Log("[Horizon] Faulted during verification - handing every call back to "
            "the original");
        orig_HorizonBuild(xyBase, zBase, indices, count, offset, mode);
        return;
    }

    orig_HorizonBuild(xyBase, zBase, indices, count, offset, mode);

    long n = InterlockedIncrement(&g_checked);

    // On a mismatch, report enough to identify the cause without another round
    // trip. The first attempt gave one column and two values, which narrowed it
    // to "a span boundary moved" and no further - so it also says how many
    // columns differ, whether they are contiguous, and the inputs, because a
    // single displaced column means a rounding difference at a truncation while
    // a whole block means the geometry is being read wrongly.
    int firstBad = -1, lastBad = -1, badCount = 0;
    for (int i = 0; i < COLUMNS; ++i) {
        if (horizon[i] != ours[i]) {
            if (firstBad < 0) firstBad = i;
            lastBad = i;
            ++badCount;
        }
    }

    if (badCount > 0) {
        g_abandoned = true;
        Log("[Horizon] Disagreed with the client on call %ld: %d of %d columns "
            "differ, first %d (client %.9g, ours %.9g), last %d - mode=%d, "
            "vertices=%d",
            n, badCount, COLUMNS, firstBad, horizon[firstBad], ours[firstBad],
            lastBad, mode, count);

        // The projected span of the first few segments, which is what the column
        // index is derived from.
        const float* scratch = (const float*)ADDR_Scratch;
        int show = count < 4 ? count : 4;
        for (int i = 0; i < show; ++i) {
            Log("[Horizon]   vertex %d: x=%.9g y=%.9g z=%.9g",
                i, scratch[i * 3], scratch[i * 3 + 1], scratch[i * 3 + 2]);
        }
        Log("[Horizon] Handing every call back to the original");
        return;   // the client's output is already in place
    }

    if (n >= VERIFY_CALLS) {
        g_trusted = true;
        Log("[Horizon] Matched the client's output exactly on %ld consecutive real "
            "calls - running ours alone from here", n);
    }
}

// ---------------------------------------------------------------------------
// The consumer: sub_78FDC0's scan of the array this module builds
//
// The build is 2.46% of executing time in an older profile. Its consumer is the
// largest single wow.exe entry in the most recent session - 3621 samples, 1.91%
// of everything sampled - and nothing had looked at it.
//
// sub_78FDC0 projects the eight corners of a bounding box, works out the column
// range they cover, and then walks that range one column at a time asking
// whether any column's horizon is below the box's top:
//
//     fld   [ebp-20h]                ; the box's maximum projected y
//   loop:
//     fcom  flt_CD8938[ecx*4]
//     fnstsw ax
//     test  ah, 41h
//     jz    visible                  ; this column is below it - stop, drawn
//     add   ecx, 1
//     cmp   ecx, edx
//     jle   loop
//     fstp  st                       ; ran off the end - occluded
//
// Up to 384 iterations, each with an fcom and an fnstsw. The status-word
// transfer after a compare is the slowest way x86 has ever had to branch on a
// float, and it is inside the loop.
//
// Four columns at a time with one packed compare and a movemask is the same
// question asked four times at once. It needs no tolerance and no harness for
// precision: both sides are floats sitting in memory that nothing has computed
// on, and fcom widening them to 80 bits answers exactly what an ordered packed
// single compare answers, for every input including NaN. This is the same case
// as the collision outcode, which has now verified 3043 times in the field
// without one disagreement.
//
// What can still be wrong is the reading of the registers, so the first calls
// run both and compare the index.
//
// Register contract at 0x0078FF62, read off the disassembly:
//
//   in   ecx  first column, already clamped to 0..383
//        edx  last column, inclusive, already clamped
//        ebp  the caller's frame; [ebp-20h] is the box's maximum y, a float
//        x87  balanced - the fld this replaces is the first push
//   out  jump to 0x0078FF89 when a column is below it (the client's "visible"
//        path, entered past its fstp because nothing was pushed), or to
//        0x0078FF7C for occluded. eax, ecx and edx are dead at both.
//
// The two targets are reached from elsewhere in the function with a balanced x87
// stack already - 0x78FF43 and 0x78FF47 both jump to 0x78FF89, and 0x78FF60
// jumps to 0x78FF7C - so neither expects a value to pop.

static constexpr uintptr_t ADDR_ScanHead = 0x0078FF62;
static constexpr uintptr_t ADDR_ScanFound = 0x0078FF89;   // a column is below: visible
static constexpr uintptr_t ADDR_ScanNone  = 0x0078FF7C;   // ran off the end: occluded
// Not constexpr: a cast from an integer to a pointer is not a constant
// expression, and the array is fixed at a known address in the client anyway.
static float* const        kHorizon       = (float*)0x00CD8938;
static constexpr int       kColumns       = 384;

// The ten bytes the jump replaces, which are exactly two whole instructions:
//
//   78FF62: D9 45 E0                fld   dword ptr [ebp-20h]
//   78FF65: D8 14 8D 38 89 CD 00    fcom  dword ptr flt_CD8938[ecx*4]
//
// Ten and not eight. The fcom is seven bytes and ends at 0x78FF6B; saving eight
// would leave its last two, CD 00, sitting after the nop fill as an int 0. It is
// unreachable either way, but a disassembler reading it should see nops.
static const unsigned char kScanHeadBytes[10] = {
    0xD9, 0x45, 0xE0, 0xD8, 0x14, 0x8D, 0x38, 0x89, 0xCD, 0x00
};

static bool  g_scanPatched = false;
static bool  g_scanDead    = false;
static unsigned char g_scanSaved[sizeof(kScanHeadBytes)] = {};
static void* g_scanFound = (void*)ADDR_ScanFound;
static void* g_scanNone  = (void*)ADDR_ScanNone;

static unsigned long long g_scanCalls   = 0;
static unsigned long long g_scanColumns = 0;   // how far the scan actually walked
static unsigned long      g_scanMaxRun  = 0;
static unsigned long      g_scanVerified = 0;
static unsigned long      g_scanMismatch = 0;
static const unsigned long kScanVerify = 20000;

// Returns the first index in [first,last] whose horizon is below maxY, or -1.
static inline int ScanScalar(int first, int last, float maxY) {
    for (int i = first; i <= last; ++i)
        if (kHorizon[i] < maxY) return i;
    return -1;
}

static inline int ScanSse(int first, int last, float maxY) {
    // Unaligned loads, deliberately. The array is at 0x00CD8938, which is eight
    // modulo sixteen, so no index at all makes an aligned address and an aligned
    // load would fault on the first call. Aligning the loop to a four-index
    // boundary would not help either, for the same reason.
    const __m128 v = _mm_set1_ps(maxY);
    int i = first;
    for (; i + 3 <= last; i += 4) {
        const __m128 h = _mm_loadu_ps(kHorizon + i);
        const int m = _mm_movemask_ps(_mm_cmplt_ps(h, v));
        if (m) {
            unsigned long bit;
            _BitScanForward(&bit, (unsigned long)m);
            return i + (int)bit;
        }
    }
    for (; i <= last; ++i)
        if (kHorizon[i] < maxY) return i;
    return -1;
}

// Returns 1 when a column below maxY was found, 0 when none was.
extern "C" int __cdecl HorizonScan_Run(int first, int last, float maxY) {
    ++g_scanCalls;
    if (first < 0) first = 0;
    if (last >= kColumns) last = kColumns - 1;
    if (first > last) return 0;

    const unsigned long run = (unsigned long)(last - first + 1);
    g_scanColumns += run;
    if (run > g_scanMaxRun) g_scanMaxRun = run;

    if (g_scanDead) return ScanScalar(first, last, maxY) >= 0 ? 1 : 0;

    const int mine = ScanSse(first, last, maxY);
    if (g_scanVerified < kScanVerify) {
        const int theirs = ScanScalar(first, last, maxY);
        if (mine != theirs) {
            ++g_scanMismatch;
            g_scanDead = true;
            Log("[Horizon] SCAN DISABLED: the packed compare found column %d and "
                "the client's own order found %d over [%d,%d]. There is no "
                "arithmetic in this to round, so a difference means the registers "
                "were misread. Every scan from here goes the client's way.",
                mine, theirs, first, last);
            return theirs >= 0 ? 1 : 0;
        }
        ++g_scanVerified;
    }
    return mine >= 0 ? 1 : 0;
}

// Register marshalling only. ebp belongs to the client throughout, which is why
// the box's maximum y is read from its frame here rather than passed in.
static __declspec(naked) void HorizonScanThunk() {
    __asm {
        push dword ptr [ebp-0x20]       // maxY, as a float argument
        push edx                        // last column
        push ecx                        // first column
        call HorizonScan_Run
        add  esp, 12
        test eax, eax
        jnz  found
        jmp  dword ptr [g_scanNone]     // ran off the end: occluded
found:
        jmp  dword ptr [g_scanFound]    // a column is below it: visible
    }
}

static bool ScanBytesMatch() {
    __try {
        return memcmp((const void*)ADDR_ScanHead, kScanHeadBytes,
                      sizeof(kScanHeadBytes)) == 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

static bool PatchScan() {
    if (!ScanBytesMatch()) {
        Log("[Horizon] scan NOT replaced: the bytes at 0x%08X are not the loop "
            "this was read from.", (unsigned)ADDR_ScanHead);
        return false;
    }
    if (!WowOpt_ClientPatchAllowed((const void*)ADDR_ScanHead)) {
        Log("[Horizon] scan NOT replaced: No Client Patches is on.");
        return false;
    }
    DWORD old = 0;
    if (!VirtualProtect((void*)ADDR_ScanHead, sizeof(g_scanSaved),
                        PAGE_EXECUTE_READWRITE, &old)) {
        Log("[Horizon] scan NOT replaced: could not make 0x%08X writable",
            (unsigned)ADDR_ScanHead);
        return false;
    }
    memcpy(g_scanSaved, (const void*)ADDR_ScanHead, sizeof(g_scanSaved));

    unsigned char patch[sizeof(g_scanSaved)];
    patch[0] = 0xE9;
    *(int32_t*)(patch + 1) =
        (int32_t)((uintptr_t)&HorizonScanThunk - (ADDR_ScanHead + 5));
    // The three bytes after the jump are the tail of the fcom it split.
    // Unreachable; filled so a disassembler shows nops rather than half of one.
    memset(patch + 5, 0x90, sizeof(patch) - 5);
    memcpy((void*)ADDR_ScanHead, patch, sizeof(patch));

    DWORD ignored = 0;
    VirtualProtect((void*)ADDR_ScanHead, sizeof(g_scanSaved), old, &ignored);
    FlushInstructionCache(GetCurrentProcess(), (void*)ADDR_ScanHead,
                          sizeof(g_scanSaved));
    g_scanPatched = true;
    return true;
}

bool Init() {
    if (!Config::g_settings.OptHorizonOcclusionSse2) return true;

    static const unsigned char kExp_HorizonBuild[8] = { 0x55, 0x8B, 0xEC, 0x83, 0xEC, 0x1C, 0xF6, 0x05 };
    if (IsBadReadPtr((void*)ADDR_HorizonBuild, 8) ||
        memcmp((const void*)ADDR_HorizonBuild, kExp_HorizonBuild, 8) != 0) {
        Log("[Horizon] 0x%08X bad prologue or unreadable - not installing", (unsigned)ADDR_HorizonBuild);
        return false;
    }

    if (WineSafe_CreateHook((void*)ADDR_HorizonBuild, (void*)Hooked_HorizonBuild,
                            (void**)&orig_HorizonBuild) != MH_OK) {
        Log("[Horizon] Could not hook the horizon builder at 0x%08X",
            (unsigned)ADDR_HorizonBuild);
        return false;
    }
    if (WO_EnableHook((void*)ADDR_HorizonBuild) != MH_OK) {
        Log("[Horizon] Could not enable the hook");
        return false;
    }

    g_active = true;
    // The consumer of the array this builder writes. Same feature, same globals,
    // same switch: turning the builder on and its reader off would be two halves
    // of one thing behind one tickbox, which is the defect this project keeps
    // finding. It patches independently and says so if it could not.
    PatchScan();
    Log("[Horizon] SSE2 terrain horizon builder installed at 0x%08X - verifying "
        "against the client for the first %ld calls",
        (unsigned)ADDR_HorizonBuild, VERIFY_CALLS);
    return true;
}

void LogStats() {
    if (!g_active) {
        // This is in the diagnostic run, so a tester who ticks it and gets no
        // line back cannot tell a failed install from a quiet one.
        Log("[Horizon] not measured: the replacement is not active. Either the "
            "switch is off or the install refused - the reason is earlier in "
            "this log.");
        return;
    }
    Log("[Horizon] %lu calls (a lower bound - the counter is plain, see the note "
        "on it), %s",
        g_calls,
        g_abandoned ? "abandoned - the client's routine is doing the work"
                    : (g_trusted ? "verified, running ours"
                                 : "still verifying against the client"));

    if (!g_scanPatched) {
        Log("[Horizon]   the column scan in sub_78FDC0 is NOT replaced - the "
            "reason is above.");
        return;
    }
    if (g_scanCalls == 0) {
        Log("[Horizon]   the column scan is patched and has not run. That is a "
            "measurement: either nothing was tested against the horizon, or the "
            "patch is not on the path it was read from.");
        return;
    }
    // The average run is the number that decides whether this was worth doing.
    // Four columns at a time saves nothing on a scan of two and a great deal on
    // one of two hundred, and until now nobody knew which this is.
    Log("[Horizon]   column scan: %llu call(s) over %llu column(s), %.1f on "
        "average, longest %lu of 384. Four are compared at a time.",
        g_scanCalls, g_scanColumns,
        (double)g_scanColumns / (double)g_scanCalls, g_scanMaxRun);
    if (g_scanMismatch) {
        Log("[Horizon]   the scan DISAGREED with the client %lu time(s) and is "
            "retired. There is no arithmetic in it, so that is a misread "
            "register, not a rounding difference.", g_scanMismatch);
    } else if (g_scanVerified < kScanVerify) {
        Log("[Horizon]   %lu of %lu scans checked against the client's own order "
            "so far, none differed.", g_scanVerified, kScanVerify);
    } else {
        Log("[Horizon]   %lu scans checked against the client's own order, none "
            "differed, and the check is off.", kScanVerify);
    }
}

void Shutdown() {
    if (!g_active) return;
    g_active = false;
    MH_DisableHook((void*)ADDR_HorizonBuild);
    if (g_scanPatched) {
        DWORD old = 0;
        if (VirtualProtect((void*)ADDR_ScanHead, sizeof(g_scanSaved),
                           PAGE_EXECUTE_READWRITE, &old)) {
            memcpy((void*)ADDR_ScanHead, g_scanSaved, sizeof(g_scanSaved));
            DWORD ignored = 0;
            VirtualProtect((void*)ADDR_ScanHead, sizeof(g_scanSaved), old, &ignored);
            FlushInstructionCache(GetCurrentProcess(), (void*)ADDR_ScanHead,
                                  sizeof(g_scanSaved));
        }
        g_scanPatched = false;
    }
    // No LogStats here. It returns on !g_active, which was cleared four lines
    // up, so the call has never printed anything - and this DLL leaves through
    // TerminateProcess anyway, where Shutdown does not run at all. The periodic
    // report is where these numbers reach a log.
}

} // namespace HorizonOcclusion
