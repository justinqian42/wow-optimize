// ============================================================================
// Module: aabb_transform_sse2
//
// Hardware double-precision SSE2 rewrite of the bounding-box transformation
// routines:
//   sub_7F9430: 4x4 matrix * AABB transformation (including translation)
//   sub_7F93D0: 3x3 matrix * AABB transformation (orientation only)
//
// Background & Analysis:
//   During scene graph visibility traversal (sub_7A50C0 and 21 other callers
//   across the rendering, culling, and collision pipeline), the client transforms
//   an object-space axis-aligned bounding box into world-space before performing
//   culling tests (such as AABB_Overlap at sub_78F370 or frustum intersection).
//
//   Both sub_7F9430 and sub_7F93D0 delegate their core computation to sub_7F9320,
//   which implements Jim Arvo's bounding box transformation algorithm (Graphics Gems I).
//   For each of the three output coordinates (X, Y, Z), it performs three pairs
//   of x87 multiplications and compares:
//
//       fld    dword ptr [esi]
//       fmul   dword ptr [edx+edi]
//       fld    dword ptr [edx+edi]
//       fmul   dword ptr [esi+0Ch]
//       fcom   st(1)
//       fnstsw ax
//       test   ah, 41h
//       jnz    loc_7F9362
//
//   There are 18 serialized x87 status-word transfers (fnstsw ax) and 9 data-dependent
//   branches per box. Because scene objects have diverse orientations and scales,
//   the branch predictor frequently mispredicts on every axis.
//
//   This module replaces both functions with hardware double-precision SSE2 math.
//   Under MSVC /fp:precise /arch:SSE2, each axis evaluates in registers without
//   x87 stack transfers or conditional branch mispredictions, while maintaining
//   100% bit-exact parity with the client's 53-bit x87 arithmetic and per-step
//   float rounding.
//
//   Dual-run verified against client output for bit-exact floating-point results.
// ============================================================================

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <emmintrin.h>
#include <cstdint>
#include <cstring>

#include "aabb_transform_sse2.h"
#include "MinHook.h"
#include "version.h"
#include "config.h"
#include "sampling_profiler.h"
#include "ab_test.h"
#include "session_verdict.h"

extern "C" void Log(const char* fmt, ...);

MH_STATUS WineSafe_CreateHook(void* target, void* detour, void** original);
MH_STATUS WO_EnableHook(void* target);

