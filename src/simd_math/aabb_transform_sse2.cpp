// ============================================================================
// Module: aabb_transform_sse2
//
// Hardware double-precision SSE2 rewrite of the bounding-box transformation
// routines:
//   sub_7F9430: 4x4 matrix * AABB transformation (including translation)
//   sub_7F93D0: 3x3 matrix * AABB transformation (orientation only)
//   sub_984930: AABB_FromVertices: bounding box [min, max] calculation from vertices
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
//   sub_984930 (AABB_FromVertices) evaluates component-wise min and max across an
//   arbitrary array of 3D vertices using x87 4-way unrolling with 6 fcom / fnstsw
//   sequences and conditional branches per vertex. Vectorized using SSE2 minps/maxps.
//
//   This module replaces these routines with hardware double-precision and SSE2 math.
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
#if !TEST_DISABLE_AABB_FROM_VERTS_SSE2
constexpr uintptr_t kFromVertices = 0x00984930;
#endif

typedef void (__cdecl* Transform4x4_fn)(const float* matrix, const float* in_aabb, float* out_aabb);
typedef void (__cdecl* Transform3x3_fn)(const float* matrix, const float* in_aabb, float* out_aabb);
#if !TEST_DISABLE_AABB_FROM_VERTS_SSE2
typedef float* (__cdecl* FromVertices_fn)(float* out_box, const float* verts, uint32_t count);
#endif

Transform4x4_fn orig_Transform4x4 = nullptr;
Transform3x3_fn orig_Transform3x3 = nullptr;
#if !TEST_DISABLE_AABB_FROM_VERTS_SSE2
FromVertices_fn orig_FromVertices = nullptr;
#endif

bool g_installed = false;
bool g_armed4x4  = false;
bool g_armed3x3  = false;
#if !TEST_DISABLE_AABB_FROM_VERTS_SSE2
bool g_armedFromVerts = false;
#endif
bool g_abSubject = false;
bool g_dead      = false;

unsigned long g_calls4x4    = 0;
unsigned long g_calls3x3    = 0;
unsigned long g_verified4x4 = 0;
unsigned long g_verified3x3 = 0;
#if !TEST_DISABLE_AABB_FROM_VERTS_SSE2
unsigned long g_callsFromVerts    = 0;
unsigned long g_verifiedFromVerts = 0;
#endif

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

#if !TEST_DISABLE_AABB_FROM_VERTS_SSE2
// ============================================================================
// AABB_FromVertices (sub_984930)
//
// Computes axis-aligned bounding box [minX, minY, minZ, maxX, maxY, maxZ] from
// an arbitrary stream of 3D vertices (float x, y, z triplets).
//
// Replaces stock 4-way unrolled x87 fcom / fnstsw ax / test sequences with
// parallel SSE2 _mm_min_ps / _mm_max_ps, eliminating serialized status-word
// stalls and data-dependent branch mispredictions entirely.
// ============================================================================
inline float* AABB_FromVertices_SSE2(float* out_box, const float* verts, uint32_t count) {
    if (!out_box) return nullptr;
    if (!verts || count == 0) {
        out_box[0] = 0.0f;
        out_box[1] = 0.0f;
        out_box[2] = 0.0f;
        out_box[3] = 0.0f;
        out_box[4] = 0.0f;
        out_box[5] = 0.0f;
        return out_box;
    }

    __m128 min_vec = _mm_set_ps(0.0f, verts[2], verts[1], verts[0]);
    __m128 max_vec = min_vec;

    uint32_t i = 1;
    const float* p = verts + 3;

    // Process 4 vertices per iteration
    for (; i + 3 < count; i += 4, p += 12) {
        __m128 v0 = _mm_set_ps(0.0f, p[2],  p[1],  p[0]);
        __m128 v1 = _mm_set_ps(0.0f, p[5],  p[4],  p[3]);
        __m128 v2 = _mm_set_ps(0.0f, p[8],  p[7],  p[6]);
        __m128 v3 = _mm_set_ps(0.0f, p[11], p[10], p[9]);

        __m128 min01 = _mm_min_ps(v0, v1);
        __m128 min23 = _mm_min_ps(v2, v3);
        min_vec = _mm_min_ps(min_vec, _mm_min_ps(min01, min23));

        __m128 max01 = _mm_max_ps(v0, v1);
        __m128 max23 = _mm_max_ps(v2, v3);
        max_vec = _mm_max_ps(max_vec, _mm_max_ps(max01, max23));
    }

    // Remainder vertices
    for (; i < count; ++i, p += 3) {
        __m128 v = _mm_set_ps(0.0f, p[2], p[1], p[0]);
        min_vec = _mm_min_ps(min_vec, v);
        max_vec = _mm_max_ps(max_vec, v);
    }

    alignas(16) float min_buf[4];
    alignas(16) float max_buf[4];
    _mm_store_ps(min_buf, min_vec);
    _mm_store_ps(max_buf, max_vec);

    out_box[0] = min_buf[0];
    out_box[1] = min_buf[1];
    out_box[2] = min_buf[2];
    out_box[3] = max_buf[0];
    out_box[4] = max_buf[1];
    out_box[5] = max_buf[2];

    return out_box;
}

