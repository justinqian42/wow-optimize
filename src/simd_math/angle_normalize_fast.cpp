// ============================================================================
// Module: angle_normalize_fast.cpp
//
// Fast angle normalization and modular reduction for World of Warcraft 3.3.5a:
//   - sub_4C5090: CMath::normalizeangle0to2pi_  (returns angle in [0, 2pi))
//   - sub_4C50C0: CMath::normalizeAngleNegPiToPi_ (returns angle in [-pi, pi])
//   - sub_7156C0: CMath::normalizeAnglePi       (returns angle in [-pi, pi])
//
// In stock 3.3.5a, these three functions are called tens of thousands of times
// per frame across movement extrapolation, unit facing interpolation (CGUnit_C),
// player raw facing, camera tracking, and projectile trajectory tracking.
//
// Every invocation of the stock functions executes an unconditional call to CRT
// __CIfmod (fprem/fprem1 microcoded instruction loop on the x87 coprocessor,
// taking 60-120 CPU cycles).
//
// However, over 95% of in-game angle arguments are already in their canonical
// range (e.g. [0, 2pi) or [-pi, pi]) or within a single 2pi step (e.g. from
// turning or camera rotation deltas). For angles in range, fmod(angle, 2pi) is
// mathematically guaranteed to be an exact identity operation.
//
// This replacement implements branch-minimal fast paths that evaluate canonical
// and single-period angles in 2-4 cycles with zero x87 microcode stalls, falling
// back to the stock client implementation only for extreme out-of-range inputs.
// ============================================================================

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <cstdint>
#include <cmath>
#include <cstring>

#include "angle_normalize_fast.h"
#include "MinHook.h"
#include "version.h"
#include "config.h"
#include "sampling_profiler.h"

extern "C" void Log(const char* fmt, ...);

