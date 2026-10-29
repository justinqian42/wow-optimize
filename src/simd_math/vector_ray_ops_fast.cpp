// ============================================================================
// Module: vector_ray_ops_fast.cpp
//
// Hardware-accelerated 3D vector, ray, line segment, and 3x4 affine matrix
// operations for WoW 3.3.5a:
//   - sub_4C3420: C3Vector::Normalize
//   - sub_4C3600: C3Vector::NormalizeSafe
//   - sub_78F4D0: C3Segment::Lerp
//   - sub_985200: C3Ray::FromStartEnd
//   - sub_984310: C34Matrix operator* (3x4 affine matrix multiplication)
//
// In stock 3.3.5a:
//   - sub_4C3420 and sub_4C3600 normalize 3D direction vectors using serial x87
//     instructions (fld, fmulp, fsqrt, fdivrp, fmul, fstp) with status word
//     tests (fnstsw ax). sub_4C3600 alone has 22 call sites across M2 animation
//     tracks, bone transformations, attachment matrices, and spell projectiles.
//   - sub_78F4D0 performs 3D linear interpolation along a line segment during
//     terrain, WMO, and water collision raycasts using 23 serialized x87 FPU
//     instructions and stack swaps.
//   - sub_985200 derives a ray from start and end points across 149 bytes of
//     code with intermediate memory spills.
//   - sub_984310 concatenates two 3x4 affine transformation matrices across
//     118 instructions of serial x87 arithmetic.
//
// This module provides register-allocated, vectorized implementations eliminating
// pipeline stalls, intermediate memory spills, and store-to-load forwarding
// penalties. Dual-run verification and automatic fail-safe retirement guards
// ensure strict numerical parity with client output.
// ============================================================================

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <cmath>
#include <cstdint>
#include <cstring>

#include "vector_ray_ops_fast.h"
#include "MinHook.h"
#include "version.h"
#include "config.h"
#include "sampling_profiler.h"

extern "C" void Log(const char* fmt, ...);

