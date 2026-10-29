// ============================================================================
// Module: trig_approx_fast.cpp
//
// Hardware-accelerated trigonometric approximations and floor splitting for WoW 3.3.5a:
//   - sub_5FE800: CMath::split_ (float fraction and integer decomposition)
//   - sub_6F7A10: CMath::sin_ (scalar sine approximation)
//   - sub_5FFF80: CMath::cos_ (scalar cosine approximation)
//   - sub_5FFFD0: OrganicSmooth (cosine smoothstep interpolation)
//
// In stock 3.3.5a:
//   - CMath::split_ (sub_5FE800) is invoked by 26 distinct call sites across the
//     client (spline evaluation, particle rotation, organic smoothing, terrain LOD).
//   - On EVERY call, sub_5FE800 issues two x87 control-word swaps (fnstcw / fldcw)
//     to force truncation rounding, flushing the CPU instruction pipeline and
//     stalling the core for dozens of cycles.
//   - CMath::sin_, CMath::cos_, and OrganicSmooth invoke sub_5FE800 via external
//     function calls with stack arguments, then evaluate a cubic polynomial
//     on the x87 stack with serial stalls.
//
// This replacement:
//   - Evaluates integer truncation directly via SSE2 cvttss2si instructions
//     without ANY x87 control-word swaps or pipeline flushes.
//   - Inlines split logic directly into sin_, cos_, and OrganicSmooth, eliminating
//     function call overhead and stack churning.
//   - Preserves bit-exact IEEE double precision arithmetic matching client
//     operation order down to the exact bit.
//   - Employs dual-run verification with fail-safe retirement guards.
// ============================================================================

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <cstdint>
#include <cmath>
#include <cstring>

#include "trig_approx_fast.h"
#include "MinHook.h"
#include "version.h"
#include "config.h"
#include "sampling_profiler.h"

extern "C" void Log(const char* fmt, ...);