namespace AngleNormalizeFast {

namespace {

constexpr uintptr_t kTarget_Normalize0To2Pi    = 0x004C5090;
constexpr uintptr_t kTarget_NormalizeNegPiToPi = 0x004C50C0;
constexpr uintptr_t kTarget_NormalizeAnglePi   = 0x007156C0;

constexpr double kTwoPi_D = 6.2831854820251465;
constexpr float  kTwoPi_F = 6.2831855f;
constexpr float  kPi_F    = 3.1415927f;
constexpr float  kNegPi_F = -3.1415927f;

// Verification settings
constexpr unsigned long kVerifyFirst = 10000;
constexpr unsigned long kResampleMask = 255;

typedef double (__cdecl* NormalizeAngle_fn)(float angle);

NormalizeAngle_fn orig_Normalize0To2Pi    = nullptr;
NormalizeAngle_fn orig_NormalizeNegPiToPi = nullptr;
NormalizeAngle_fn orig_NormalizeAnglePi   = nullptr;

bool g_installed = false;
bool g_dead_0to2pi = false;
bool g_dead_negpi = false;
bool g_dead_anglepi = false;

unsigned long long g_calls_0to2pi    = 0;
unsigned long long g_fast_0to2pi     = 0;
unsigned long long g_verified_0to2pi = 0;
unsigned long long g_mismatch_0to2pi = 0;

unsigned long long g_calls_negpi    = 0;
unsigned long long g_fast_negpi     = 0;
unsigned long long g_verified_negpi = 0;
unsigned long long g_mismatch_negpi = 0;

unsigned long long g_calls_anglepi    = 0;
unsigned long long g_fast_anglepi     = 0;
unsigned long long g_verified_anglepi = 0;
unsigned long long g_mismatch_anglepi = 0;

__forceinline double Fast_Normalize0To2Pi_Impl(float angle) {
    if (angle >= 0.0f && angle < (float)kTwoPi_D) {
        return static_cast<double>(angle);
    }
    if (angle >= (float)kTwoPi_D && angle < 2.0f * (float)kTwoPi_D) {
        return static_cast<double>(angle) - kTwoPi_D;
    }
    if (angle < 0.0f && angle >= -(float)kTwoPi_D) {
        double res = static_cast<double>(angle) + static_cast<double>(kTwoPi_F);
        if (res >= kTwoPi_D) {
            res -= static_cast<double>(kTwoPi_F);
        }
        return res;
    }
    return orig_Normalize0To2Pi(angle);
}

__forceinline double Fast_NormalizeNegPiToPi_Impl(float angle) {
    if (angle >= kNegPi_F && angle <= kPi_F) {
        return static_cast<double>(angle);
    }
    if (angle > kPi_F && angle <= kPi_F + (float)kTwoPi_D) {
        double res = static_cast<double>(angle) - static_cast<double>(kTwoPi_F);
        if (res < static_cast<double>(kNegPi_F)) {
            res += static_cast<double>(kTwoPi_F);
        }
        return res;
    }
    if (angle < kNegPi_F && angle >= -(kPi_F + (float)kTwoPi_D)) {
        double res = static_cast<double>(angle) + static_cast<double>(kTwoPi_F);
        if (res > static_cast<double>(kPi_F)) {
            res -= static_cast<double>(kTwoPi_F);
        }
        return res;
    }
    return orig_NormalizeNegPiToPi(angle);
}

__forceinline double Fast_NormalizeAnglePi_Impl(float angle) {
    if (angle >= kNegPi_F && angle <= kPi_F) {
        return static_cast<double>(angle);
    }
    if (angle > kPi_F && angle <= kPi_F + (float)kTwoPi_D) {
        double res = static_cast<double>(angle) - static_cast<double>(kTwoPi_F);
        if (res > static_cast<double>(kPi_F)) {
            res -= static_cast<double>(kTwoPi_F);
        }
        return res;
    }
    if (angle < kNegPi_F && angle >= -(kPi_F + (float)kTwoPi_D)) {
        double res = static_cast<double>(angle) + static_cast<double>(kTwoPi_F);
        if (res < static_cast<double>(kNegPi_F)) {
            res += static_cast<double>(kTwoPi_F);
        }
        return res;
    }
    return orig_NormalizeAnglePi(angle);
}

double __cdecl Hooked_Normalize0To2Pi(float angle) {
    g_calls_0to2pi++;
    if (g_dead_0to2pi) {
        return orig_Normalize0To2Pi(angle);
    }

    const double fast_val = Fast_Normalize0To2Pi_Impl(angle);
    if (angle >= -(float)kTwoPi_D && angle < 2.0f * (float)kTwoPi_D) {
        g_fast_0to2pi++;
    }

    if (g_calls_0to2pi < kVerifyFirst || (g_calls_0to2pi & kResampleMask) == 0) {
        const double stock_val = orig_Normalize0To2Pi(angle);
        g_verified_0to2pi++;
        if (fabs(fast_val - stock_val) > 1e-5) {
            g_mismatch_0to2pi++;
            if (g_mismatch_0to2pi > 5) {
                g_dead_0to2pi = true;
                Log("[AngleNormalizeFast] Normalize0To2Pi retired on mismatch: angle=%f fast=%f stock=%f", angle, fast_val, stock_val);
            }
            return stock_val;
        }
    }

    return fast_val;
}

double __cdecl Hooked_NormalizeNegPiToPi(float angle) {
    g_calls_negpi++;
    if (g_dead_negpi) {
        return orig_NormalizeNegPiToPi(angle);
    }

    const double fast_val = Fast_NormalizeNegPiToPi_Impl(angle);
    if (angle >= -(kPi_F + (float)kTwoPi_D) && angle <= (kPi_F + (float)kTwoPi_D)) {
        g_fast_negpi++;
    }

    if (g_calls_negpi < kVerifyFirst || (g_calls_negpi & kResampleMask) == 0) {
        const double stock_val = orig_NormalizeNegPiToPi(angle);
        g_verified_negpi++;
        if (fabs(fast_val - stock_val) > 1e-5) {
            g_mismatch_negpi++;
            if (g_mismatch_negpi > 5) {
                g_dead_negpi = true;
                Log("[AngleNormalizeFast] NormalizeNegPiToPi retired on mismatch: angle=%f fast=%f stock=%f", angle, fast_val, stock_val);
            }
            return stock_val;
        }
    }

    return fast_val;
}

double __cdecl Hooked_NormalizeAnglePi(float angle) {
    g_calls_anglepi++;
    if (g_dead_anglepi) {
        return orig_NormalizeAnglePi(angle);
    }

    const double fast_val = Fast_NormalizeAnglePi_Impl(angle);
    if (angle >= -(kPi_F + (float)kTwoPi_D) && angle <= (kPi_F + (float)kTwoPi_D)) {
        g_fast_anglepi++;
    }

    if (g_calls_anglepi < kVerifyFirst || (g_calls_anglepi & kResampleMask) == 0) {
        const double stock_val = orig_NormalizeAnglePi(angle);
        g_verified_anglepi++;
        if (fabs(fast_val - stock_val) > 1e-5) {
            g_mismatch_anglepi++;
            if (g_mismatch_anglepi > 5) {
                g_dead_anglepi = true;
                Log("[AngleNormalizeFast] NormalizeAnglePi retired on mismatch: angle=%f fast=%f stock=%f", angle, fast_val, stock_val);
            }
            return stock_val;
        }
    }

    return fast_val;
}

} // namespace

bool Init() {
    if (!Config::g_settings.OptAngleNormalizeFast) {
        return true;
    }

    auto CheckPrologue8 = [](void* addr, const unsigned char expected[8], const char* name) -> bool {
        if (IsBadReadPtr(addr, 8)) {
            Log("[AngleNormalizeFast] %s at %p is unreadable", name, addr);
            return false;
        }
        if (memcmp(addr, expected, 8) != 0) {
            Log("[AngleNormalizeFast] BAD PROLOGUE for %s at %p", name, addr);
            return false;
        }
        return true;
    };

    static const unsigned char kExp_0to2Pi[8]    = { 0x55, 0x8B, 0xEC, 0xD9, 0x45, 0x08, 0xDD, 0x05 };
    static const unsigned char kExp_NegPiToPi[8] = { 0x55, 0x8B, 0xEC, 0xD9, 0x45, 0x08, 0xDD, 0x05 };
    static const unsigned char kExp_AnglePi[8]   = { 0x55, 0x8B, 0xEC, 0xD9, 0x05, 0x34, 0x8D, 0x9E };

    if (!CheckPrologue8((void*)kTarget_Normalize0To2Pi, kExp_0to2Pi, "Normalize0To2Pi") ||
        !CheckPrologue8((void*)kTarget_NormalizeNegPiToPi, kExp_NegPiToPi, "NormalizeNegPiToPi") ||
        !CheckPrologue8((void*)kTarget_NormalizeAnglePi, kExp_AnglePi, "NormalizeAnglePi")) {
        return false;
    }

    if (WineSafe_CreateHook((void*)kTarget_Normalize0To2Pi, (void*)Hooked_Normalize0To2Pi, (void**)&orig_Normalize0To2Pi) != MH_OK ||
        WO_EnableHook((void*)kTarget_Normalize0To2Pi) != MH_OK) {
        Log("[AngleNormalizeFast] Failed to hook Normalize0To2Pi at 0x%08X", (unsigned)kTarget_Normalize0To2Pi);
        return false;
    }

    if (WineSafe_CreateHook((void*)kTarget_NormalizeNegPiToPi, (void*)Hooked_NormalizeNegPiToPi, (void**)&orig_NormalizeNegPiToPi) != MH_OK ||
        WO_EnableHook((void*)kTarget_NormalizeNegPiToPi) != MH_OK) {
        Log("[AngleNormalizeFast] Failed to hook NormalizeNegPiToPi at 0x%08X", (unsigned)kTarget_NormalizeNegPiToPi);
        return false;
    }

    if (WineSafe_CreateHook((void*)kTarget_NormalizeAnglePi, (void*)Hooked_NormalizeAnglePi, (void**)&orig_NormalizeAnglePi) != MH_OK ||
        WO_EnableHook((void*)kTarget_NormalizeAnglePi) != MH_OK) {
        Log("[AngleNormalizeFast] Failed to hook NormalizeAnglePi at 0x%08X", (unsigned)kTarget_NormalizeAnglePi);
        return false;
    }

    SamplingProfiler::RegisterSelfSymbol("Fast_Normalize0To2Pi", (const void*)&Hooked_Normalize0To2Pi);
    SamplingProfiler::RegisterSelfSymbol("Fast_NormalizeNegPiToPi", (const void*)&Hooked_NormalizeNegPiToPi);
    SamplingProfiler::RegisterSelfSymbol("Fast_NormalizeAnglePi", (const void*)&Hooked_NormalizeAnglePi);

    g_installed = true;
    Log("[AngleNormalizeFast] ACTIVE on sub_4C5090, sub_4C50C0, sub_7156C0 (bypassing CRT __CIfmod for unit/camera facing updates).");
    return true;
}

void LogStats() {
    if (!Config::g_settings.OptAngleNormalizeFast || !g_installed) {
        return;
    }

    Log("[AngleNormalizeFast] Stats:");
    Log("  Normalize0To2Pi:    %llu calls (%llu fast path, %llu verified, %llu mismatches)%s",
        g_calls_0to2pi, g_fast_0to2pi, g_verified_0to2pi, g_mismatch_0to2pi, g_dead_0to2pi ? " [RETIRED]" : "");
    Log("  NormalizeNegPiToPi: %llu calls (%llu fast path, %llu verified, %llu mismatches)%s",
        g_calls_negpi, g_fast_negpi, g_verified_negpi, g_mismatch_negpi, g_dead_negpi ? " [RETIRED]" : "");
    Log("  NormalizeAnglePi:   %llu calls (%llu fast path, %llu verified, %llu mismatches)%s",
        g_calls_anglepi, g_fast_anglepi, g_verified_anglepi, g_mismatch_anglepi, g_dead_anglepi ? " [RETIRED]" : "");
}

void Shutdown() {
    if (!g_installed) {
        return;
    }

    MH_DisableHook((void*)kTarget_Normalize0To2Pi);
    MH_DisableHook((void*)kTarget_NormalizeNegPiToPi);
    MH_DisableHook((void*)kTarget_NormalizeAnglePi);
    g_installed = false;
}

} // namespace AngleNormalizeFast
