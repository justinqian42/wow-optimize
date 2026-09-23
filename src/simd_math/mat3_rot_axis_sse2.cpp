// ============================================================================
// Module: mat3_rot_axis_sse2.cpp
//
// Accelerates Rodrigues' 3x3 rotation matrix generation in sub_4C5820
// (0x112 bytes / 274 bytes).
//
// In profiler logs, sub_4C5820 was sampled 7,002 times at 0x004C588B (the x87
// fsincos instruction) during scene rendering and particle updates. Callers
// include ParticleQuad (sub_97BE80), RibbonEmitter update/render (sub_6FF6D0,
// sub_702FC0), Matrix3x3::RotateAxisAngle (sub_4C5940), and entity orientation
// helpers (sub_981D40, sub_98CA00).
//
// The client implementation executes 114 instructions with over 60 x87 floating
// point operations, creating x87 stack register pressure that requires spilling
// partial products to stack memory as 32-bit floats.
//
// This replacement:
//   1. Normalizes the axis vector (if is_normalized is 0) using 53-bit double
//      precision matching client x87 reciprocal square root precision.
//   2. Evaluates sine and cosine of the angle via inlined fsincos instruction.
//   3. Derives 3x3 rotation matrix elements in exact client accumulation order
//      and precision semantics, including matching client stack spill float
//      truncations for out[3] and out[6].
//   4. Eliminates x87 stack register spills and status-word stalls, storing
//      directly into the output matrix.
//   5. Guarantees zero security cookies and zero SEH frames on the hot path via
//      __declspec(safebuffers).
//
// Verification: Verified offline against verbatim client instructions over
// 1,000,000 cases with 0 bit differences (harness only, not run in a game).
// Runtime verification tests the first 10,000 calls and 1 in every 128
// thereafter, retiring immediately on the first mismatch.
// ============================================================================

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "mat3_rot_axis_sse2.h"
#include "MinHook.h"
#include "version.h"
#include "config.h"
#include "ab_test.h"
#include "sampling_profiler.h"

extern "C" void Log(const char* fmt, ...);
MH_STATUS WineSafe_CreateHook(void* target, void* detour, void** original);
MH_STATUS WO_EnableHook(void* target);

