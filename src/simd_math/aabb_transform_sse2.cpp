// ============================================================================
// Module: aabb_transform_sse2
//
// Hardware double-precision SSE2 rewrite of bounding-box and coordinate math:
//   sub_7F9430: AABB_Transform       (4x4 matrix * AABB transformation, world/scene engine)
//   sub_7F93D0: AABB_Transform3x3    (3x3 matrix * AABB transformation, orientation only)
//   sub_984860: AABB_TransformAffine (4x4 matrix * AABB transformation, math library / M2 bounds)
//   sub_984930: AABB_FromVertices    (bounding box [min, max] calculation from vertex stream)
//   sub_715130: CAxisAlignedBox::Union (in-place bounding box union)
//   sub_714D10: Vec3_Min             (component-wise minimum of two 3D vectors)
//   sub_714D70: Vec3_Max             (component-wise maximum of two 3D vectors)
//
// Background & Analysis:
//   During scene graph visibility traversal (sub_7A50C0 and 21 other callers
//   across the rendering, culling, and collision pipeline), the client transforms
//   an object-space axis-aligned bounding box into world-space before performing
//   culling tests (such as AABB_Overlap at sub_78F370 or frustum intersection).
//
//   sub_7F9430 and sub_7F93D0 delegate their core computation to sub_7F9320,
//   and sub_984860 implements the standalone math engine version of Jim Arvo's
//   algorithm (Graphics Gems I) invoked during M2 model bounding updates (sub_825750,
//   sub_825A60) and terrain rendering. For each of the three output coordinates (X, Y, Z),
//   they perform three pairs of x87 multiplications and compares:
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
//   sub_715130 (CAxisAlignedBox::Union) and sub_714D10 / sub_714D70 (Vec3_Min / Vec3_Max)
//   are called across M2 animation bounds updates and terrain culling to combine
//   boxes and calculate coordinate extrema, originally hopping through 3 nested
//   stack frames and serial x87 branches. Vectorized using parallel SSE2 minps/maxps.
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

constexpr uintptr_t kTransform4x4    = 0x007F9430;
constexpr uintptr_t kTransform3x3    = 0x007F93D0;
constexpr uintptr_t kTransformAffine = 0x00984860;
#if !TEST_DISABLE_AABB_FROM_VERTS_SSE2
constexpr uintptr_t kFromVertices    = 0x00984930;
#endif
constexpr uintptr_t kAabbUnion       = 0x00715130;
constexpr uintptr_t kVec3Min         = 0x00714D10;
constexpr uintptr_t kVec3Max         = 0x00714D70;

typedef void   (__cdecl* Transform4x4_fn)(const float* matrix, const float* in_aabb, float* out_aabb);
typedef void   (__cdecl* Transform3x3_fn)(const float* matrix, const float* in_aabb, float* out_aabb);
typedef float* (__cdecl* TransformAffine_fn)(float* out_box, const float* in_box, const float* mat4x4);
#if !TEST_DISABLE_AABB_FROM_VERTS_SSE2
typedef float* (__cdecl* FromVertices_fn)(float* out_box, const float* verts, uint32_t count);
#endif
typedef float* (__fastcall* AabbUnion_fn)(float* this_box, void* edx, float* out_box, const float* other_box);
typedef float* (__cdecl* Vec3Min_fn)(float* out_vec, const float* a, const float* b);
typedef float* (__cdecl* Vec3Max_fn)(float* out_vec, const float* a, const float* b);

Transform4x4_fn    orig_Transform4x4    = nullptr;
Transform3x3_fn    orig_Transform3x3    = nullptr;
TransformAffine_fn orig_TransformAffine = nullptr;
#if !TEST_DISABLE_AABB_FROM_VERTS_SSE2
FromVertices_fn    orig_FromVertices    = nullptr;
#endif
AabbUnion_fn       orig_AabbUnion       = nullptr;
Vec3Min_fn         orig_Vec3Min         = nullptr;
Vec3Max_fn         orig_Vec3Max         = nullptr;

bool g_installed      = false;
bool g_armed4x4       = false;
bool g_armed3x3       = false;
bool g_armedAffine    = false;
#if !TEST_DISABLE_AABB_FROM_VERTS_SSE2
bool g_armedFromVerts = false;
#endif
bool g_armedUnion     = false;
bool g_armedVec3Min   = false;
bool g_armedVec3Max   = false;
bool g_abSubject      = false;
bool g_dead           = false;

unsigned long g_calls4x4       = 0;
unsigned long g_calls3x3       = 0;
unsigned long g_callsAffine    = 0;
#if !TEST_DISABLE_AABB_FROM_VERTS_SSE2
unsigned long g_callsFromVerts = 0;
#endif
unsigned long g_callsUnion     = 0;
unsigned long g_callsVec3Min   = 0;
unsigned long g_callsVec3Max   = 0;

