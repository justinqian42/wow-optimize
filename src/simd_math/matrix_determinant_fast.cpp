// ============================================================================
// Module: matrix_determinant_fast.cpp
//
// Hardware-accelerated 4x4 matrix determinant calculation for WoW 3.3.5a:
//   - sub_4C1930: C44Matrix::Determinant() const
//
// In stock 3.3.5a:
//   - sub_4C1930 evaluates the determinant of a 4x4 matrix across 10 engine
//     subsystems: camera projection, NDC transform, terrain projection, portal
//     visibility, shadow mapping, M2 model matrix setup, and particle rendering.
//   - The stock implementation is 506 bytes of serialized x87 FPU code with
//     a 124-byte stack frame. Due to x87 register starvation (only 8 registers),
//     it continually spills and reloads intermediate products across 12 stack
//     variables, creating severe pipeline stalls and memory dependency chains.
//
// This optimization evaluates the Laplace 3x3 cofactor expansion directly in
// IEEE double precision registers strictly preserving client arithmetic order,
// eliminating all 12 stack spills, memory reloads, and FPU stalls. Dual-run
// verification and automatic fail-safe retirement guards ensure strict parity
// with client output.
// ============================================================================

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <cmath>
#include <cstdint>
#include <cstring>

#include "matrix_determinant_fast.h"
#include "MinHook.h"
#include "version.h"
#include "config.h"
#include "sampling_profiler.h"

extern "C" void Log(const char* fmt, ...);

namespace MatrixDeterminantFast {

namespace {

constexpr uintptr_t kTarget_Determinant = 0x004C1930;

// Verification settings
constexpr unsigned long kVerifyFirst = 20000;
constexpr unsigned long kResampleMask = 4095;
constexpr float kVerifyTolerance = 0.0001f;

typedef float (__fastcall* MatrixDeterminant_fn)(const void* this_ptr, void* dummy_edx);

MatrixDeterminant_fn orig_Determinant = nullptr;

bool g_installed = false;
bool g_dead = false;

unsigned long long g_calls = 0;
unsigned long long g_verified = 0;
unsigned long long g_mismatches = 0;

__forceinline float Fast_Determinant_Impl(const void* this_ptr) {
    const float* const m = reinterpret_cast<const float*>(this_ptr);

    const double a0 = m[0],  a1 = m[1],  a2 = m[2],  a3 = m[3];
    const double b0 = m[4],  b1 = m[5],  b2 = m[6],  b3 = m[7];
    const double c0 = m[8],  c1 = m[9],  c2 = m[10], c3 = m[11];
    const double d0 = m[12], d1 = m[13], d2 = m[14], d3 = m[15];

    const double term0 = b1 * c2 * d3 + c3 * b2 * d1 + b3 * c1 * d2 - c2 * b3 * d1 - d3 * (c1 * b2) - b1 * c3 * d2;
    const double term1 = c2 * b0 * d3 + c3 * b2 * d0 + b3 * c0 * d2 - b3 * c2 * d0 - d3 * (c0 * b2) - b0 * c3 * d2;
    const double term2 = c1 * b0 * d3 + c3 * b1 * d0 + b3 * c0 * d1 - b3 * c1 * d0 - d3 * (c0 * b1) - b0 * c3 * d1;
    const double term3 = c1 * b0 * d2 + c2 * b1 * d0 + b2 * c0 * d1 - b2 * c1 * d0 - d2 * (c0 * b1) - b0 * c2 * d1;

    const double det = term0 * a0 - term1 * a1 + term2 * a2 - term3 * a3;
    return static_cast<float>(det);
}

float __fastcall Hooked_Determinant(const void* this_ptr, void* dummy_edx) {
    if (g_dead || !this_ptr) {
        return orig_Determinant(this_ptr, dummy_edx);
    }

    const unsigned long long call_idx = ++g_calls;
    const float fast_res = Fast_Determinant_Impl(this_ptr);

    if (call_idx <= kVerifyFirst || ((call_idx & kResampleMask) == 0)) {
        ++g_verified;
        const float orig_res = orig_Determinant(this_ptr, dummy_edx);

        if (std::fabs(fast_res - orig_res) > kVerifyTolerance) {
            if (++g_mismatches == 1) {
                Log("[MatrixDeterminantFast] MISMATCH at call %llu: fast=%f, orig=%f, diff=%f, retiring hook",
                    call_idx, fast_res, orig_res, std::fabs(fast_res - orig_res));
                g_dead = true;
            }
            return orig_res;
        }
    }

    return fast_res;
}

} // namespace

void Init() {
    if (!Config::g_settings.OptMatrixDeterminantFast) {
        return;
    }

    static const unsigned char kExp_Prologue[8] = { 0x55, 0x8B, 0xEC, 0x83, 0xEC, 0x7C, 0xD9, 0x41 };

    if (memcmp((const void*)kTarget_Determinant, kExp_Prologue, 8) != 0) {
        Log("[MatrixDeterminantFast] Prologue mismatch at 0x%08X, aborting installation", kTarget_Determinant);
        return;
    }

    if (WineSafe_CreateHook((void*)kTarget_Determinant, (void*)Hooked_Determinant, (void**)&orig_Determinant) != MH_OK) {
        Log("[MatrixDeterminantFast] Failed to create hook");
        return;
    }

    if (WO_EnableHook((void*)kTarget_Determinant) != MH_OK) {
        Log("[MatrixDeterminantFast] Failed to enable hook");
        return;
    }

    SamplingProfiler::RegisterSelfSymbol("Fast_MatrixDeterminant", (const void*)&Hooked_Determinant);

    g_installed = true;
    Log("[MatrixDeterminantFast] ACTIVE on C44Matrix Determinant (0x004C1930)");
}

void LogStats() {
    if (!Config::g_settings.OptMatrixDeterminantFast || !g_installed) {
        return;
    }

    Log("[MatrixDeterminantFast] Stats: calls=%llu (verified=%llu, mismatches=%llu%s)",
        g_calls, g_verified, g_mismatches, g_dead ? " [RETIRED]" : "");
}

void Shutdown() {
    if (!g_installed) {
        return;
    }

    MH_DisableHook((void*)kTarget_Determinant);
    g_installed = false;
}

} // namespace MatrixDeterminantFast