float* Hooked_FromVerticesBody(float* out_box, const float* verts, uint32_t count) {
    g_callsFromVerts++;
    if (g_dead) {
        return orig_FromVertices(out_box, verts, count);
    }

    if (!g_armedFromVerts || (g_callsFromVerts & kResampleMask) == 0) {
        float theirs[6];
        float mine[6];
        orig_FromVertices(theirs, verts, count);
        AABB_FromVertices_SSE2(mine, verts, count);

        if (memcmp(theirs, mine, sizeof(theirs)) != 0) {
            g_dead = true;
            Verdict::Add(Verdict::Bad, "AABB_FromVertices disagreed with client; retired for session");
            Log("[AabbTransform] DISAGREED on FromVertices at call %lu - retired for session", g_callsFromVerts);
            memcpy(out_box, theirs, sizeof(theirs));
            return out_box;
        }

        g_verifiedFromVerts++;
        if (!g_armedFromVerts && g_verifiedFromVerts >= kVerifyFirst) {
            g_armedFromVerts = true;
            Log("[AabbTransform] FromVertices armed: %lu calls agreed with client exactly, now answering directly (resampling 1 in %lu)",
                g_verifiedFromVerts, kResampleMask + 1);
        }
        memcpy(out_box, theirs, sizeof(theirs));
        return out_box;
    }

    return AABB_FromVertices_SSE2(out_box, verts, count);
}

float* __cdecl Hooked_FromVertices(float* out_box, const float* verts, uint32_t count) {
    if (!g_abSubject) {
        return Hooked_FromVerticesBody(out_box, verts, count);
    }
    const unsigned long long t = AbTest::TickIn();
    float* res = nullptr;
    __try {
        if (AbTest::StandAside()) {
            res = orig_FromVertices(out_box, verts, count);
        } else {
            res = Hooked_FromVerticesBody(out_box, verts, count);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        g_dead = true;
        res = orig_FromVertices(out_box, verts, count);
    }
    AbTest::TickOut(t);
    return res;
}

static bool SelfTestFromVertices() {
    FromVertices_fn original = (FromVertices_fn)kFromVertices;
    const int CASES = 10000;
    unsigned seed = 0x12345679u;

    constexpr size_t kMaxVerts = 512;
    float vert_buf[kMaxVerts * 3];
    float theirs[6];
    float mine[6];

    for (int c = 0; c < CASES; ++c) {
        uint32_t count = 0;
        if (c == 0) count = 0;
        else if (c == 1) count = 1;
        else if (c == 2) count = 2;
        else if (c == 3) count = 3;
        else if (c == 4) count = 4;
        else if (c == 5) count = 5;
        else if (c == 6) count = 7;
        else if (c == 7) count = 8;
        else if (c == 8) count = 15;
        else if (c == 9) count = 16;
        else {
            seed = seed * 1103515245u + 12345u;
            count = (seed % (kMaxVerts - 1)) + 1;
        }

        for (size_t v = 0; v < (size_t)count * 3; ++v) {
            seed = seed * 1103515245u + 12345u;
            float val = (float)((int)(seed >> 16) - 16384) * 0.125f;
            vert_buf[v] = val;
        }

        memset(theirs, 0xCC, sizeof(theirs));
        memset(mine, 0xDD, sizeof(mine));

        __try {
            original(theirs, vert_buf, count);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            Log("[AabbTransform] SelfTestFromVertices: original client routine faulted on case %d - not installing", c);
            return false;
        }

        AABB_FromVertices_SSE2(mine, vert_buf, count);

        if (memcmp(theirs, mine, sizeof(theirs)) != 0) {
            Log("[AabbTransform] SelfTestFromVertices: MISMATCH on case %d (count=%u)!\n"
                "  Client: min=(%.6f, %.6f, %.6f) max=(%.6f, %.6f, %.6f)\n"
                "  Ours:   min=(%.6f, %.6f, %.6f) max=(%.6f, %.6f, %.6f)",
                c, count,
                theirs[0], theirs[1], theirs[2], theirs[3], theirs[4], theirs[5],
                mine[0], mine[1], mine[2], mine[3], mine[4], mine[5]);
            return false;
        }
    }

    Log("[AabbTransform] SelfTestFromVertices: passed %d test cases with 100%% bit-exact match against client.", CASES);
    return true;
}
#endif

}  // namespace