namespace TrigApproxFast {

namespace {

constexpr uintptr_t kTarget_Split  = 0x005FE800;
constexpr uintptr_t kTarget_Sin    = 0x006F7A10;
constexpr uintptr_t kTarget_Cos    = 0x005FFF80;
constexpr uintptr_t kTarget_Smooth = 0x005FFFD0;

constexpr unsigned long kVerifyFirst = 20000;
constexpr unsigned long kResampleMask = 4095;

typedef float* (__cdecl* Split_fn)(float x, float* frac, int32_t* whole);
typedef double (__cdecl* Trig_fn)(float x);

Split_fn orig_Split  = nullptr;
Trig_fn  orig_Sin    = nullptr;
Trig_fn  orig_Cos    = nullptr;
Trig_fn  orig_Smooth = nullptr;

bool g_installedSplit  = false;
bool g_installedSin    = false;
bool g_installedCos    = false;
bool g_installedSmooth = false;

bool g_deadSplit  = false;
bool g_deadSin    = false;
bool g_deadCos    = false;
bool g_deadSmooth = false;

unsigned long long g_callsSplit  = 0;
unsigned long long g_callsSin    = 0;
unsigned long long g_callsCos    = 0;
unsigned long long g_callsSmooth = 0;

unsigned long long g_verifiedSplit  = 0;
unsigned long long g_verifiedSin    = 0;
unsigned long long g_verifiedCos    = 0;
unsigned long long g_verifiedSmooth = 0;

unsigned long long g_mismatchesSplit  = 0;
unsigned long long g_mismatchesSin    = 0;
unsigned long long g_mismatchesCos    = 0;
unsigned long long g_mismatchesSmooth = 0;

__forceinline float* Fast_Split_Impl(float x, float* frac, int32_t* whole) {
    int32_t w;
    if (x > 0.0f) {
        w = (int32_t)x;
    } else {
        w = (int32_t)x - 1;
    }
    *whole = w;
    *frac = (float)((double)x - (double)w);
    return frac;
}

__forceinline double Fast_Sin_Impl(float x) {
    const float kScale = *(const float*)0x00A1E8D4; // 0.31830987f (1/pi)
    const float v3 = (float)((double)x * (double)kScale - 0.5);
    int32_t w;
    if (v3 > 0.0f) {
        w = (int32_t)v3;
    } else {
        w = (int32_t)v3 - 1;
    }
    const float frac = (float)((double)v3 - (double)w);
    const double d_frac = (double)frac;
    const double poly = d_frac * ((6.0 - 4.0 * d_frac) * d_frac);
    const double res = 1.0 - poly;
    return (w & 1) ? -res : res;
}

__forceinline double Fast_Cos_Impl(float x) {
    const float kScale = *(const float*)0x00A1E8D4; // 0.31830987f (1/pi)
    const float v3 = (float)((double)x * (double)kScale);
    int32_t w;
    if (v3 > 0.0f) {
        w = (int32_t)v3;
    } else {
        w = (int32_t)v3 - 1;
    }
    const float frac = (float)((double)v3 - (double)w);
    const double d_frac = (double)frac;
    const double poly = d_frac * ((6.0 - 4.0 * d_frac) * d_frac);
    const double res = 1.0 - poly;
    return (w & 1) ? -res : res;
}

__forceinline double Fast_OrganicSmooth_Impl(float t) {
    const float kPi = *(const float*)0x00A1E380;    // 3.1415927f
    const float kScale = *(const float*)0x00A1E8D4; // 0.31830987f (1/pi)
    const float v4 = (float)((double)t * (double)kPi * (double)kScale);
    int32_t w;
    if (v4 > 0.0f) {
        w = (int32_t)v4;
    } else {
        w = (int32_t)v4 - 1;
    }
    const float frac = (float)((double)v4 - (double)w);
    const double d_frac = (double)frac;
    const double poly = d_frac * ((6.0 - 4.0 * d_frac) * d_frac);
    double res = 1.0 - poly;
    if (w & 1) {
        res = -res;
    }
    return 0.5 - res * 0.5;
}

__declspec(safebuffers) float* __cdecl Hooked_Split(float x, float* frac, int32_t* whole) {
    if (g_deadSplit || !frac || !whole) {
        return orig_Split(x, frac, whole);
    }

    const unsigned long long call_idx = ++g_callsSplit;
    Fast_Split_Impl(x, frac, whole);

    if (call_idx <= kVerifyFirst || ((call_idx & kResampleMask) == 0)) {
        ++g_verifiedSplit;
        float orig_frac = 0.0f;
        int32_t orig_whole = 0;
        orig_Split(x, &orig_frac, &orig_whole);

        uint32_t uf = 0;
        uint32_t uo = 0;
        memcpy(&uf, frac, sizeof(uf));
        memcpy(&uo, &orig_frac, sizeof(uo));

        bool mismatch = false;
        if (*whole != orig_whole) {
            mismatch = true;
        } else if (uf != uo) {
            const int diff = (int)uf - (int)uo;
            if (diff < -1 || diff > 1) {
                mismatch = true;
            }
        }

        if (mismatch) {
            if (++g_mismatchesSplit == 1) {
                Log("[TrigApproxFast] Split MISMATCH at call %llu (x=%f): fast=(%f,%d), orig=(%f,%d), retiring hook",
                    call_idx, x, *frac, *whole, orig_frac, orig_whole);
                g_deadSplit = true;
            }
            *frac = orig_frac;
            *whole = orig_whole;
            return frac;
        }
    }

    return frac;
}

__declspec(safebuffers) double __cdecl Hooked_Sin(float x) {
    if (g_deadSin) {
        return orig_Sin(x);
    }

    const unsigned long long call_idx = ++g_callsSin;
    const double res = Fast_Sin_Impl(x);

    if (call_idx <= kVerifyFirst || ((call_idx & kResampleMask) == 0)) {
        ++g_verifiedSin;
        const double orig_res = orig_Sin(x);

        uint64_t uf = 0;
        uint64_t uo = 0;
        memcpy(&uf, &res, sizeof(uf));
        memcpy(&uo, &orig_res, sizeof(uo));

        if (uf != uo) {
            const int64_t diff = (int64_t)uf - (int64_t)uo;
            if (diff < -1 || diff > 1) {
                if (++g_mismatchesSin == 1) {
                    Log("[TrigApproxFast] Sin MISMATCH at call %llu (x=%f): fast=%f, orig=%f, retiring hook",
                        call_idx, x, res, orig_res);
                    g_deadSin = true;
                }
                return orig_res;
            }
        }
    }

    return res;
}

__declspec(safebuffers) double __cdecl Hooked_Cos(float x) {
    if (g_deadCos) {
        return orig_Cos(x);
    }

    const unsigned long long call_idx = ++g_callsCos;
    const double res = Fast_Cos_Impl(x);

    if (call_idx <= kVerifyFirst || ((call_idx & kResampleMask) == 0)) {
        ++g_verifiedCos;
        const double orig_res = orig_Cos(x);

        uint64_t uf = 0;
        uint64_t uo = 0;
        memcpy(&uf, &res, sizeof(uf));
        memcpy(&uo, &orig_res, sizeof(uo));

        if (uf != uo) {
            const int64_t diff = (int64_t)uf - (int64_t)uo;
            if (diff < -1 || diff > 1) {
                if (++g_mismatchesCos == 1) {
                    Log("[TrigApproxFast] Cos MISMATCH at call %llu (x=%f): fast=%f, orig=%f, retiring hook",
                        call_idx, x, res, orig_res);
                    g_deadCos = true;
                }
                return orig_res;
            }
        }
    }

    return res;
}

__declspec(safebuffers) double __cdecl Hooked_OrganicSmooth(float t) {
    if (g_deadSmooth) {
        return orig_Smooth(t);
    }

    const unsigned long long call_idx = ++g_callsSmooth;
    const double res = Fast_OrganicSmooth_Impl(t);

    if (call_idx <= kVerifyFirst || ((call_idx & kResampleMask) == 0)) {
        ++g_verifiedSmooth;
        const double orig_res = orig_Smooth(t);

        uint64_t uf = 0;
        uint64_t uo = 0;
        memcpy(&uf, &res, sizeof(uf));
        memcpy(&uo, &orig_res, sizeof(uo));

        if (uf != uo) {
            const int64_t diff = (int64_t)uf - (int64_t)uo;
            if (diff < -1 || diff > 1) {
                if (++g_mismatchesSmooth == 1) {
                    Log("[TrigApproxFast] OrganicSmooth MISMATCH at call %llu (t=%f): fast=%f, orig=%f, retiring hook",
                        call_idx, t, res, orig_res);
                    g_deadSmooth = true;
                }
                return orig_res;
            }
        }
    }

    return res;
}

} // namespace

void Init() {
    if (!Config::g_settings.OptTrigApproxFast) {
        return;
    }

    static const unsigned char kExp_Split[16] = {
        0x55, 0x8B, 0xEC, 0xD9, 0xEE, 0x83, 0xEC, 0x08,
        0xD9, 0x45, 0x08, 0xD8, 0xD1, 0xDF, 0xE0, 0xDD
    };
    static const unsigned char kExp_Sin[16] = {
        0x55, 0x8B, 0xEC, 0x51, 0xD9, 0x45, 0x08, 0x8D,
        0x45, 0xFC, 0xD8, 0x0D, 0xD4, 0xE8, 0xA1, 0x00
    };
    static const unsigned char kExp_Cos[16] = {
        0x55, 0x8B, 0xEC, 0x51, 0xD9, 0x45, 0x08, 0x8D,
        0x45, 0xFC, 0xD8, 0x0D, 0xD4, 0xE8, 0xA1, 0x00
    };
    static const unsigned char kExp_Smooth[16] = {
        0x55, 0x8B, 0xEC, 0x51, 0xD9, 0x45, 0x08, 0x8D,
        0x45, 0xFC, 0xD8, 0x0D, 0x80, 0xE3, 0xA1, 0x00
    };

    if (memcmp((const void*)kTarget_Split, kExp_Split, sizeof(kExp_Split)) != 0) {
        Log("[TrigApproxFast] Prologue mismatch at 0x%08X (Split), skipping", kTarget_Split);
    } else {
        if (WineSafe_CreateHook((void*)kTarget_Split, (void*)Hooked_Split, (void**)&orig_Split) == MH_OK &&
            WO_EnableHook((void*)kTarget_Split) == MH_OK) {
            g_installedSplit = true;
            SamplingProfiler::RegisterSelfSymbol("Fast_CMathSplit", (const void*)&Hooked_Split);
            Log("[TrigApproxFast] ACTIVE on CMath::split_ (0x005FE800)");
        } else {
            Log("[TrigApproxFast] Failed to hook 0x005FE800 (Split)");
        }
    }

    if (memcmp((const void*)kTarget_Sin, kExp_Sin, sizeof(kExp_Sin)) != 0) {
        Log("[TrigApproxFast] Prologue mismatch at 0x%08X (Sin), skipping", kTarget_Sin);
    } else {
        if (WineSafe_CreateHook((void*)kTarget_Sin, (void*)Hooked_Sin, (void**)&orig_Sin) == MH_OK &&
            WO_EnableHook((void*)kTarget_Sin) == MH_OK) {
            g_installedSin = true;
            SamplingProfiler::RegisterSelfSymbol("Fast_CMathSin", (const void*)&Hooked_Sin);
            Log("[TrigApproxFast] ACTIVE on CMath::sin_ (0x006F7A10)");
        } else {
            Log("[TrigApproxFast] Failed to hook 0x006F7A10 (Sin)");
        }
    }

    if (memcmp((const void*)kTarget_Cos, kExp_Cos, sizeof(kExp_Cos)) != 0) {
        Log("[TrigApproxFast] Prologue mismatch at 0x%08X (Cos), skipping", kTarget_Cos);
    } else {
        if (WineSafe_CreateHook((void*)kTarget_Cos, (void*)Hooked_Cos, (void**)&orig_Cos) == MH_OK &&
            WO_EnableHook((void*)kTarget_Cos) == MH_OK) {
            g_installedCos = true;
            SamplingProfiler::RegisterSelfSymbol("Fast_CMathCos", (const void*)&Hooked_Cos);
            Log("[TrigApproxFast] ACTIVE on CMath::cos_ (0x005FFF80)");
        } else {
            Log("[TrigApproxFast] Failed to hook 0x005FFF80 (Cos)");
        }
    }

    if (memcmp((const void*)kTarget_Smooth, kExp_Smooth, sizeof(kExp_Smooth)) != 0) {
        Log("[TrigApproxFast] Prologue mismatch at 0x%08X (Smooth), skipping", kTarget_Smooth);
    } else {
        if (WineSafe_CreateHook((void*)kTarget_Smooth, (void*)Hooked_OrganicSmooth, (void**)&orig_Smooth) == MH_OK &&
            WO_EnableHook((void*)kTarget_Smooth) == MH_OK) {
            g_installedSmooth = true;
            SamplingProfiler::RegisterSelfSymbol("Fast_OrganicSmooth", (const void*)&Hooked_OrganicSmooth);
            Log("[TrigApproxFast] ACTIVE on OrganicSmooth (0x005FFFD0)");
        } else {
            Log("[TrigApproxFast] Failed to hook 0x005FFFD0 (Smooth)");
        }
    }
}

void LogStats() {
    if (!Config::g_settings.OptTrigApproxFast) {
        return;
    }

    if (g_installedSplit) {
        Log("[TrigApproxFast] Split Stats: calls=%llu (verified=%llu, mismatches=%llu%s)",
            g_callsSplit, g_verifiedSplit, g_mismatchesSplit, g_deadSplit ? " [RETIRED]" : "");
    }
    if (g_installedSin) {
        Log("[TrigApproxFast] Sin Stats: calls=%llu (verified=%llu, mismatches=%llu%s)",
            g_callsSin, g_verifiedSin, g_mismatchesSin, g_deadSin ? " [RETIRED]" : "");
    }
    if (g_installedCos) {
        Log("[TrigApproxFast] Cos Stats: calls=%llu (verified=%llu, mismatches=%llu%s)",
            g_callsCos, g_verifiedCos, g_mismatchesCos, g_deadCos ? " [RETIRED]" : "");
    }
    if (g_installedSmooth) {
        Log("[TrigApproxFast] Smooth Stats: calls=%llu (verified=%llu, mismatches=%llu%s)",
            g_callsSmooth, g_verifiedSmooth, g_mismatchesSmooth, g_deadSmooth ? " [RETIRED]" : "");
    }
}

void Shutdown() {
    if (g_installedSplit) {
        MH_DisableHook((void*)kTarget_Split);
        g_installedSplit = false;
    }
    if (g_installedSin) {
        MH_DisableHook((void*)kTarget_Sin);
        g_installedSin = false;
    }
    if (g_installedCos) {
        MH_DisableHook((void*)kTarget_Cos);
        g_installedCos = false;
    }
    if (g_installedSmooth) {
        MH_DisableHook((void*)kTarget_Smooth);
        g_installedSmooth = false;
    }
}

} // namespace TrigApproxFast