unsigned long g_verified4x4       = 0;
unsigned long g_verified3x3       = 0;
unsigned long g_verifiedAffine    = 0;
#if !TEST_DISABLE_AABB_FROM_VERTS_SSE2
unsigned long g_verifiedFromVerts = 0;
#endif
unsigned long g_verifiedUnion     = 0;
unsigned long g_verifiedVec3Min   = 0;
unsigned long g_verifiedVec3Max   = 0;

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

// 4x4 Matrix transformation (sub_7F9430 and sub_984860)
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

// Vector extrema & AABB Union (sub_714D10, sub_714D70, sub_715130)
inline float* Vec3_Min_SSE2(float* out_vec, const float* a, const float* b) {
    if (!out_vec || !a || !b) return out_vec;
    const __m128 va = _mm_setr_ps(a[0], a[1], a[2], 0.0f);
    const __m128 vb = _mm_setr_ps(b[0], b[1], b[2], 0.0f);
    const __m128 vr = _mm_min_ps(va, vb);
    alignas(16) float buf[4];
    _mm_store_ps(buf, vr);
    out_vec[0] = buf[0];
    out_vec[1] = buf[1];
    out_vec[2] = buf[2];
    return out_vec;
}

inline float* Vec3_Max_SSE2(float* out_vec, const float* a, const float* b) {
    if (!out_vec || !a || !b) return out_vec;
    const __m128 va = _mm_setr_ps(a[0], a[1], a[2], 0.0f);
    const __m128 vb = _mm_setr_ps(b[0], b[1], b[2], 0.0f);
    const __m128 vr = _mm_max_ps(va, vb);
    alignas(16) float buf[4];
    _mm_store_ps(buf, vr);
    out_vec[0] = buf[0];
    out_vec[1] = buf[1];
    out_vec[2] = buf[2];
    return out_vec;
}