bool Init() {
    if (!Config::g_settings.OptAabbTransform) {
        Log("[AabbTransform] not installed: switched off.");
        return false;
    }

    if (!WowOpt_ClientPatchAllowed((const void*)kTransform4x4) ||
        !WowOpt_ClientPatchAllowed((const void*)kTransform3x3)
#if !TEST_DISABLE_AABB_FROM_VERTS_SSE2
        || !WowOpt_ClientPatchAllowed((const void*)kFromVertices)
#endif
    ) {
        Log("[AabbTransform] not installed: No Client Patches is active.");
        return false;
    }

    if (IsBadReadPtr((void*)kTransform4x4, 16) ||
        IsBadReadPtr((void*)kTransform3x3, 16)
#if !TEST_DISABLE_AABB_FROM_VERTS_SSE2
        || IsBadReadPtr((void*)kFromVertices, 16)
#endif
    ) {
        Log("[AabbTransform] unreadable targets - not installing.");
        return false;
    }

    const unsigned char* p4 = (const unsigned char*)kTransform4x4;
    const unsigned char* p3 = (const unsigned char*)kTransform3x3;
    if (p4[0] != 0x55 || p4[1] != 0x8B || p4[2] != 0xEC ||
        p3[0] != 0x55 || p3[1] != 0x8B || p3[2] != 0xEC
#if !TEST_DISABLE_AABB_FROM_VERTS_SSE2
        || ((const unsigned char*)kFromVertices)[0] != 0x55
        || ((const unsigned char*)kFromVertices)[1] != 0x8B
        || ((const unsigned char*)kFromVertices)[2] != 0xEC
#endif
    ) {
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

#if !TEST_DISABLE_AABB_FROM_VERTS_SSE2
    if (!SelfTestFromVertices()) {
        Log("[AabbTransform] SelfTestFromVertices failed - not installing FromVertices hook.");
    } else if (WineSafe_CreateHook((void*)kFromVertices, (void*)Hooked_FromVertices,
                                   (void**)&orig_FromVertices) == MH_OK &&
               WO_EnableHook((void*)kFromVertices) == MH_OK) {
        ok++;
        SamplingProfiler::RegisterSelfSymbol("AabbFromVertices_SSE2", (const void*)&Hooked_FromVertices);
    }
#endif

    if (ok == 0) {
        Log("[AabbTransform] not installed: MinHook failed on transform evaluators.");
        return false;
    }

    g_installed = true;
    SamplingProfiler::RegisterSelfSymbol("AabbTransform4x4_SSE2", (const void*)&Hooked_Transform4x4);
    SamplingProfiler::RegisterSelfSymbol("AabbTransform3x3_SSE2", (const void*)&Hooked_Transform3x3);

    g_abSubject = AbTest::IsSubject("AabbTransform", &g_abSubject);

    Log("[AabbTransform] ACTIVE on %d of %d bounding box routines (sub_7F9430, sub_7F93D0%s). "
        "Evaluates box transform and vertex bounding in hardware SSE2. "
        "Verifying first %lu calls bit for bit with ongoing resampling.%s",
        ok,
#if !TEST_DISABLE_AABB_FROM_VERTS_SSE2
        3, ", sub_984930",
#else
        2, "",
#endif
        kVerifyFirst,
        Config::g_settings.OptAbTest
            ? " The A/B harness alternates the switch between stints."
            : "");
    return true;
}

void Shutdown() {
    if (!g_installed) return;
    MH_DisableHook((void*)kTransform4x4);
    MH_DisableHook((void*)kTransform3x3);
#if !TEST_DISABLE_AABB_FROM_VERTS_SSE2
    if (orig_FromVertices) {
        MH_DisableHook((void*)kFromVertices);
    }
#endif
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
    const unsigned long total = g_calls4x4 + g_calls3x3
#if !TEST_DISABLE_AABB_FROM_VERTS_SSE2
        + g_callsFromVerts
#endif
        ;
    if (total == 0) {
        Log("[AabbTransform] measured and zero: the hooks are in and none was reached.");
        return;
    }
#if !TEST_DISABLE_AABB_FROM_VERTS_SSE2
    Log("[AabbTransform] calls: 4x4=%lu (verified=%lu, armed=%s) | "
        "3x3=%lu (verified=%lu, armed=%s) | "
        "from_verts=%lu (verified=%lu, armed=%s)",
        g_calls4x4, g_verified4x4, g_armed4x4 ? "yes" : "no",
        g_calls3x3, g_verified3x3, g_armed3x3 ? "yes" : "no",
        g_callsFromVerts, g_verifiedFromVerts, g_armedFromVerts ? "yes" : "no");
#else
    Log("[AabbTransform] calls: 4x4=%lu (verified=%lu, armed=%s) | "
        "3x3=%lu (verified=%lu, armed=%s)",
        g_calls4x4, g_verified4x4, g_armed4x4 ? "yes" : "no",
        g_calls3x3, g_verified3x3, g_armed3x3 ? "yes" : "no");
#endif
}

}  // namespace AabbTransform
