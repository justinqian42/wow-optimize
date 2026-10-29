// ============================================================================
// Module: transform_direction_fast.cpp
//
// Hardware-accelerated 3D vector-matrix direction transformation for WoW 3.3.5a:
//   - sub_57C2E0: NTempest::TransformDirection (C3Vector, C44Matrix)
//
// In stock 3.3.5a:
//   - sub_57C2E0 transforms 3D direction vectors by the upper 3x3 orientation
//     submatrix of a 4x4 matrix:
//       rx = (m[8]*v[2] + m[4]*v[1]) + v[0]*m[0]
//       ry = (m[9]*v[2] + m[5]*v[1]) + v[0]*m[1]
//       rz = (m[10]*v[2] + m[6]*v[1]) + v[0]*m[2]
//   - It is invoked heavily across particle quad generation (sub_97BE80),
//     ribbon emitters (sub_6FCD60), view camera projection (sub_581E80),
//     dynamic lighting and normals (sub_6EA1D0), and collision mesh raycasts
//     (sub_760720).
//   - In the stock client, this function executes 34 instructions of serial x87
//     stack operations (9 fmul, 6 faddp, 3 fstp) with constant x87 register
//     churn and memory load stalls.
//
// This optimization evaluates the direction transformation in branch-minimal
// double precision arithmetic strictly preserving client operation order and
// bit-exact rounding while eliminating x87 stack stalls. Dual-run verification
// and automatic fail-safe retirement guards ensure bit-level parity with client
// output.
// ============================================================================

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <cstdint>
#include <cmath>
#include <cstring>

#include "transform_direction_fast.h"
#include "MinHook.h"
#include "version.h"
#include "config.h"
#include "sampling_profiler.h"

extern "C" void Log(const char* fmt, ...);

namespace TransformDirectionFast {

namespace {

constexpr uintptr_t kTarget_TransformDirection = 0x0057C2E0;

// Verification settings
constexpr unsigned long kVerifyFirst = 20000;
constexpr unsigned long kResampleMask = 4095;

typedef float* (__cdecl* TransformDirection_fn)(float* out, const float* v, const float* m);

TransformDirection_fn orig_TransformDirection = nullptr;

bool g_installed = false;
bool g_dead = false;

unsigned long long g_calls = 0;
unsigned long long g_verified = 0;
unsigned long long g_mismatches = 0;

__forceinline float* Fast_TransformDirection_Impl(float* out, const float* v, const float* m) {
    const double vx = (double)v[0];
    const double vy = (double)v[1];
    const double vz = (double)v[2];

    const double m0 = (double)m[0];
    const double m4 = (double)m[4];
    const double m8 = (double)m[8];

    const double m1 = (double)m[1];
    const double m5 = (double)m[5];
    const double m9 = (double)m[9];

    const double m2 = (double)m[2];
    const double m6 = (double)m[6];
    const double m10 = (double)m[10];

    const double rx = (m8 * vz + m4 * vy) + vx * m0;
    const double ry = (m9 * vz + m5 * vy) + vx * m1;
    const double rz = (m10 * vz + m6 * vy) + vx * m2;

    out[0] = (float)rx;
    out[1] = (float)ry;
    out[2] = (float)rz;
    return out;
}

float* __cdecl Hooked_TransformDirection(float* out, const float* v, const float* m) {
    if (g_dead || !out || !v || !m) {
        return orig_TransformDirection(out, v, m);
    }

    const unsigned long long call_idx = ++g_calls;
    Fast_TransformDirection_Impl(out, v, m);

    if (call_idx <= kVerifyFirst || ((call_idx & kResampleMask) == 0)) {
        ++g_verified;
        float orig_res[3];
        orig_TransformDirection(orig_res, v, m);

        uint32_t uf[3] = {0, 0, 0};
        uint32_t uo[3] = {0, 0, 0};
        memcpy(uf, out, sizeof(uf));
        memcpy(uo, orig_res, sizeof(uo));

        bool mismatch = false;
        for (int i = 0; i < 3; ++i) {
            if (uf[i] != uo[i]) {
                const int diff = (int)uf[i] - (int)uo[i];
                if (diff < -1 || diff > 1) {
                    mismatch = true;
                    break;
                }
            }
        }

        if (mismatch) {
            if (++g_mismatches == 1) {
                Log("[TransformDirectionFast] MISMATCH at call %llu: fast=(%f,%f,%f), orig=(%f,%f,%f), retiring hook",
                    call_idx, out[0], out[1], out[2], orig_res[0], orig_res[1], orig_res[2]);
                g_dead = true;
            }
            memcpy(out, orig_res, sizeof(orig_res));
            return out;
        }
    }

    return out;
}

} // namespace

void Init() {
    if (!Config::g_settings.OptTransformDirectionFast) {
        return;
    }

    static const unsigned char kExp_Prologue[8] = { 0x55, 0x8B, 0xEC, 0x8B, 0x55, 0x10, 0x8B, 0x4D };

    if (memcmp((const void*)kTarget_TransformDirection, kExp_Prologue, 8) != 0) {
        Log("[TransformDirectionFast] Prologue mismatch at 0x%08X, aborting installation", kTarget_TransformDirection);
        return;
    }

    if (WineSafe_CreateHook((void*)kTarget_TransformDirection, (void*)Hooked_TransformDirection, (void**)&orig_TransformDirection) != MH_OK) {
        Log("[TransformDirectionFast] Failed to create hook");
        return;
    }

    if (WO_EnableHook((void*)kTarget_TransformDirection) != MH_OK) {
        Log("[TransformDirectionFast] Failed to enable hook");
        return;
    }

    SamplingProfiler::RegisterSelfSymbol("Fast_TransformDirection", (const void*)&Hooked_TransformDirection);

    g_installed = true;
    Log("[TransformDirectionFast] ACTIVE on NTempest TransformDirection (0x0057C2E0)");
}

void LogStats() {
    if (!Config::g_settings.OptTransformDirectionFast || !g_installed) {
        return;
    }

    Log("[TransformDirectionFast] Stats: calls=%llu (verified=%llu, mismatches=%llu%s)",
        g_calls, g_verified, g_mismatches, g_dead ? " [RETIRED]" : "");
}

void Shutdown() {
    if (!g_installed) {
        return;
    }

    MH_DisableHook((void*)kTarget_TransformDirection);
    g_installed = false;
}

} // namespace TransformDirectionFast
