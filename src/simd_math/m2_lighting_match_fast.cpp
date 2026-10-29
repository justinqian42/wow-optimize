// ============================================================================
// Module: m2_lighting_match_fast.cpp
//
// Hardware-accelerated M2 particle lighting equality comparison for WoW 3.3.5a:
//   - sub_81F800: CM2SceneRender::LightingColorsMatch(const CM2Lighting* a, const CM2Lighting* b)
//
// In stock 3.3.5a:
//   - sub_81F800 compares 6 single-precision floats representing ambient and
//     diffuse sunlight colors between two particle lighting configurations:
//       m_sunAmbient (x, y, z at offset +0x54, +0x58, +0x5C)
//       m_sunDiffuse (x, y, z at offset +0x60, +0x64, +0x68)
//     Each float difference is evaluated against an epsilon of 1/255 (0.0039215689f).
//   - In stock code, sub_81F800 is called in an inner loop for every candidate
//     particle emitter in CM2SceneRender::DrawBatchedParticle (sub_821100).
//   - The stock implementation uses serial x87 subtractions, fabs, fcomp,
//     multiple fnstsw pipeline stalls, conditional branches, and calls to sub_482870.
//
// This optimization implements:
//   1. Pointer equality fast-path: When two particle emitters share the same
//      lighting pointer (common for particles from the same model or global light),
//      returns match immediately with zero memory reads or comparisons.
//   2. SSE2 128-bit vectorization: Evaluates ambient and diffuse color channels
//      in parallel using SIMD vector subtraction, bitwise absolute value, and
//      simultaneous threshold comparison.
//   3. Dual-run verification and automatic fail-safe retirement guards ensure
//      strict parity with client output.
// ============================================================================

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <xmmintrin.h>
#include <emmintrin.h>
#include <cstdint>
#include <cstring>

#include "m2_lighting_match_fast.h"
#include "MinHook.h"
#include "version.h"
#include "config.h"
#include "sampling_profiler.h"

extern "C" void Log(const char* fmt, ...);

namespace M2LightingMatchFast {

namespace {

constexpr uintptr_t kTarget_LightingColorsMatch = 0x0081F800;

// Verification settings
constexpr unsigned long kVerifyFirst = 20000;
constexpr unsigned long kResampleMask = 4095;

typedef int (__cdecl* LightingColorsMatch_fn)(const void* a, const void* b);

LightingColorsMatch_fn orig_LightingColorsMatch = nullptr;

bool g_installed = false;
bool g_dead = false;

unsigned long long g_calls = 0;
unsigned long long g_verified = 0;
unsigned long long g_mismatches = 0;
unsigned long long g_ptr_equal_hits = 0;

__forceinline int Fast_LightingColorsMatch_Impl(const void* a, const void* b) {
    if (a == b) {
        return 1;
    }
    if (!a || !b) {
        return 0;
    }

    const __m128 sign_mask = _mm_castsi128_ps(_mm_set1_epi32(0x7FFFFFFF));
    const __m128 eps = _mm_set1_ps(0.0039215689f);

    // Vector 1: First 4 floats at +0x54 (sunAmbient.x, y, z and sunDiffuse.x)
    const __m128 va1 = _mm_loadu_ps(reinterpret_cast<const float*>(reinterpret_cast<const char*>(a) + 0x54));
    const __m128 vb1 = _mm_loadu_ps(reinterpret_cast<const float*>(reinterpret_cast<const char*>(b) + 0x54));
    const __m128 diff1 = _mm_sub_ps(va1, vb1);
    const __m128 abs1 = _mm_and_ps(diff1, sign_mask);
    const __m128 cmp1 = _mm_cmplt_ps(abs1, eps);

    if (_mm_movemask_ps(cmp1) != 0xF) {
        return 0;
    }

    // Vector 2: Remaining 2 floats at +0x64 (sunDiffuse.y, z)
    const __m128 va2 = _mm_castpd_ps(_mm_load_sd(reinterpret_cast<const double*>(reinterpret_cast<const char*>(a) + 0x64)));
    const __m128 vb2 = _mm_castpd_ps(_mm_load_sd(reinterpret_cast<const double*>(reinterpret_cast<const char*>(b) + 0x64)));
    const __m128 diff2 = _mm_sub_ps(va2, vb2);
    const __m128 abs2 = _mm_and_ps(diff2, sign_mask);
    const __m128 cmp2 = _mm_cmplt_ps(abs2, eps);

    return ((_mm_movemask_ps(cmp2) & 3) == 3) ? 1 : 0;
}

int __cdecl Hooked_LightingColorsMatch(const void* a, const void* b) {
    if (g_dead || !a || !b) {
        return orig_LightingColorsMatch(a, b);
    }

    if (a == b) {
        ++g_ptr_equal_hits;
        return 1;
    }

    const unsigned long long call_idx = ++g_calls;
    const int fast_res = Fast_LightingColorsMatch_Impl(a, b);

    if (call_idx <= kVerifyFirst || ((call_idx & kResampleMask) == 0)) {
        ++g_verified;
        const int orig_res = orig_LightingColorsMatch(a, b);

        if (fast_res != orig_res) {
            if (++g_mismatches == 1) {
                Log("[M2LightingMatchFast] MISMATCH at call %llu: fast=%d, orig=%d, retiring hook",
                    call_idx, fast_res, orig_res);
                g_dead = true;
            }
            return orig_res;
        }
    }

    return fast_res;
}

} // namespace

void Init() {
    if (!Config::g_settings.OptM2LightingMatchFast) {
        return;
    }

    static const unsigned char kExp_Prologue[8] = { 0x55, 0x8B, 0xEC, 0x56, 0x8B, 0x75, 0x08, 0xD9 };

    if (memcmp((const void*)kTarget_LightingColorsMatch, kExp_Prologue, 8) != 0) {
        Log("[M2LightingMatchFast] Prologue mismatch at 0x%08X, aborting installation", kTarget_LightingColorsMatch);
        return;
    }

    if (WineSafe_CreateHook((void*)kTarget_LightingColorsMatch, (void*)Hooked_LightingColorsMatch, (void**)&orig_LightingColorsMatch) != MH_OK) {
        Log("[M2LightingMatchFast] Failed to create hook");
        return;
    }

    if (WO_EnableHook((void*)kTarget_LightingColorsMatch) != MH_OK) {
        Log("[M2LightingMatchFast] Failed to enable hook");
        return;
    }

    SamplingProfiler::RegisterSelfSymbol("Fast_LightingColorsMatch", (const void*)&Hooked_LightingColorsMatch);

    g_installed = true;
    Log("[M2LightingMatchFast] ACTIVE on CM2SceneRender LightingColorsMatch (0x0081F800)");
}

void LogStats() {
    if (!Config::g_settings.OptM2LightingMatchFast || !g_installed) {
        return;
    }

    Log("[M2LightingMatchFast] Stats: calls=%llu (ptr_hits=%llu, verified=%llu, mismatches=%llu%s)",
        g_calls, g_ptr_equal_hits, g_verified, g_mismatches, g_dead ? " [RETIRED]" : "");
}

void Shutdown() {
    if (!g_installed) {
        return;
    }

    MH_DisableHook((void*)kTarget_LightingColorsMatch);
    g_installed = false;
}

} // namespace M2LightingMatchFast
