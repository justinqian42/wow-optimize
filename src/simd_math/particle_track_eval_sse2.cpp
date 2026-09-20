// ============================================================================
// Module: particle_track_eval_sse2.cpp
//
// The per-particle track evaluation, sub_979D60. In a tester's 2026-09-19
// profile it is 1.75% of executing main-thread time, and its only caller,
// sub_97BE80, is another 2.09% - the particle update is the whole of that
// family. This one is a leaf: 94 instructions, no calls, pure arithmetic.
//
// It takes the particle's age over its lifetime, turns that into a position
// along a two-segment track, and evaluates seven values from the track entry:
// four colour bytes, one float written to two slots, and two integers.
//
//   this+0A4  lifetime           this+118  where the track splits
//   this+11C  the track entry    this+130  a scale on the alpha
//   entry+00..03  four byte bases, entry+04,08,0C,10  four integer slopes
//   entry+14,18   float base and slope, entry+1C,20 and +24,28  integer pairs
//
// Reading it from the disassembly rather than the decompiler matters twice.
//
// First, the seven `fistp` instructions have no `fldcw` in front of them. The
// decompiler prints them as C casts, which truncate; the client leaves the
// control word alone, so they round to nearest even. The replacement uses
// cvtss2si, which is the same rounding, and not cvttss2si.
//
// Second, the position along the track is kept twice: `fst [ebp+arg_0]` stores
// it as a float and keeps the full value in st(0). The first of the seven
// products uses the value still in the register, at 53-bit precision, and the
// six after it reload the float. Using one of them for all seven would change
// the answer.
//
// Width. The client's x87 runs at 53-bit precision here, so every operation is
// a double rounded once, and each result is stored to a float before the
// convert - both reproduced exactly. Every operand is a float, a byte or a
// 32-bit integer widened exactly. The one division is a double division of two
// exactly-widened values. So this is bit-exact by construction rather than by
// measurement, and the verification below checks that on live data anyway.
//
// NaN. The two comparisons are ordered ones whose unordered case the client
// sends down a particular branch - the lifetime clamp keeps 0.001 and the split
// test takes the far segment - and the replacement takes the same branch. An
// out-of-range or NaN convert gives 0x80000000 from fistp and from cvtss2si
// alike.
//
// Verification, predict-then-compare. The function writes through four
// pointers. While learning, the answer is worked out into locals, the client
// runs and writes the real ones, and the two are compared byte for byte. The
// first difference retires this for the session.
// ============================================================================

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <cstdint>
#include <cstring>
#include <emmintrin.h>

#include "particle_track_eval_sse2.h"
#include "MinHook.h"
#include "version.h"
#include "config.h"
#include "ab_test.h"
#include "session_verdict.h"
#include "sampling_profiler.h"

extern "C" void Log(const char* fmt, ...);
MH_STATUS WineSafe_CreateHook(void* target, void* detour, void** original);
MH_STATUS WO_EnableHook(void* target);