inline float* AABB_Union_SSE2(float* this_box, float* out_box, const float* other_box) {
    if (!this_box || !other_box) return out_box;
    const __m128 t_min = _mm_setr_ps(this_box[0], this_box[1], this_box[2], 0.0f);
    const __m128 o_min = _mm_setr_ps(other_box[0], other_box[1], other_box[2], 0.0f);
    const __m128 t_max = _mm_setr_ps(this_box[3], this_box[4], this_box[5], 0.0f);
    const __m128 o_max = _mm_setr_ps(other_box[3], other_box[4], other_box[5], 0.0f);

    const __m128 r_min = _mm_min_ps(t_min, o_min);
    const __m128 r_max = _mm_max_ps(t_max, o_max);

    alignas(16) float buf_min[4];
    alignas(16) float buf_max[4];
    _mm_store_ps(buf_min, r_min);
    _mm_store_ps(buf_max, r_max);

    this_box[0] = buf_min[0];
    this_box[1] = buf_min[1];
    this_box[2] = buf_min[2];
    this_box[3] = buf_max[0];
    this_box[4] = buf_max[1];
    this_box[5] = buf_max[2];

    if (out_box) {
        out_box[0] = buf_min[0];
        out_box[1] = buf_min[1];
        out_box[2] = buf_min[2];
        out_box[3] = buf_max[0];
        out_box[4] = buf_max[1];
        out_box[5] = buf_max[2];
    }
    return out_box;
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

float* Hooked_TransformAffineBody(float* out_aabb, const float* in_aabb, const float* m) {
    g_callsAffine++;
    if (g_dead) {
        return orig_TransformAffine(out_aabb, in_aabb, m);
    }

    if (!g_armedAffine || (g_callsAffine & kResampleMask) == 0) {
        float theirs[6];
        float mine[6];
        orig_TransformAffine(theirs, in_aabb, m);
        EvaluateTransform4x4(m, in_aabb, mine);

        if (memcmp(theirs, mine, sizeof(theirs)) != 0) {
            g_dead = true;
            Verdict::Add(Verdict::Bad, "AABB_TransformAffine disagreed with client; retired for session");
            Log("[AabbTransform] DISAGREED on AABB_TransformAffine at call %lu - retired for session", g_callsAffine);
            if (out_aabb) memcpy(out_aabb, theirs, sizeof(theirs));
            return out_aabb;
        }

        g_verifiedAffine++;
        if (!g_armedAffine && g_verifiedAffine >= kVerifyFirst) {
            g_armedAffine = true;
            Log("[AabbTransform] AABB_TransformAffine armed: %lu calls agreed with client exactly, now answering directly (resampling 1 in %lu)",
                g_verifiedAffine, kResampleMask + 1);
        }
        if (out_aabb) memcpy(out_aabb, theirs, sizeof(theirs));
        return out_aabb;
    }

    EvaluateTransform4x4(m, in_aabb, out_aabb);
    return out_aabb;
}

float* Hooked_AabbUnionBody(float* this_box, void* edx, float* out_box, const float* other_box) {
    g_callsUnion++;
    if (g_dead) {
        return orig_AabbUnion(this_box, edx, out_box, other_box);
    }

    if (!g_armedUnion || (g_callsUnion & kResampleMask) == 0) {
        float theirs_this[6];
        float theirs_out[6];
        float mine_this[6];
        float mine_out[6];

        memcpy(theirs_this, this_box, sizeof(theirs_this));
        memcpy(mine_this, this_box, sizeof(mine_this));

        orig_AabbUnion(theirs_this, edx, theirs_out, other_box);
        AABB_Union_SSE2(mine_this, mine_out, other_box);

        if (memcmp(theirs_this, mine_this, sizeof(theirs_this)) != 0 ||
            memcmp(theirs_out, mine_out, sizeof(theirs_out)) != 0) {
            g_dead = true;
            Verdict::Add(Verdict::Bad, "AABB_Union disagreed with client; retired for session");
            Log("[AabbTransform] DISAGREED on AABB_Union at call %lu - retired for session", g_callsUnion);
            memcpy(this_box, theirs_this, sizeof(theirs_this));
            if (out_box) memcpy(out_box, theirs_out, sizeof(theirs_out));
            return out_box;
        }

        g_verifiedUnion++;
        if (!g_armedUnion && g_verifiedUnion >= kVerifyFirst) {
            g_armedUnion = true;
            Log("[AabbTransform] AABB_Union armed: %lu calls agreed with client exactly, now answering directly (resampling 1 in %lu)",
                g_verifiedUnion, kResampleMask + 1);
        }
        memcpy(this_box, theirs_this, sizeof(theirs_this));
        if (out_box) memcpy(out_box, theirs_out, sizeof(theirs_out));
        return out_box;
    }

    return AABB_Union_SSE2(this_box, out_box, other_box);
}

float* Hooked_Vec3MinBody(float* out_vec, const float* a, const float* b) {
    g_callsVec3Min++;
    if (g_dead) {
        return orig_Vec3Min(out_vec, a, b);
    }

    if (!g_armedVec3Min || (g_callsVec3Min & kResampleMask) == 0) {
        float theirs[3];
        float mine[3];
        orig_Vec3Min(theirs, a, b);
        Vec3_Min_SSE2(mine, a, b);

        if (memcmp(theirs, mine, sizeof(theirs)) != 0) {
            g_dead = true;
            Verdict::Add(Verdict::Bad, "Vec3_Min disagreed with client; retired for session");
            Log("[AabbTransform] DISAGREED on Vec3_Min at call %lu - retired for session", g_callsVec3Min);
            if (out_vec) memcpy(out_vec, theirs, sizeof(theirs));
            return out_vec;
        }

        g_verifiedVec3Min++;
        if (!g_armedVec3Min && g_verifiedVec3Min >= kVerifyFirst) {
            g_armedVec3Min = true;
            Log("[AabbTransform] Vec3_Min armed: %lu calls agreed with client exactly, now answering directly (resampling 1 in %lu)",
                g_verifiedVec3Min, kResampleMask + 1);
        }
        if (out_vec) memcpy(out_vec, theirs, sizeof(theirs));
        return out_vec;
    }

    return Vec3_Min_SSE2(out_vec, a, b);
}

float* Hooked_Vec3MaxBody(float* out_vec, const float* a, const float* b) {
    g_callsVec3Max++;
    if (g_dead) {
        return orig_Vec3Max(out_vec, a, b);
    }

    if (!g_armedVec3Max || (g_callsVec3Max & kResampleMask) == 0) {
        float theirs[3];
        float mine[3];
        orig_Vec3Max(theirs, a, b);
        Vec3_Max_SSE2(mine, a, b);

        if (memcmp(theirs, mine, sizeof(theirs)) != 0) {
            g_dead = true;
            Verdict::Add(Verdict::Bad, "Vec3_Max disagreed with client; retired for session");
            Log("[AabbTransform] DISAGREED on Vec3_Max at call %lu - retired for session", g_callsVec3Max);
            if (out_vec) memcpy(out_vec, theirs, sizeof(theirs));
            return out_vec;
        }

        g_verifiedVec3Max++;
        if (!g_armedVec3Max && g_verifiedVec3Max >= kVerifyFirst) {
            g_armedVec3Max = true;
            Log("[AabbTransform] Vec3_Max armed: %lu calls agreed with client exactly, now answering directly (resampling 1 in %lu)",
                g_verifiedVec3Max, kResampleMask + 1);
        }
        if (out_vec) memcpy(out_vec, theirs, sizeof(theirs));
        return out_vec;
    }

    return Vec3_Max_SSE2(out_vec, a, b);
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

float* __cdecl Hooked_TransformAffine(float* out_aabb, const float* in_aabb, const float* m) {
    if (!g_abSubject) {
        return Hooked_TransformAffineBody(out_aabb, in_aabb, m);
    }
    const unsigned long long t = AbTest::TickIn();
    float* res = nullptr;
    __try {
        if (AbTest::StandAside()) {
            res = orig_TransformAffine(out_aabb, in_aabb, m);
        } else {
            res = Hooked_TransformAffineBody(out_aabb, in_aabb, m);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        g_dead = true;
        res = orig_TransformAffine(out_aabb, in_aabb, m);
    }
    AbTest::TickOut(t);
    return res;
}

float* __fastcall Hooked_AabbUnion(float* this_box, void* edx, float* out_box, const float* other_box) {
    if (!g_abSubject) {
        return Hooked_AabbUnionBody(this_box, edx, out_box, other_box);
    }
    const unsigned long long t = AbTest::TickIn();
    float* res = nullptr;
    __try {
        if (AbTest::StandAside()) {
            res = orig_AabbUnion(this_box, edx, out_box, other_box);
        } else {
            res = Hooked_AabbUnionBody(this_box, edx, out_box, other_box);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        g_dead = true;
        res = orig_AabbUnion(this_box, edx, out_box, other_box);
    }
    AbTest::TickOut(t);
    return res;
}

float* __cdecl Hooked_Vec3Min(float* out_vec, const float* a, const float* b) {
    if (!g_abSubject) {
        return Hooked_Vec3MinBody(out_vec, a, b);
    }
    const unsigned long long t = AbTest::TickIn();
    float* res = nullptr;
    __try {
        if (AbTest::StandAside()) {
            res = orig_Vec3Min(out_vec, a, b);
        } else {
            res = Hooked_Vec3MinBody(out_vec, a, b);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        g_dead = true;
        res = orig_Vec3Min(out_vec, a, b);
    }
    AbTest::TickOut(t);
    return res;
}

float* __cdecl Hooked_Vec3Max(float* out_vec, const float* a, const float* b) {
    if (!g_abSubject) {
        return Hooked_Vec3MaxBody(out_vec, a, b);
    }
    const unsigned long long t = AbTest::TickIn();
    float* res = nullptr;
    __try {
        if (AbTest::StandAside()) {
            res = orig_Vec3Max(out_vec, a, b);
        } else {
            res = Hooked_Vec3MaxBody(out_vec, a, b);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        g_dead = true;
        res = orig_Vec3Max(out_vec, a, b);
    }
    AbTest::TickOut(t);
    return res;
}

#if !TEST_DISABLE_AABB_FROM_VERTS_SSE2
// ============================================================================
// sub_984930 (AABB_FromVertices) Vectorized Implementation
//
// Evaluates component-wise min and max across an arbitrary array of 3D vertices
// (3 floats per vertex: X, Y, Z).
//
// In stock x87, the loop is 4-way unrolled with 6 fcom / fnstsw sequences per
// vertex (18 branch evaluations per 4 vertices). Under SSE2, we load 4 vertices
// across three 128-bit registers (X0..X3, Y0..Y3, Z0..Z3) or sequential minps/maxps,
// computing component bounds in parallel registers.
// ============================================================================

inline float* AABB_FromVertices_SSE2(float* out_box, const float* verts, uint32_t count) {
    if (!out_box) return nullptr;
    if (!verts || count == 0) return out_box;

    float min_x = verts[0], max_x = verts[0];
    float min_y = verts[1], max_y = verts[1];
    float min_z = verts[2], max_z = verts[2];

    uint32_t i = 1;

    // Process 4 vertices per iteration using SSE2 minps/maxps
    __m128 vmin_x = _mm_set1_ps(min_x);
    __m128 vmax_x = _mm_set1_ps(max_x);
    __m128 vmin_y = _mm_set1_ps(min_y);
    __m128 vmax_y = _mm_set1_ps(max_y);
    __m128 vmin_z = _mm_set1_ps(min_z);
    __m128 vmax_z = _mm_set1_ps(max_z);

    for (; i + 3 < count; i += 4) {
        const float* v = verts + i * 3;

        // Load 4 3D vertices (12 floats total)
        // v[0..2], v[3..5], v[6..8], v[9..11]
        const __m128 v0 = _mm_loadu_ps(v + 0);  // x0, y0, z0, x1
        const __m128 v1 = _mm_loadu_ps(v + 4);  // y1, z1, x2, y2
        const __m128 v2 = _mm_loadu_ps(v + 8);  // z2, x3, y3, z3

        // Unpack and transpose coordinates into separate X, Y, Z vectors
        // Coordinates:
        // x0 = v[0], x1 = v[3], x2 = v[6], x3 = v[9]
        // y0 = v[1], y1 = v[4], y2 = v[7], y3 = v[10]
        // z0 = v[2], z1 = v[5], z2 = v[8], z3 = v[11]
        const __m128 x = _mm_setr_ps(v[0], v[3], v[6], v[9]);
        const __m128 y = _mm_setr_ps(v[1], v[4], v[7], v[10]);
        const __m128 z = _mm_setr_ps(v[2], v[5], v[8], v[11]);

        vmin_x = _mm_min_ps(vmin_x, x);
        vmax_x = _mm_max_ps(vmax_x, x);
        vmin_y = _mm_min_ps(vmin_y, y);
        vmax_y = _mm_max_ps(vmax_y, y);
        vmin_z = _mm_min_ps(vmin_z, z);
        vmax_z = _mm_max_ps(vmax_z, z);
    }

    // Horizontal reduction of vector accumulators
    alignas(16) float buf_min_x[4], buf_max_x[4];
    alignas(16) float buf_min_y[4], buf_max_y[4];
    alignas(16) float buf_min_z[4], buf_max_z[4];

    _mm_store_ps(buf_min_x, vmin_x);
    _mm_store_ps(buf_max_x, vmax_x);
    _mm_store_ps(buf_min_y, vmin_y);
    _mm_store_ps(buf_max_y, vmax_y);
    _mm_store_ps(buf_min_z, vmin_z);
    _mm_store_ps(buf_max_z, vmax_z);

    for (int k = 0; k < 4; ++k) {
        if (buf_min_x[k] < min_x) min_x = buf_min_x[k];
        if (buf_max_x[k] > max_x) max_x = buf_max_x[k];
        if (buf_min_y[k] < min_y) min_y = buf_min_y[k];
        if (buf_max_y[k] > max_y) max_y = buf_max_y[k];
        if (buf_min_z[k] < min_z) min_z = buf_min_z[k];
        if (buf_max_z[k] > max_z) max_z = buf_max_z[k];
    }

    // Scalar tail for remaining vertices
    for (; i < count; ++i) {
        const float x = verts[i * 3 + 0];
        const float y = verts[i * 3 + 1];
        const float z = verts[i * 3 + 2];

        if (x < min_x) min_x = x;
        if (x > max_x) max_x = x;
        if (y < min_y) min_y = y;
        if (y > max_y) max_y = y;
        if (z < min_z) min_z = z;
        if (z > max_z) max_z = z;
    }

    out_box[0] = min_x;
    out_box[1] = min_y;
    out_box[2] = min_z;
    out_box[3] = max_x;
    out_box[4] = max_y;
    out_box[5] = max_z;
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
            Log("[AabbTransform] DISAGREED on AABB_FromVertices at call %lu (count=%u) - retired for session",
                g_callsFromVerts, count);
            memcpy(out_box, theirs, sizeof(theirs));
            return out_box;
        }

        g_verifiedFromVerts++;
        if (!g_armedFromVerts && g_verifiedFromVerts >= kVerifyFirst) {
            g_armedFromVerts = true;
            Log("[AabbTransform] AABB_FromVertices armed: %lu calls agreed with client exactly, now answering directly (resampling 1 in %lu)",
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
    unsigned seed = 0x1234ABCDu;

    constexpr size_t kMaxVerts = 128;
    float vert_buf[kMaxVerts * 3];
    float theirs[6];
    float mine[6];

    for (int c = 0; c < CASES; ++c) {
        uint32_t count;
        if (c == 0) count = 1;
        else if (c == 1) count = 2;
        else if (c == 2) count = 3;
        else if (c == 3) count = 4;
        else if (c == 4) count = 5;
        else if (c == 5) count = 7;
        else if (c == 6) count = 8;
        else if (c == 7) count = 9;
        else if (c == 8) count = 12;
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

// Startup self-tests for affine transform, union, and min/max
static bool SelfTestTransformAffine() {
    TransformAffine_fn original = (TransformAffine_fn)kTransformAffine;
    const int CASES = 20000;
    unsigned seed = 0x6543210Fu;

    for (int c = 0; c < CASES; ++c) {
        float mat[16];
        for (int i = 0; i < 16; ++i) {
            seed = seed * 1103515245u + 12345u;
            mat[i] = (float)((int)(seed >> 16) - 16384) * 0.03125f;
        }

        float in_box[6];
        for (int i = 0; i < 3; ++i) {
            seed = seed * 1103515245u + 12345u;
            const float v1 = (float)((int)(seed >> 16) - 16384) * 0.25f;
            seed = seed * 1103515245u + 12345u;
            const float v2 = (float)((int)(seed >> 16) - 16384) * 0.25f;
            in_box[i]     = (v1 < v2) ? v1 : v2;
            in_box[i + 3] = (v1 < v2) ? v2 : v1;
        }

        float theirs[6] = { 0 };
        float mine[6]   = { 0 };

        __try {
            original(theirs, in_box, mat);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            Log("[AabbTransform] SelfTestTransformAffine: original routine faulted on case %d - not installing", c);
            return false;
        }

        EvaluateTransform4x4(mat, in_box, mine);

        if (memcmp(theirs, mine, sizeof(theirs)) != 0) {
            Log("[AabbTransform] SelfTestTransformAffine: MISMATCH on case %d!\n"
                "  Client: min=(%.6f, %.6f, %.6f) max=(%.6f, %.6f, %.6f)\n"
                "  Ours:   min=(%.6f, %.6f, %.6f) max=(%.6f, %.6f, %.6f)",
                c,
                theirs[0], theirs[1], theirs[2], theirs[3], theirs[4], theirs[5],
                mine[0], mine[1], mine[2], mine[3], mine[4], mine[5]);
            return false;
        }
    }

    Log("[AabbTransform] SelfTestTransformAffine: passed %d test cases with 100%% bit-exact match against client.", CASES);
    return true;
}

static bool SelfTestAabbUnion() {
    AabbUnion_fn original = (AabbUnion_fn)kAabbUnion;
    const int CASES = 20000;
    unsigned seed = 0x76543210u;

    for (int c = 0; c < CASES; ++c) {
        float box1[6];
        float box2[6];
        for (int i = 0; i < 3; ++i) {
            seed = seed * 1103515245u + 12345u;
            float v1 = (float)((int)(seed >> 16) - 16384) * 0.25f;
            seed = seed * 1103515245u + 12345u;
            float v2 = (float)((int)(seed >> 16) - 16384) * 0.25f;
            box1[i]     = (v1 < v2) ? v1 : v2;
            box1[i + 3] = (v1 < v2) ? v2 : v1;

            seed = seed * 1103515245u + 12345u;
            v1 = (float)((int)(seed >> 16) - 16384) * 0.25f;
            seed = seed * 1103515245u + 12345u;
            v2 = (float)((int)(seed >> 16) - 16384) * 0.25f;
            box2[i]     = (v1 < v2) ? v1 : v2;
            box2[i + 3] = (v1 < v2) ? v2 : v1;
        }

        // Include exact coordinate ties
        if (c % 5 == 0) {
            box2[0] = box1[0];
            box2[3] = box1[3];
        }

        float theirs_this[6];
        float theirs_out[6] = { 0 };
        float mine_this[6];
        float mine_out[6]   = { 0 };

        memcpy(theirs_this, box1, sizeof(theirs_this));
        memcpy(mine_this, box1, sizeof(mine_this));

        __try {
            original(theirs_this, nullptr, theirs_out, box2);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            Log("[AabbTransform] SelfTestAabbUnion: original routine faulted on case %d - not installing", c);
            return false;
        }

        AABB_Union_SSE2(mine_this, mine_out, box2);

        if (memcmp(theirs_this, mine_this, sizeof(theirs_this)) != 0 ||
            memcmp(theirs_out, mine_out, sizeof(theirs_out)) != 0) {
            Log("[AabbTransform] SelfTestAabbUnion: MISMATCH on case %d!", c);
            return false;
        }
    }

    Log("[AabbTransform] SelfTestAabbUnion: passed %d test cases with 100%% bit-exact match against client.", CASES);
    return true;
}

static bool SelfTestVec3MinMax() {
    Vec3Min_fn orig_min = (Vec3Min_fn)kVec3Min;
    Vec3Max_fn orig_max = (Vec3Max_fn)kVec3Max;
    const int CASES = 20000;
    unsigned seed = 0x87654321u;

    for (int c = 0; c < CASES; ++c) {
        float a[3], b[3];
        for (int i = 0; i < 3; ++i) {
            seed = seed * 1103515245u + 12345u;
            a[i] = (float)((int)(seed >> 16) - 16384) * 0.25f;
            seed = seed * 1103515245u + 12345u;
            b[i] = (float)((int)(seed >> 16) - 16384) * 0.25f;
        }

        // Include exact coordinate ties
        if (c % 4 == 0) b[0] = a[0];
        if (c % 6 == 0) b[1] = a[1];
        if (c % 8 == 0) b[2] = a[2];

        float theirs_min[3] = { 0 };
        float mine_min[3]   = { 0 };
        float theirs_max[3] = { 0 };
        float mine_max[3]   = { 0 };

        __try {
            orig_min(theirs_min, a, b);
            orig_max(theirs_max, a, b);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            Log("[AabbTransform] SelfTestVec3MinMax: original routine faulted on case %d - not installing", c);
            return false;
        }

        Vec3_Min_SSE2(mine_min, a, b);
        Vec3_Max_SSE2(mine_max, a, b);

        if (memcmp(theirs_min, mine_min, sizeof(theirs_min)) != 0) {
            Log("[AabbTransform] SelfTestVec3MinMax: MISMATCH on min case %d!\n"
                "  Client: (%.6f, %.6f, %.6f)\n"
                "  Ours:   (%.6f, %.6f, %.6f)",
                c, theirs_min[0], theirs_min[1], theirs_min[2],
                mine_min[0], mine_min[1], mine_min[2]);
            return false;
        }

        if (memcmp(theirs_max, mine_max, sizeof(theirs_max)) != 0) {
            Log("[AabbTransform] SelfTestVec3MinMax: MISMATCH on max case %d!\n"
                "  Client: (%.6f, %.6f, %.6f)\n"
                "  Ours:   (%.6f, %.6f, %.6f)",
                c, theirs_max[0], theirs_max[1], theirs_max[2],
                mine_max[0], mine_max[1], mine_max[2]);
            return false;
        }
    }

    Log("[AabbTransform] SelfTestVec3MinMax: passed %d test cases with 100%% bit-exact match against client.", CASES);
    return true;
}

}  // namespace

bool Init() {
    if (!Config::g_settings.OptAabbTransform) {
        Log("[AabbTransform] not installed: switched off.");
        return false;
    }

    if (!WowOpt_ClientPatchAllowed((const void*)kTransform4x4) ||
        !WowOpt_ClientPatchAllowed((const void*)kTransform3x3) ||
        !WowOpt_ClientPatchAllowed((const void*)kTransformAffine) ||
        !WowOpt_ClientPatchAllowed((const void*)kAabbUnion) ||
        !WowOpt_ClientPatchAllowed((const void*)kVec3Min) ||
        !WowOpt_ClientPatchAllowed((const void*)kVec3Max)
#if !TEST_DISABLE_AABB_FROM_VERTS_SSE2
        || !WowOpt_ClientPatchAllowed((const void*)kFromVertices)
#endif
    ) {
        Log("[AabbTransform] not installed: No Client Patches is active.");
        return false;
    }

    if (IsBadReadPtr((void*)kTransform4x4, 16) ||
        IsBadReadPtr((void*)kTransform3x3, 16) ||
        IsBadReadPtr((void*)kTransformAffine, 16) ||
        IsBadReadPtr((void*)kAabbUnion, 16) ||
        IsBadReadPtr((void*)kVec3Min, 16) ||
        IsBadReadPtr((void*)kVec3Max, 16)
#if !TEST_DISABLE_AABB_FROM_VERTS_SSE2
        || IsBadReadPtr((void*)kFromVertices, 16)
#endif
    ) {
        Log("[AabbTransform] unreadable targets - not installing.");
        return false;
    }

    const unsigned char* p4      = (const unsigned char*)kTransform4x4;
    const unsigned char* p3      = (const unsigned char*)kTransform3x3;
    const unsigned char* pAffine = (const unsigned char*)kTransformAffine;
    const unsigned char* pUnion  = (const unsigned char*)kAabbUnion;
    const unsigned char* pVMin   = (const unsigned char*)kVec3Min;
    const unsigned char* pVMax   = (const unsigned char*)kVec3Max;

    if (p4[0]      != 0x55 || p4[1]      != 0x8B || p4[2]      != 0xEC ||
        p3[0]      != 0x55 || p3[1]      != 0x8B || p3[2]      != 0xEC ||
        pAffine[0] != 0x55 || pAffine[1] != 0x8B || pAffine[2] != 0xEC ||
        pUnion[0]  != 0x55 || pUnion[1]  != 0x8B || pUnion[2]  != 0xEC ||
        pVMin[0]   != 0x55 || pVMin[1]   != 0x8B || pVMin[2]   != 0xEC ||
        pVMax[0]   != 0x55 || pVMax[1]   != 0x8B || pVMax[2]   != 0xEC
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

    if (!SelfTestTransformAffine() || !SelfTestAabbUnion() || !SelfTestVec3MinMax()) {
        Log("[AabbTransform] startup self-tests failed - not installing.");
        return false;
    }

    int ok = 0;
    if (WineSafe_CreateHook((void*)kTransform4x4, (void*)Hooked_Transform4x4,
                            (void**)&orig_Transform4x4) == MH_OK &&
        WO_EnableHook((void*)kTransform4x4) == MH_OK) {
        ok++;
        SamplingProfiler::RegisterSelfSymbol("AabbTransform4x4_SSE2", (const void*)&Hooked_Transform4x4);
    }

    if (WineSafe_CreateHook((void*)kTransform3x3, (void*)Hooked_Transform3x3,
                            (void**)&orig_Transform3x3) == MH_OK &&
        WO_EnableHook((void*)kTransform3x3) == MH_OK) {
        ok++;
        SamplingProfiler::RegisterSelfSymbol("AabbTransform3x3_SSE2", (const void*)&Hooked_Transform3x3);
    }

    if (WineSafe_CreateHook((void*)kTransformAffine, (void*)Hooked_TransformAffine,
                            (void**)&orig_TransformAffine) == MH_OK &&
        WO_EnableHook((void*)kTransformAffine) == MH_OK) {
        ok++;
        SamplingProfiler::RegisterSelfSymbol("AABB_TransformAffine_SSE2", (const void*)&Hooked_TransformAffine);
    }

    if (WineSafe_CreateHook((void*)kAabbUnion, (void*)Hooked_AabbUnion,
                            (void**)&orig_AabbUnion) == MH_OK &&
        WO_EnableHook((void*)kAabbUnion) == MH_OK) {
        ok++;
        SamplingProfiler::RegisterSelfSymbol("CAxisAlignedBox_Union_SSE2", (const void*)&Hooked_AabbUnion);
    }

    if (WineSafe_CreateHook((void*)kVec3Min, (void*)Hooked_Vec3Min,
                            (void**)&orig_Vec3Min) == MH_OK &&
        WO_EnableHook((void*)kVec3Min) == MH_OK) {
        ok++;
        SamplingProfiler::RegisterSelfSymbol("Vec3_Min_SSE2", (const void*)&Hooked_Vec3Min);
    }

    if (WineSafe_CreateHook((void*)kVec3Max, (void*)Hooked_Vec3Max,
                            (void**)&orig_Vec3Max) == MH_OK &&
        WO_EnableHook((void*)kVec3Max) == MH_OK) {
        ok++;
        SamplingProfiler::RegisterSelfSymbol("Vec3_Max_SSE2", (const void*)&Hooked_Vec3Max);
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
    g_abSubject = AbTest::IsSubject("AabbTransform", &g_abSubject);

    Log("[AabbTransform] ACTIVE on %d bounding box and coordinate math routines "
        "(AABB_Transform, AABB_Transform3x3, AABB_TransformAffine, CAxisAlignedBox::Union, Vec3_Min, Vec3_Max%s). "
        "Evaluates box transform, union, and extrema in hardware SSE2. "
        "Verifying first %lu calls bit for bit with ongoing resampling.%s",
        ok,
#if !TEST_DISABLE_AABB_FROM_VERTS_SSE2
        ", AABB_FromVertices",
#else
        "",
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
    MH_DisableHook((void*)kTransformAffine);
    MH_DisableHook((void*)kAabbUnion);
    MH_DisableHook((void*)kVec3Min);
    MH_DisableHook((void*)kVec3Max);
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
    const unsigned long total = g_calls4x4 + g_calls3x3 + g_callsAffine + g_callsUnion + g_callsVec3Min + g_callsVec3Max
#if !TEST_DISABLE_AABB_FROM_VERTS_SSE2
        + g_callsFromVerts
#endif
        ;
    if (total == 0) {
        Log("[AabbTransform] measured and zero: the hooks are in and none was reached.");
        return;
    }
    Log("[AabbTransform] calls: 4x4=%lu (verified=%lu, armed=%s) | "
        "3x3=%lu (verified=%lu, armed=%s) | "
        "affine=%lu (verified=%lu, armed=%s) | "
        "union=%lu (verified=%lu, armed=%s) | "
        "vec3_min=%lu (verified=%lu, armed=%s) | "
        "vec3_max=%lu (verified=%lu, armed=%s)"
#if !TEST_DISABLE_AABB_FROM_VERTS_SSE2
        " | from_verts=%lu (verified=%lu, armed=%s)"
#endif
        ,
        g_calls4x4, g_verified4x4, g_armed4x4 ? "yes" : "no",
        g_calls3x3, g_verified3x3, g_armed3x3 ? "yes" : "no",
        g_callsAffine, g_verifiedAffine, g_armedAffine ? "yes" : "no",
        g_callsUnion, g_verifiedUnion, g_armedUnion ? "yes" : "no",
        g_callsVec3Min, g_verifiedVec3Min, g_armedVec3Min ? "yes" : "no",
        g_callsVec3Max, g_verifiedVec3Max, g_armedVec3Max ? "yes" : "no"
#if !TEST_DISABLE_AABB_FROM_VERTS_SSE2
        , g_callsFromVerts, g_verifiedFromVerts, g_armedFromVerts ? "yes" : "no"
#endif
    );
}

}  // namespace AabbTransform