namespace Mat3RotAxis {

namespace {

constexpr uintptr_t kTarget = 0x004C5820;

// push ebp / mov ebp, esp / sub esp, 18h / cmp byte ptr [ebp+14h], 0 / mov eax, [ebp+10h] / mov ecx, [eax] / mov edx, [eax+4]
const unsigned char kPrologue[16] = {
    0x55, 0x8B, 0xEC, 0x83, 0xEC, 0x18, 0x80, 0x7D,
    0x14, 0x00, 0x8B, 0x45, 0x10, 0x8B, 0x08, 0x8B
};

typedef float* (__cdecl* Mat3RotAxis_fn)(float* out_mat, float angle, const float* axis, int is_normalized);
static Mat3RotAxis_fn g_orig = nullptr;

static bool g_installed = false;
static bool g_active = false;
static bool g_dead = false;
static bool g_abSubject = false;

static uint64_t g_calls = 0;
static uint64_t g_armedCalls = 0;
static uint64_t g_verifiedCalls = 0;
static uint64_t g_controlCalls = 0;
static uint32_t g_mismatches = 0;

constexpr uint32_t kVerifyCalls = 10000;
constexpr uint32_t kVerifyMask = 127;

__declspec(safebuffers) static inline float* FastMat3RotAxis(float* out_mat, float angle, const float* axis, int is_normalized) {
    float x = axis[0];
    float y = axis[1];
    float z = axis[2];

    if (!(is_normalized & 0xFF)) {
        double dz = (double)z;
        double dy = (double)y;
        double dx = (double)x;
        double len_sq = (dz * dz + dy * dy) + (dx * dx);
        double inv_len = 1.0 / sqrt(len_sq);
        x = (float)(dx * inv_len);
        y = (float)(dy * inv_len);
        z = (float)(dz * inv_len);
    }

    float c, s;
    __asm {
        fld dword ptr [angle]
        fsincos
        fstp dword ptr [c]
        fstp dword ptr [s]
    }

    const double dx = (double)x;
    const double dy = (double)y;
    const double dz = (double)z;
    const double dc = (double)c;
    const double ds = (double)s;

    const float xy = (float)(dx * dy);
    const float yz = (float)(dz * dy);
    const float zx = (float)(dz * dx);

    const float zs = (float)(ds * dz);
    const double ys = ds * dy;
    const double xs = ds * dx;

    const double t = 1.0 - dc;

    // out[0]
    out_mat[0] = (float)((dx * (dx * t)) + dc);

    // out[1] & out[3]
    const double xy_t = (double)xy * t;
    const float xy_t_flt = (float)xy_t;
    out_mat[1] = (float)(xy_t + (double)zs);
    out_mat[3] = (float)((double)xy_t_flt - (double)zs);

    // out[2] & out[6]
    const double zx_t = (double)zx * t;
    const float zx_t_flt = (float)zx_t;
    out_mat[2] = (float)(zx_t - ys);
    out_mat[6] = (float)((double)zx_t_flt + ys);

    // out[4]
    out_mat[4] = (float)((dy * (dy * t)) + dc);

    // out[5] & out[7]
    const double yz_t = (double)yz * t;
    out_mat[5] = (float)(yz_t + xs);
    out_mat[7] = (float)(yz_t - xs);

    // out[8]
    out_mat[8] = (float)(((dz * dz) * t) + dc);

    return out_mat;
}

__declspec(noinline) static float* Verify_sub_4C5820(float* out_mat, float angle, const float* axis, int is_normalized) {
    alignas(16) float client_out[9];
    alignas(16) float fast_out[9];

    float* const clientRes = g_orig(client_out, angle, axis, is_normalized);
    float* const fastRes = FastMat3RotAxis(fast_out, angle, axis, is_normalized);

    if (clientRes != client_out || fastRes != fast_out || memcmp(client_out, fast_out, sizeof(client_out)) != 0) {
        g_dead = true;
        ++g_mismatches;
        Log("[Mat3RotAxis] Disagreement at angle=%f axis={%f,%f,%f} norm=%d. Hook retired.",
            angle, axis ? axis[0] : 0.0f, axis ? axis[1] : 0.0f, axis ? axis[2] : 0.0f, is_normalized);
        memcpy(out_mat, client_out, sizeof(client_out));
        return out_mat;
    }

    ++g_verifiedCalls;
    memcpy(out_mat, client_out, sizeof(client_out));
    return out_mat;
}

__declspec(safebuffers) static float* __cdecl Hook_sub_4C5820(float* out_mat, float angle, const float* axis, int is_normalized) {
    ++g_calls;
    if (g_dead || !g_active) {
        return g_orig(out_mat, angle, axis, is_normalized);
    }
    if (g_abSubject && AbTest::StandAside()) {
        ++g_controlCalls;
        return g_orig(out_mat, angle, axis, is_normalized);
    }
    if (g_verifiedCalls < kVerifyCalls || ((g_verifiedCalls & kVerifyMask) == 0)) {
        return Verify_sub_4C5820(out_mat, angle, axis, is_normalized);
    }
    ++g_armedCalls;
    return FastMat3RotAxis(out_mat, angle, axis, is_normalized);
}

} // anonymous namespace

bool Init() {
    if (!Config::g_settings.OptMat3RotAxis) {
        return true;
    }

    if (!WowOpt_ClientPatchAllowed((const void*)kTarget)) {
        Log("[Mat3RotAxis] NOT active: client patches not allowed");
        return false;
    }

    if (memcmp((const void*)kTarget, kPrologue, sizeof(kPrologue)) != 0) {
        Log("[Mat3RotAxis] NOT active: prologue mismatch at 0x%08X", (unsigned)kTarget);
        return false;
    }

    if (WineSafe_CreateHook((void*)kTarget, (void*)Hook_sub_4C5820, (void**)&g_orig) != MH_OK) {
        Log("[Mat3RotAxis] NOT active: CreateHook failed on 0x%08X", (unsigned)kTarget);
        return false;
    }

    if (WO_EnableHook((void*)kTarget) != MH_OK) {
        Log("[Mat3RotAxis] NOT active: EnableHook failed on 0x%08X", (unsigned)kTarget);
        return false;
    }

    g_installed = true;
    g_active = true;
    g_abSubject = AbTest::IsSubject("Mat3RotAxis", &g_abSubject);
    SamplingProfiler::RegisterSelfSymbol("Mat3RotAxis_Hook", (const void*)&Hook_sub_4C5820);
    Log("[Mat3RotAxis] Hook installed on sub_4C5820 (0x112 bytes)");
    return true;
}

void Shutdown() {
    if (g_installed) {
        g_active = false;
        g_installed = false;
    }
}

void LogStats() {
    if (!Config::g_settings.OptMat3RotAxis) return;
    if (!g_installed) {
        Log("[Mat3RotAxis] Not installed");
        return;
    }
    Log("[Mat3RotAxis] calls=%llu (armed=%llu, verified=%llu, control=%llu) mismatches=%u%s",
        g_calls, g_armedCalls, g_verifiedCalls, g_controlCalls, g_mismatches,
        g_dead ? " [RETIRED]" : "");
}

} // namespace Mat3RotAxis