namespace ParticleTrackEval {

namespace {

constexpr uintptr_t kTarget   = 0x00979D60;
constexpr uintptr_t kMinLife  = 0x009E1134;   // 0.001f, read from the client

// push ebp / mov ebp,esp / fld ds:flt_9E1134 / sub esp,8 / fcom [ecx+0A4h]
const unsigned char kPrologue[16] = {
    0x55, 0x8B, 0xEC, 0xD9, 0x05, 0x34, 0x11, 0x9E,
    0x00, 0x83, 0xEC, 0x08, 0xD8, 0x91, 0xA4, 0x00
};

constexpr unsigned kF_lifetime = 0x0A4;
constexpr unsigned kF_split    = 0x118;
constexpr unsigned kF_track    = 0x11C;
constexpr unsigned kF_alphaMul = 0x130;

constexpr unsigned long kLearnCalls   = 20000;
constexpr unsigned long kResampleMask = 4095;

typedef uint32_t* (__fastcall* EvalFn)(void* self, void* edx, const float* age,
                                       uint8_t* colour, float* size,
                                       uint32_t* outA, uint32_t* outB);
EvalFn g_orig = nullptr;

bool g_installed = false;
bool g_dead = false;
bool g_abSubject = false;

unsigned long long g_calls = 0;
unsigned long long g_answered = 0;
unsigned long long g_control = 0;
unsigned long g_verified = 0;
unsigned g_mismatches = 0;

struct Out {
    uint8_t  colour[4];
    float    size;
    uint32_t a;
    uint32_t b;
};

// fistp with the control word untouched: round to nearest, ties to even, and
// 0x80000000 for anything it cannot represent. cvtss2si is the same.
__forceinline int32_t RoundToInt(float v) {
    return _mm_cvtss_si32(_mm_set_ss(v));
}

__forceinline void Evaluate(const void* self, const float* age, Out* out) {
    const char* s = (const char*)self;

    double life = *(const float*)kMinLife;
    const double own = *(const float*)(s + kF_lifetime);
    if (life < own) life = own;                       // NaN keeps the minimum

    const double pos = (double)*age / life;
    const double split = *(const float*)(s + kF_split);

    const char* e = *(const char* const*)(s + kF_track);
    double along;
    if (pos < split) {
        along = pos / split;
    } else {                                          // NaN lands here too
        e += 0x2C;
        along = (pos - split) / (1.0 - split);
    }
    // Kept both ways on purpose: the first product uses the value still in the
    // register, the six after it use the float the client stored.
    const float alongF = (float)along;
    const double alongD = (double)alongF;

    const double alphaMul = *(const float*)(s + kF_alphaMul);
    const float c3 = (float)((along * (double)*(const int32_t*)(e + 0x10)
                              + (double)(uint8_t)e[3]) * alphaMul);
    out->colour[3] = (uint8_t)RoundToInt(c3);

    const float c2 = (float)((double)*(const int32_t*)(e + 0x04) * alongD
                             + (double)(uint8_t)e[2]);
    out->colour[2] = (uint8_t)RoundToInt(c2);

    const float c1 = (float)((double)*(const int32_t*)(e + 0x08) * alongD
                             + (double)(uint8_t)e[1]);
    out->colour[1] = (uint8_t)RoundToInt(c1);

    const float c0 = (float)((double)*(const int32_t*)(e + 0x0C) * alongD
                             + (double)(uint8_t)e[0]);
    out->colour[0] = (uint8_t)RoundToInt(c0);

    out->size = (float)((double)*(const float*)(e + 0x18) * alongD
                        + (double)*(const float*)(e + 0x14));

    const float fa = (float)(alongD * (double)*(const int32_t*)(e + 0x20)
                             + (double)*(const int32_t*)(e + 0x1C));
    out->a = (uint32_t)RoundToInt(fa);

    const float fb = (float)((double)*(const int32_t*)(e + 0x28) * alongD
                             + (double)*(const int32_t*)(e + 0x24));
    out->b = (uint32_t)RoundToInt(fb);
}

__declspec(noinline) void Retire(const Out& mine, const Out& theirs) {
    ++g_mismatches;
    g_dead = true;
    Log("[ParticleTrackEval] RETIRED: the client wrote colour %02X%02X%02X%02X, "
        "size %.9g, %u and %u; this worked out %02X%02X%02X%02X, %.9g, %u and %u. "
        "Every evaluation from here on is the client's own.",
        theirs.colour[0], theirs.colour[1], theirs.colour[2], theirs.colour[3],
        theirs.size, theirs.a, theirs.b,
        mine.colour[0], mine.colour[1], mine.colour[2], mine.colour[3],
        mine.size, mine.a, mine.b);
    Verdict::Add(Verdict::Bad, "ParticleTrackEval evaluated a particle track "
                 "differently from the client and retired itself for this session");
}

uint32_t* __fastcall Detour(void* self, void* edx, const float* age,
                            uint8_t* colour, float* size,
                            uint32_t* outA, uint32_t* outB) {
    ++g_calls;
    if (g_dead || !self || !age || !colour || !size || !outA || !outB)
        return g_orig(self, edx, age, colour, size, outA, outB);
    if (g_abSubject && AbTest::StandAside()) {
        ++g_control;
        return g_orig(self, edx, age, colour, size, outA, outB);
    }

    const bool learning = (g_verified < kLearnCalls) ||
                          ((unsigned long)g_calls & kResampleMask) == 0;
    if (!learning) {
        Out mine;
        Evaluate(self, age, &mine);
        memcpy(colour, mine.colour, 4);
        size[0] = mine.size;
        size[1] = mine.size;
        *outA = mine.a;
        *outB = mine.b;
        ++g_answered;
        return outB;
    }

    Out mine;
    Evaluate(self, age, &mine);
    uint32_t* ret = g_orig(self, edx, age, colour, size, outA, outB);

    Out theirs;
    memcpy(theirs.colour, colour, 4);
    theirs.size = size[0];
    theirs.a = *outA;
    theirs.b = *outB;
    uint32_t mineSize, theirsSize;
    memcpy(&mineSize, &mine.size, 4);
    memcpy(&theirsSize, &theirs.size, 4);
    if (memcmp(mine.colour, theirs.colour, 4) != 0 || mineSize != theirsSize ||
        mine.a != theirs.a || mine.b != theirs.b ||
        memcmp(&size[0], &size[1], 4) != 0) {
        Retire(mine, theirs);
        return ret;
    }
    ++g_verified;
    return ret;
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
    if (!Config::g_settings.OptParticleTrackEval) return true;

    if (!BytesMatch(kTarget, kPrologue, sizeof(kPrologue))) {
        Log("[ParticleTrackEval] NOT active: the bytes at 0x%08X are not the particle "
            "track evaluation this was read from, so nothing was hooked.",
            (unsigned)kTarget);
        return false;
    }
    if (!WowOpt_ClientPatchAllowed((const void*)kTarget)) {
        Log("[ParticleTrackEval] NOT active: No Client Patches is on, and this hooks "
            "a function inside wow.exe.");
        return false;
    }
    if (WineSafe_CreateHook((void*)kTarget, (void*)&Detour, (void**)&g_orig) != MH_OK) {
        Log("[ParticleTrackEval] NOT active: the hook on 0x%08X could not be created.",
            (unsigned)kTarget);
        return false;
    }
    if (WO_EnableHook((void*)kTarget) != MH_OK) {
        MH_RemoveHook((void*)kTarget);
        Log("[ParticleTrackEval] NOT active: the hook on 0x%08X could not be enabled.",
            (unsigned)kTarget);
        return false;
    }
    g_installed = true;
    g_abSubject = AbTest::IsSubject("ParticleTrackEval", &g_abSubject);
    SamplingProfiler::RegisterSelfSymbol("ParticleTrackEval", (const void*)&Detour);

    Log("[ParticleTrackEval] ACTIVE on the per-particle track evaluation (sub_979D60 "
        "@ 0x%08X), 1.75%% of executing time in an uncapped tester session. Seven "
        "values a particle, each an x87 multiply-add that goes out to a float on the "
        "stack and comes back before it is converted. The same arithmetic in SSE2 "
        "doubles, with the client's rounding: its converts leave the control word "
        "alone, so they round to nearest rather than truncate. The first %lu calls "
        "are answered by the client and compared byte for byte, then one in %lu.",
        (unsigned)kTarget, kLearnCalls, kResampleMask + 1);
    if (g_abSubject)
        Log("[ParticleTrackEval]   under A/B test: the control half runs the client's "
            "function through the same hook.");
    return true;
}

void Shutdown() {
    if (!g_installed) return;
    MH_DisableHook((void*)kTarget);
    g_installed = false;
}

void LogStats() {
    if (!Config::g_settings.OptParticleTrackEval) return;
    if (!g_installed) {
        Log("[ParticleTrackEval] not installed - the reason is at the top of this log");
        return;
    }
    if (g_calls == 0) {
        Log("[ParticleTrackEval] hooked, and no particle has been evaluated yet. That "
            "is a measurement: no emitter has run since it went in.");
        return;
    }
    Log("[ParticleTrackEval] %llu call(s), %llu answered here. Plain counters, lower "
        "bounds.", g_calls, g_answered);
    if (g_mismatches) {
        Log("[ParticleTrackEval]   DISABLED after a difference from the client's own "
            "output; the line that says which is earlier in this log.");
    } else if (g_verified < kLearnCalls) {
        Log("[ParticleTrackEval]   %lu of %lu calls compared with the client so far, "
            "none differed; every call is still the client's own.",
            g_verified, kLearnCalls);
    } else {
        Log("[ParticleTrackEval]   %lu calls compared with the client byte for byte, "
            "none differed; one in %lu is still compared.",
            g_verified, kResampleMask + 1);
    }
    if (g_abSubject)
        Log("[ParticleTrackEval]   %llu call(s) ran the client's function as the A/B "
            "control half.", g_control);
}

}  // namespace ParticleTrackEval