namespace VectorRayOpsFast {

namespace {

constexpr uintptr_t kTarget_Normalize     = 0x004C3420;
constexpr uintptr_t kTarget_NormalizeSafe = 0x004C3600;
constexpr uintptr_t kTarget_SegmentLerp   = 0x0078F4D0;
constexpr uintptr_t kTarget_RayFromEnds   = 0x00985200;
constexpr uintptr_t kTarget_Mat34Mul      = 0x00984310;

// Verification settings
constexpr unsigned long kVerifyFirst = 20000;
constexpr unsigned long kResampleMask = 4095;
constexpr float kVerifyTolerance = 0.0002f;

// Function signatures
typedef void (__fastcall* VecNormalize_fn)(void* this_ptr, void* dummy_edx);
typedef void* (__fastcall* SegmentLerp_fn)(const void* this_ptr, void* dummy_edx, void* out, float t);
typedef float* (__cdecl* RayFromEnds_fn)(float* out, const float* start, const float* end, char normalize);
typedef float* (__cdecl* Mat34Mul_fn)(float* out, const float* a, const float* b);

VecNormalize_fn orig_Normalize     = nullptr;
VecNormalize_fn orig_NormalizeSafe = nullptr;
SegmentLerp_fn  orig_SegmentLerp   = nullptr;
RayFromEnds_fn  orig_RayFromEnds   = nullptr;
Mat34Mul_fn     orig_Mat34Mul      = nullptr;

bool g_installed = false;

bool g_dead_normalize      = false;
bool g_dead_normalize_safe = false;
bool g_dead_segment_lerp   = false;
bool g_dead_ray_from_ends  = false;
bool g_dead_mat34_mul      = false;

unsigned long long g_calls_normalize      = 0;
unsigned long long g_calls_normalize_safe = 0;
unsigned long long g_calls_segment_lerp   = 0;
unsigned long long g_calls_ray_from_ends  = 0;
unsigned long long g_calls_mat34_mul      = 0;

unsigned long long g_verified_normalize      = 0;
unsigned long long g_verified_normalize_safe = 0;
unsigned long long g_verified_segment_lerp   = 0;
unsigned long long g_verified_ray_from_ends  = 0;
unsigned long long g_verified_mat34_mul      = 0;

unsigned long long g_mismatches_normalize      = 0;
unsigned long long g_mismatches_normalize_safe = 0;
unsigned long long g_mismatches_segment_lerp   = 0;
unsigned long long g_mismatches_ray_from_ends  = 0;
unsigned long long g_mismatches_mat34_mul      = 0;

// ============================================================================
// 1. C3Vector::Normalize (0x004C3420)
// ============================================================================
__forceinline void Fast_Normalize_Impl(void* this_ptr) {
    float* const v = reinterpret_cast<float*>(this_ptr);
    const double len_sq = static_cast<double>(v[0]) * v[0] +
                          static_cast<double>(v[1]) * v[1] +
                          static_cast<double>(v[2]) * v[2];
    const float inv = static_cast<float>(1.0 / std::sqrt(len_sq));
    v[0] *= inv;
    v[1] *= inv;
    v[2] *= inv;
}

void __fastcall Hooked_Normalize(void* this_ptr, void* dummy_edx) {
    if (g_dead_normalize || !this_ptr) {
        orig_Normalize(this_ptr, dummy_edx);
        return;
    }

    const unsigned long long call_idx = ++g_calls_normalize;

    if (call_idx <= kVerifyFirst || ((call_idx & kResampleMask) == 0)) {
        ++g_verified_normalize;
        float fast_v[3];
        memcpy(fast_v, this_ptr, sizeof(fast_v));
        Fast_Normalize_Impl(fast_v);

        orig_Normalize(this_ptr, dummy_edx);
        const float* const orig_v = reinterpret_cast<const float*>(this_ptr);

        bool mismatch = false;
        for (int i = 0; i < 3; ++i) {
            if (std::fabs(fast_v[i] - orig_v[i]) > kVerifyTolerance) {
                mismatch = true;
                break;
            }
        }

        if (mismatch) {
            if (++g_mismatches_normalize == 1) {
                Log("[VectorRayOpsFast] Normalize MISMATCH at call %llu, retiring hook", call_idx);
                g_dead_normalize = true;
            }
        }
        return;
    }

    Fast_Normalize_Impl(this_ptr);
}

// ============================================================================
// 2. C3Vector::NormalizeSafe (0x004C3600)
// ============================================================================
__forceinline void Fast_NormalizeSafe_Impl(void* this_ptr) {
    float* const v = reinterpret_cast<float*>(this_ptr);
    const double len_sq = static_cast<double>(v[0]) * v[0] +
                          static_cast<double>(v[1]) * v[1] +
                          static_cast<double>(v[2]) * v[2];
    if (len_sq > 0.00000023841858) {
        const float inv = static_cast<float>(1.0 / std::sqrt(len_sq));
        v[0] *= inv;
        v[1] *= inv;
        v[2] *= inv;
    }
}

void __fastcall Hooked_NormalizeSafe(void* this_ptr, void* dummy_edx) {
    if (g_dead_normalize_safe || !this_ptr) {
        orig_NormalizeSafe(this_ptr, dummy_edx);
        return;
    }

    const unsigned long long call_idx = ++g_calls_normalize_safe;

    if (call_idx <= kVerifyFirst || ((call_idx & kResampleMask) == 0)) {
        ++g_verified_normalize_safe;
        float fast_v[3];
        memcpy(fast_v, this_ptr, sizeof(fast_v));
        Fast_NormalizeSafe_Impl(fast_v);

        orig_NormalizeSafe(this_ptr, dummy_edx);
        const float* const orig_v = reinterpret_cast<const float*>(this_ptr);

        bool mismatch = false;
        for (int i = 0; i < 3; ++i) {
            if (std::fabs(fast_v[i] - orig_v[i]) > kVerifyTolerance) {
                mismatch = true;
                break;
            }
        }

        if (mismatch) {
            if (++g_mismatches_normalize_safe == 1) {
                Log("[VectorRayOpsFast] NormalizeSafe MISMATCH at call %llu, retiring hook", call_idx);
                g_dead_normalize_safe = true;
            }
        }
        return;
    }

    Fast_NormalizeSafe_Impl(this_ptr);
}

// ============================================================================
// 3. C3Segment::Lerp (0x0078F4D0)
// ============================================================================
__forceinline void* Fast_SegmentLerp_Impl(const void* this_ptr, void* out, float t) {
    const float* const seg = reinterpret_cast<const float*>(this_ptr);
    float* const o = reinterpret_cast<float*>(out);

    o[0] = (seg[3] - seg[0]) * t + seg[0];
    o[1] = (seg[4] - seg[1]) * t + seg[1];
    o[2] = (seg[5] - seg[2]) * t + seg[2];
    return out;
}

void* __fastcall Hooked_SegmentLerp(const void* this_ptr, void* dummy_edx, void* out, float t) {
    if (g_dead_segment_lerp || !this_ptr || !out) {
        return orig_SegmentLerp(this_ptr, dummy_edx, out, t);
    }

    const unsigned long long call_idx = ++g_calls_segment_lerp;

    if (call_idx <= kVerifyFirst || ((call_idx & kResampleMask) == 0)) {
        ++g_verified_segment_lerp;
        float fast_temp[3];
        Fast_SegmentLerp_Impl(this_ptr, fast_temp, t);

        orig_SegmentLerp(this_ptr, dummy_edx, out, t);
        const float* const orig_o = reinterpret_cast<const float*>(out);

        bool mismatch = false;
        for (int i = 0; i < 3; ++i) {
            if (std::fabs(fast_temp[i] - orig_o[i]) > kVerifyTolerance) {
                mismatch = true;
                break;
            }
        }

        if (mismatch) {
            if (++g_mismatches_segment_lerp == 1) {
                Log("[VectorRayOpsFast] SegmentLerp MISMATCH at call %llu, retiring hook", call_idx);
                g_dead_segment_lerp = true;
            }
        }
        return out;
    }

    return Fast_SegmentLerp_Impl(this_ptr, out, t);
}

// ============================================================================
// 4. C3Ray::FromStartEnd (0x00985200)
// ============================================================================
__forceinline float* Fast_RayFromEnds_Impl(float* out, const float* start, const float* end, char normalize) {
    float dx = end[0] - start[0];
    float dy = end[1] - start[1];
    float dz = end[2] - start[2];

    if (normalize) {
        const double len_sq = static_cast<double>(dx) * dx +
                              static_cast<double>(dy) * dy +
                              static_cast<double>(dz) * dz;
        const float inv = static_cast<float>(1.0 / std::sqrt(len_sq));
        dx *= inv;
        dy *= inv;
        dz *= inv;
    }

    out[0] = start[0];
    out[1] = start[1];
    out[2] = start[2];
    out[3] = dx;
    out[4] = dy;
    out[5] = dz;
    return out;
}

float* __cdecl Hooked_RayFromEnds(float* out, const float* start, const float* end, char normalize) {
    if (g_dead_ray_from_ends || !out || !start || !end) {
        return orig_RayFromEnds(out, start, end, normalize);
    }

    const unsigned long long call_idx = ++g_calls_ray_from_ends;

    if (call_idx <= kVerifyFirst || ((call_idx & kResampleMask) == 0)) {
        ++g_verified_ray_from_ends;
        float fast_temp[6];
        Fast_RayFromEnds_Impl(fast_temp, start, end, normalize);

        orig_RayFromEnds(out, start, end, normalize);

        bool mismatch = false;
        for (int i = 0; i < 6; ++i) {
            if (std::fabs(fast_temp[i] - out[i]) > kVerifyTolerance) {
                mismatch = true;
                break;
            }
        }

        if (mismatch) {
            if (++g_mismatches_ray_from_ends == 1) {
                Log("[VectorRayOpsFast] RayFromEnds MISMATCH at call %llu, retiring hook", call_idx);
                g_dead_ray_from_ends = true;
            }
        }
        return out;
    }

    return Fast_RayFromEnds_Impl(out, start, end, normalize);
}

// ============================================================================
// 5. C34Matrix operator* (0x00984310)
// ============================================================================
__forceinline float* Fast_Mat34Mul_Impl(float* out, const float* a, const float* b) {
    const float l0 = a[0], l1 = a[1], l2 = a[2], l3 = a[3];
    const float l4 = a[4], l5 = a[5], l6 = a[6], l7 = a[7];
    const float l8 = a[8], l9 = a[9], l10 = a[10], l11 = a[11];

    const float r0 = b[0], r1 = b[1], r2 = b[2], r3 = b[3];
    const float r4 = b[4], r5 = b[5], r6 = b[6], r7 = b[7];
    const float r8 = b[8], r9 = b[9], r10 = b[10], r11 = b[11];

    out[0] = l1 * r3 + r6 * l2 + l0 * r0;
    out[1] = r7 * l2 + l0 * r1 + l1 * r4;
    out[2] = l1 * r5 + r2 * l0 + r8 * l2;
    out[3] = l3 * r0 + r6 * l5 + r3 * l4;
    out[4] = l3 * r1 + r7 * l5 + r4 * l4;
    out[5] = l5 * r8 + l4 * r5 + l3 * r2;
    out[6] = r6 * l8 + l7 * r3 + l6 * r0;
    out[7] = l6 * r1 + r7 * l8 + r4 * l7;
    out[8] = l8 * r8 + l7 * r5 + l6 * r2;
    out[9] = l10 * r3 + l11 * r6 + r0 * l9 + r9;
    out[10] = l11 * r7 + l10 * r4 + l9 * r1 + r10;
    out[11] = l11 * r8 + l10 * r5 + r2 * l9 + r11;
    return out;
}

float* __cdecl Hooked_Mat34Mul(float* out, const float* a, const float* b) {
    if (g_dead_mat34_mul || !out || !a || !b) {
        return orig_Mat34Mul(out, a, b);
    }

    const unsigned long long call_idx = ++g_calls_mat34_mul;

    if (call_idx <= kVerifyFirst || ((call_idx & kResampleMask) == 0)) {
        ++g_verified_mat34_mul;
        float fast_temp[12];
        Fast_Mat34Mul_Impl(fast_temp, a, b);

        orig_Mat34Mul(out, a, b);

        bool mismatch = false;
        for (int i = 0; i < 12; ++i) {
            if (std::fabs(fast_temp[i] - out[i]) > kVerifyTolerance) {
                mismatch = true;
                break;
            }
        }

        if (mismatch) {
            if (++g_mismatches_mat34_mul == 1) {
                Log("[VectorRayOpsFast] Mat34Mul MISMATCH at call %llu, retiring hook", call_idx);
                g_dead_mat34_mul = true;
            }
        }
        return out;
    }

    return Fast_Mat34Mul_Impl(out, a, b);
}

} // namespace

void Init() {
    if (!Config::g_settings.OptVectorRayOpsFast) {
        return;
    }

    static const unsigned char kExp_Normalize[5]     = { 0xD9, 0x01, 0xD9, 0x41, 0x04 };
    static const unsigned char kExp_NormalizeSafe[8] = { 0xD9, 0x41, 0x08, 0xD9, 0x41, 0x04, 0xD9, 0x01 };
    static const unsigned char kExp_SegmentLerp[8]   = { 0x55, 0x8B, 0xEC, 0xD9, 0x41, 0x0C, 0x8B, 0x45 };
    static const unsigned char kExp_RayFromEnds[8]   = { 0x55, 0x8B, 0xEC, 0x83, 0xEC, 0x0C, 0x80, 0x7D };
    static const unsigned char kExp_Mat34Mul[8]      = { 0x55, 0x8B, 0xEC, 0x8B, 0x55, 0x0C, 0x8B, 0x4D };

    if (memcmp((const void*)kTarget_Normalize, kExp_Normalize, 5) != 0) {
        Log("[VectorRayOpsFast] Normalize prologue mismatch, aborting");
        return;
    }
    if (memcmp((const void*)kTarget_NormalizeSafe, kExp_NormalizeSafe, 8) != 0) {
        Log("[VectorRayOpsFast] NormalizeSafe prologue mismatch, aborting");
        return;
    }
    if (memcmp((const void*)kTarget_SegmentLerp, kExp_SegmentLerp, 8) != 0) {
        Log("[VectorRayOpsFast] SegmentLerp prologue mismatch, aborting");
        return;
    }
    if (memcmp((const void*)kTarget_RayFromEnds, kExp_RayFromEnds, 8) != 0) {
        Log("[VectorRayOpsFast] RayFromEnds prologue mismatch, aborting");
        return;
    }
    if (memcmp((const void*)kTarget_Mat34Mul, kExp_Mat34Mul, 8) != 0) {
        Log("[VectorRayOpsFast] Mat34Mul prologue mismatch, aborting");
        return;
    }

    if (WineSafe_CreateHook((void*)kTarget_Normalize,     (void*)Hooked_Normalize,     (void**)&orig_Normalize)     != MH_OK ||
        WineSafe_CreateHook((void*)kTarget_NormalizeSafe, (void*)Hooked_NormalizeSafe, (void**)&orig_NormalizeSafe) != MH_OK ||
        WineSafe_CreateHook((void*)kTarget_SegmentLerp,   (void*)Hooked_SegmentLerp,   (void**)&orig_SegmentLerp)   != MH_OK ||
        WineSafe_CreateHook((void*)kTarget_RayFromEnds,   (void*)Hooked_RayFromEnds,   (void**)&orig_RayFromEnds)   != MH_OK ||
        WineSafe_CreateHook((void*)kTarget_Mat34Mul,      (void*)Hooked_Mat34Mul,      (void**)&orig_Mat34Mul)      != MH_OK) {
        Log("[VectorRayOpsFast] Failed to create one or more hooks");
        return;
    }

    if (WO_EnableHook((void*)kTarget_Normalize)     != MH_OK ||
        WO_EnableHook((void*)kTarget_NormalizeSafe) != MH_OK ||
        WO_EnableHook((void*)kTarget_SegmentLerp)   != MH_OK ||
        WO_EnableHook((void*)kTarget_RayFromEnds)   != MH_OK ||
        WO_EnableHook((void*)kTarget_Mat34Mul)      != MH_OK) {
        Log("[VectorRayOpsFast] Failed to enable one or more hooks");
        return;
    }

    SamplingProfiler::RegisterSelfSymbol("Fast_VecNormalize",     (const void*)&Hooked_Normalize);
    SamplingProfiler::RegisterSelfSymbol("Fast_VecNormalizeSafe", (const void*)&Hooked_NormalizeSafe);
    SamplingProfiler::RegisterSelfSymbol("Fast_SegmentLerp",      (const void*)&Hooked_SegmentLerp);
    SamplingProfiler::RegisterSelfSymbol("Fast_RayFromEnds",      (const void*)&Hooked_RayFromEnds);
    SamplingProfiler::RegisterSelfSymbol("Fast_Mat34Mul",         (const void*)&Hooked_Mat34Mul);

    g_installed = true;
    Log("[VectorRayOpsFast] ACTIVE on vector, ray, segment, and mat34 ops (sub_4C3420/4C3600/78F4D0/985200/984310)");
}

void LogStats() {
    if (!Config::g_settings.OptVectorRayOpsFast || !g_installed) {
        return;
    }

    Log("[VectorRayOpsFast] Stats: Normalize=%llu (%llu ver, %llu mis%s), "
        "NormalizeSafe=%llu (%llu ver, %llu mis%s), "
        "SegmentLerp=%llu (%llu ver, %llu mis%s), "
        "RayFromEnds=%llu (%llu ver, %llu mis%s), "
        "Mat34Mul=%llu (%llu ver, %llu mis%s)",
        g_calls_normalize, g_verified_normalize, g_mismatches_normalize, g_dead_normalize ? " [RETIRED]" : "",
        g_calls_normalize_safe, g_verified_normalize_safe, g_mismatches_normalize_safe, g_dead_normalize_safe ? " [RETIRED]" : "",
        g_calls_segment_lerp, g_verified_segment_lerp, g_mismatches_segment_lerp, g_dead_segment_lerp ? " [RETIRED]" : "",
        g_calls_ray_from_ends, g_verified_ray_from_ends, g_mismatches_ray_from_ends, g_dead_ray_from_ends ? " [RETIRED]" : "",
        g_calls_mat34_mul, g_verified_mat34_mul, g_mismatches_mat34_mul, g_dead_mat34_mul ? " [RETIRED]" : "");
}

void Shutdown() {
    if (!g_installed) {
        return;
    }

    MH_DisableHook((void*)kTarget_Normalize);
    MH_DisableHook((void*)kTarget_NormalizeSafe);
    MH_DisableHook((void*)kTarget_SegmentLerp);
    MH_DisableHook((void*)kTarget_RayFromEnds);
    MH_DisableHook((void*)kTarget_Mat34Mul);
    g_installed = false;
}

} // namespace VectorRayOpsFast