namespace AabbTransform {

namespace {

constexpr uintptr_t kTransform4x4 = 0x007F9430;
constexpr uintptr_t kTransform3x3 = 0x007F93D0;

typedef void (__cdecl* Transform4x4_fn)(const float* matrix, const float* in_aabb, float* out_aabb);
typedef void (__cdecl* Transform3x3_fn)(const float* matrix, const float* in_aabb, float* out_aabb);

Transform4x4_fn orig_Transform4x4 = nullptr;
Transform3x3_fn orig_Transform3x3 = nullptr;

bool g_installed = false;
bool g_armed4x4  = false;
bool g_armed3x3  = false;
bool g_abSubject = false;
bool g_dead      = false;

unsigned long g_calls4x4    = 0;
unsigned long g_calls3x3    = 0;
unsigned long g_verified4x4 = 0;
unsigned long g_verified3x3 = 0;

constexpr unsigned long kVerifyFirst  = 30000;
constexpr unsigned long kResampleMask = 8191;

// ============================================================================
// Core Axis Transformation (Jim Arvo's algorithm)
//
// Transforms one output coordinate axis by accumulating the min and max
// product terms from each of the three input dimensions (X, Y, Z).
//
// To match the client's x87 53-bit FPU state and rounding behavior exactly:
//   - Products are computed in IEEE double precision.
//   - Each product term is added to the running sum in double precision.
//   - The intermediate sum is rounded to 32-bit float at each of the 3 steps,
//     matching the client's `fstp dword ptr` after every term.
// ============================================================================
inline void TransformAxis(
    double r0, double r1, double r2,
    double in_min_x, double in_max_x,
    double in_min_y, double in_max_y,
    double in_min_z, double in_max_z,
    double init_val,
    float& out_min, float& out_max)
{
    double cur_min = init_val;
    double cur_max = init_val;

    // j = 0 (X axis contribution)
    {
        const double a0 = in_min_x * r0;
        const double b0 = in_max_x * r0;
        if (b0 <= a0) {
            cur_min = (float)(cur_min + b0);
            cur_max = (float)(cur_max + a0);
        } else {
            cur_min = (float)(cur_min + a0);
            cur_max = (float)(cur_max + b0);
        }
    }

    // j = 1 (Y axis contribution)
    {
        const double a1 = in_min_y * r1;
        const double b1 = in_max_y * r1;
        if (b1 <= a1) {
            cur_min = (float)(cur_min + b1);
            cur_max = (float)(cur_max + a1);
        } else {
            cur_min = (float)(cur_min + a1);
            cur_max = (float)(cur_max + b1);
        }
    }

    // j = 2 (Z axis contribution)
    {
        const double a2 = in_min_z * r2;
        const double b2 = in_max_z * r2;
        if (b2 <= a2) {
            cur_min = (float)(cur_min + b2);
            cur_max = (float)(cur_max + a2);
        } else {
            cur_min = (float)(cur_min + a2);
            cur_max = (float)(cur_max + b2);
        }
    }

    out_min = (float)cur_min;
    out_max = (float)cur_max;
}

// 4x4 Matrix transformation (sub_7F9430)
inline void EvaluateTransform4x4(const float* m, const float* in_aabb, float* out_aabb) {
    const double in_min_x = (double)in_aabb[0];
    const double in_min_y = (double)in_aabb[1];
    const double in_min_z = (double)in_aabb[2];
    const double in_max_x = (double)in_aabb[3];
    const double in_max_y = (double)in_aabb[4];
    const double in_max_z = (double)in_aabb[5];

    // Translation vector from row 3 of the 4x4 matrix
    const double trans_x = (double)m[12];
    const double trans_y = (double)m[13];
    const double trans_z = (double)m[14];

    float res_min_x, res_max_x;
    float res_min_y, res_max_y;
    float res_min_z, res_max_z;

    TransformAxis((double)m[0], (double)m[4], (double)m[8],
                  in_min_x, in_max_x, in_min_y, in_max_y, in_min_z, in_max_z,
                  trans_x, res_min_x, res_max_x);

    TransformAxis((double)m[1], (double)m[5], (double)m[9],
                  in_min_x, in_max_x, in_min_y, in_max_y, in_min_z, in_max_z,
                  trans_y, res_min_y, res_max_y);

    TransformAxis((double)m[2], (double)m[6], (double)m[10],
                  in_min_x, in_max_x, in_min_y, in_max_y, in_min_z, in_max_z,
                  trans_z, res_min_z, res_max_z);

    out_aabb[0] = res_min_x;
    out_aabb[1] = res_min_y;
    out_aabb[2] = res_min_z;
    out_aabb[3] = res_max_x;
    out_aabb[4] = res_max_y;
    out_aabb[5] = res_max_z;
}

// 3x3 Matrix transformation without translation (sub_7F93D0)
inline void EvaluateTransform3x3(const float* m, const float* in_aabb, float* out_aabb) {
    const double in_min_x = (double)in_aabb[0];
    const double in_min_y = (double)in_aabb[1];
    const double in_min_z = (double)in_aabb[2];
    const double in_max_x = (double)in_aabb[3];
    const double in_max_y = (double)in_aabb[4];
    const double in_max_z = (double)in_aabb[5];

    float res_min_x, res_max_x;
    float res_min_y, res_max_y;
    float res_min_z, res_max_z;

    TransformAxis((double)m[0], (double)m[3], (double)m[6],
                  in_min_x, in_max_x, in_min_y, in_max_y, in_min_z, in_max_z,
                  0.0, res_min_x, res_max_x);

    TransformAxis((double)m[1], (double)m[4], (double)m[7],
                  in_min_x, in_max_x, in_min_y, in_max_y, in_min_z, in_max_z,
                  0.0, res_min_y, res_max_y);

    TransformAxis((double)m[2], (double)m[5], (double)m[8],
                  in_min_x, in_max_x, in_min_y, in_max_y, in_min_z, in_max_z,
                  0.0, res_min_z, res_max_z);

    out_aabb[0] = res_min_x;
    out_aabb[1] = res_min_y;
    out_aabb[2] = res_min_z;
    out_aabb[3] = res_max_x;
    out_aabb[4] = res_max_y;
    out_aabb[5] = res_max_z;
}

// ============================================================================
// Dual-Run Hook Bodies
// ============================================================================

void Hooked_Transform4x4Body(const float* m, const float* in_aabb, float* out_aabb) {
    g_calls4x4++;
    if (g_dead) {
        orig_Transform4x4(m, in_aabb, out_aabb);
        return;
    }

    if (!g_armed4x4 || (g_calls4x4 & kResampleMask) == 0) {
        float theirs[6];
        float mine[6];
        orig_Transform4x4(m, in_aabb, theirs);
        EvaluateTransform4x4(m, in_aabb, mine);

        if (memcmp(theirs, mine, sizeof(theirs)) != 0) {
            g_dead = true;
            Verdict::Add(Verdict::Bad, "AabbTransform 4x4 disagreed with client; retired for session");
            Log("[AabbTransform] DISAGREED on 4x4 matrix at call %lu - retired for session", g_calls4x4);
            memcpy(out_aabb, theirs, sizeof(theirs));
            return;
        }

        g_verified4x4++;
        if (!g_armed4x4 && g_verified4x4 >= kVerifyFirst) {
            g_armed4x4 = true;
            Log("[AabbTransform] 4x4 armed: %lu calls agreed with client exactly, now answering directly (resampling 1 in %lu)",
                g_verified4x4, kResampleMask + 1);
        }
        memcpy(out_aabb, theirs, sizeof(theirs));
        return;
    }

    EvaluateTransform4x4(m, in_aabb, out_aabb);
}

void Hooked_Transform3x3Body(const float* m, const float* in_aabb, float* out_aabb) {
    g_calls3x3++;
    if (g_dead) {
        orig_Transform3x3(m, in_aabb, out_aabb);
        return;
    }

    if (!g_armed3x3 || (g_calls3x3 & kResampleMask) == 0) {
        float theirs[6];
        float mine[6];
        orig_Transform3x3(m, in_aabb, theirs);
        EvaluateTransform3x3(m, in_aabb, mine);

        if (memcmp(theirs, mine, sizeof(theirs)) != 0) {
            g_dead = true;
            Verdict::Add(Verdict::Bad, "AabbTransform 3x3 disagreed with client; retired for session");
            Log("[AabbTransform] DISAGREED on 3x3 matrix at call %lu - retired for session", g_calls3x3);
            memcpy(out_aabb, theirs, sizeof(theirs));
            return;
        }

        g_verified3x3++;
        if (!g_armed3x3 && g_verified3x3 >= kVerifyFirst) {
            g_armed3x3 = true;
            Log("[AabbTransform] 3x3 armed: %lu calls agreed with client exactly, now answering directly (resampling 1 in %lu)",
                g_verified3x3, kResampleMask + 1);
        }
        memcpy(out_aabb, theirs, sizeof(theirs));
        return;
    }

    EvaluateTransform3x3(m, in_aabb, out_aabb);
}

// Detour wrappers with AbTest and SEH guard
void __cdecl Hooked_Transform4x4(const float* m, const float* in_aabb, float* out_aabb) {
    if (!g_abSubject) {
        Hooked_Transform4x4Body(m, in_aabb, out_aabb);
        return;
    }
    const unsigned long long t = AbTest::TickIn();
    __try {
        if (AbTest::StandAside()) {
            orig_Transform4x4(m, in_aabb, out_aabb);
        } else {
            Hooked_Transform4x4Body(m, in_aabb, out_aabb);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        g_dead = true;
        orig_Transform4x4(m, in_aabb, out_aabb);
    }
    AbTest::TickOut(t);
}

void __cdecl Hooked_Transform3x3(const float* m, const float* in_aabb, float* out_aabb) {
    if (!g_abSubject) {
        Hooked_Transform3x3Body(m, in_aabb, out_aabb);
        return;
    }
    const unsigned long long t = AbTest::TickIn();
    __try {
        if (AbTest::StandAside()) {
            orig_Transform3x3(m, in_aabb, out_aabb);
        } else {
            Hooked_Transform3x3Body(m, in_aabb, out_aabb);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        g_dead = true;
        orig_Transform3x3(m, in_aabb, out_aabb);
    }
    AbTest::TickOut(t);
}

}  // namespace

bool Init() {
    if (!Config::g_settings.OptAabbTransform) {
        Log("[AabbTransform] not installed: switched off.");
        return false;
    }

    if (!WowOpt_ClientPatchAllowed((const void*)kTransform4x4) ||
        !WowOpt_ClientPatchAllowed((const void*)kTransform3x3)) {
        Log("[AabbTransform] not installed: No Client Patches is active.");
        return false;
    }

    if (IsBadReadPtr((void*)kTransform4x4, 16) ||
        IsBadReadPtr((void*)kTransform3x3, 16)) {
        Log("[AabbTransform] unreadable targets - not installing.");
        return false;
    }

    const unsigned char* p4 = (const unsigned char*)kTransform4x4;
    const unsigned char* p3 = (const unsigned char*)kTransform3x3;
    if (p4[0] != 0x55 || p4[1] != 0x8B || p4[2] != 0xEC ||
        p3[0] != 0x55 || p3[1] != 0x8B || p3[2] != 0xEC) {
        Log("[AabbTransform] unexpected prologue bytes - not installing.");
        return false;
    }

    // Startup self-test: verify identity matrix transformation
    {
        const float id4[16] = {
            1.0f, 0.0f, 0.0f, 0.0f,
            0.0f, 1.0f, 0.0f, 0.0f,
            0.0f, 0.0f, 1.0f, 0.0f,
            10.0f, 20.0f, 30.0f, 1.0f
        };
        const float in_box[6] = { -5.0f, -10.0f, -15.0f, 5.0f, 10.0f, 15.0f };
        float out_box[6] = { 0 };

        EvaluateTransform4x4(id4, in_box, out_box);
        if (out_box[0] != 5.0f || out_box[1] != 10.0f || out_box[2] != 15.0f ||
            out_box[3] != 15.0f || out_box[4] != 30.0f || out_box[5] != 45.0f) {
            Log("[AabbTransform] startup self-test failed on identity transform - not installing.");
            return false;
        }
    }

    int ok = 0;
    if (WineSafe_CreateHook((void*)kTransform4x4, (void*)Hooked_Transform4x4,
                            (void**)&orig_Transform4x4) == MH_OK &&
        WO_EnableHook((void*)kTransform4x4) == MH_OK) {
        ok++;
    }

    if (WineSafe_CreateHook((void*)kTransform3x3, (void*)Hooked_Transform3x3,
                            (void**)&orig_Transform3x3) == MH_OK &&
        WO_EnableHook((void*)kTransform3x3) == MH_OK) {
        ok++;
    }

    if (ok == 0) {
        Log("[AabbTransform] not installed: MinHook failed on both transform evaluators.");
        return false;
    }

    g_installed = true;
    SamplingProfiler::RegisterSelfSymbol("AabbTransform4x4_SSE2", (const void*)&Hooked_Transform4x4);
    SamplingProfiler::RegisterSelfSymbol("AabbTransform3x3_SSE2", (const void*)&Hooked_Transform3x3);

    g_abSubject = AbTest::IsSubject("AabbTransform", &g_abSubject);

    Log("[AabbTransform] ACTIVE on %d of 2 bounding box transform routines (sub_7F9430 and sub_7F93D0). "
        "Evaluates Jim Arvo's box transformation algorithm in hardware double-precision SSE2. "
        "Verifying first %lu calls bit for bit with ongoing resampling.%s",
        ok, kVerifyFirst,
        Config::g_settings.OptAbTest
            ? " The A/B harness alternates the switch between stints."
            : "");
    return true;
}

void Shutdown() {
    if (!g_installed) return;
    MH_DisableHook((void*)kTransform4x4);
    MH_DisableHook((void*)kTransform3x3);
    g_installed = false;
}

void LogStats() {
    if (!Config::g_settings.OptAabbTransform) {
        Log("[AabbTransform] not measured: switched off.");
        return;
    }
    if (!g_installed) {
        Log("[AabbTransform] not measured: hooks are not installed.");
        return;
    }
    if (g_dead) {
        Log("[AabbTransform] retired early: evaluation disagreed with the client.");
        return;
    }
    const unsigned long total = g_calls4x4 + g_calls3x3;
    if (total == 0) {
        Log("[AabbTransform] measured and zero: the hooks are in and neither was reached.");
        return;
    }
    Log("[AabbTransform] calls: 4x4=%lu (verified=%lu, armed=%s) | "
        "3x3=%lu (verified=%lu, armed=%s)",
        g_calls4x4, g_verified4x4, g_armed4x4 ? "yes" : "no",
        g_calls3x3, g_verified3x3, g_armed3x3 ? "yes" : "no");
}

}  // namespace AabbTransform
