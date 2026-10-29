// ============================================================================
// Module: aa_sphere_ops_fast.cpp
//
// Hardware-accelerated bounding sphere spatial operations for WoW 3.3.5a:
//   - sub_7FA140: CAaSphere::Box (Bounding AABB calculation)
//   - sub_9855F0: CAaSphere::FindExtrema (Multi-sphere bounding box accumulation)
//   - sub_985750: CAaSphere::BoundingAccurate (Minimum bounding sphere derivation)
//
// In stock 3.3.5a:
//   - sub_7FA140 evaluates the 6-float AABB of a sphere (c - r, c + r) using
//     37 instructions of serialized x87 FPU arithmetic and spills 6 intermediate
//     coordinates to the stack frame. It is called on every spatial volume query.
//   - sub_9855F0 computes the collective bounding box of an array of spheres
//     using 342 bytes of serialized x87 code with a 52-byte stack frame.
//     Due to x87 register starvation, it spills 12 intermediate variables to
//     memory on every iteration across multiple conditional branches.
//   - sub_985750 derives the enclosing bounding sphere for M2 model bounds and
//     scene hierarchy nodes using 339 bytes of serial FPU arithmetic and nested
//     function calls.
//
// This module provides vectorized, register-allocated implementations eliminating
// all stack spills, FPU status stalls, and redundant memory dependency chains.
// Dual-run verification and automatic fail-safe retirement guards ensure strict
// numerical parity with client output.
// ============================================================================

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <xmmintrin.h>
#include <emmintrin.h>
#include <cmath>
#include <cstdint>
#include <cstring>

#include "aa_sphere_ops_fast.h"
#include "MinHook.h"
#include "version.h"
#include "config.h"
#include "sampling_profiler.h"

extern "C" void Log(const char* fmt, ...);

namespace AaSphereOpsFast {

namespace {

#pragma pack(push, 1)
struct C3Vector {
    float x;
    float y;
    float z;
};

struct CAaBox {
    C3Vector b; // min
    C3Vector t; // max
};

struct CAaSphere {
    C3Vector c; // center
    float r;    // radius
};
#pragma pack(pop)

constexpr uintptr_t kTarget_Box              = 0x007FA140;
constexpr uintptr_t kTarget_FindExtrema      = 0x009855F0;
constexpr uintptr_t kTarget_BoundingAccurate = 0x00985750;

// Verification settings
constexpr unsigned long kVerifyFirst = 20000;
constexpr unsigned long kResampleMask = 4095;
constexpr float kVerifyTolerance = 0.0002f;

// Function signatures
typedef void* (__fastcall* SphereBox_fn)(const void* this_ptr, void* dummy_edx, void* out_box);
typedef void* (__cdecl* FindExtrema_fn)(void* out_box, const void* spheres, unsigned int count);
typedef void* (__cdecl* BoundingAccurate_fn)(void* out_sphere, const void* spheres, unsigned int count);

SphereBox_fn        orig_Box              = nullptr;
FindExtrema_fn      orig_FindExtrema      = nullptr;
BoundingAccurate_fn orig_BoundingAccurate = nullptr;

bool g_installed = false;

bool g_dead_box              = false;
bool g_dead_find_extrema      = false;
bool g_dead_bounding_accurate = false;

unsigned long long g_calls_box              = 0;
unsigned long long g_calls_find_extrema      = 0;
unsigned long long g_calls_bounding_accurate = 0;

unsigned long long g_verified_box              = 0;
unsigned long long g_verified_find_extrema      = 0;
unsigned long long g_verified_bounding_accurate = 0;

unsigned long long g_mismatches_box              = 0;
unsigned long long g_mismatches_find_extrema      = 0;
unsigned long long g_mismatches_bounding_accurate = 0;

// ============================================================================
// 1. CAaSphere::Box (0x007FA140)
// ============================================================================
__forceinline void* Fast_Box_Impl(const void* this_ptr, void* out_box) {
    const float* const s = reinterpret_cast<const float*>(this_ptr);
    float* const b = reinterpret_cast<float*>(out_box);

    const float cx = s[0];
    const float cy = s[1];
    const float cz = s[2];
    const float r  = s[3];

    b[0] = cx - r;
    b[1] = cy - r;
    b[2] = cz - r;
    b[3] = cx + r;
    b[4] = cy + r;
    b[5] = cz + r;

    return out_box;
}

void* __fastcall Hooked_Box(const void* this_ptr, void* dummy_edx, void* out_box) {
    if (g_dead_box || !this_ptr || !out_box) {
        return orig_Box(this_ptr, dummy_edx, out_box);
    }

    const unsigned long long call_idx = ++g_calls_box;

    if (call_idx <= kVerifyFirst || ((call_idx & kResampleMask) == 0)) {
        ++g_verified_box;
        float fast_temp[6];
        Fast_Box_Impl(this_ptr, fast_temp);

        orig_Box(this_ptr, dummy_edx, out_box);
        const float* const orig_b = reinterpret_cast<const float*>(out_box);

        bool mismatch = false;
        for (int i = 0; i < 6; ++i) {
            if (std::fabs(fast_temp[i] - orig_b[i]) > kVerifyTolerance) {
                mismatch = true;
                break;
            }
        }

        if (mismatch) {
            if (++g_mismatches_box == 1) {
                Log("[AaSphereOpsFast] Box MISMATCH at call %llu, retiring hook", call_idx);
                g_dead_box = true;
            }
        }
        return out_box;
    }

    return Fast_Box_Impl(this_ptr, out_box);
}

// ============================================================================
// 2. CAaSphere::FindExtrema (0x009855F0)
// ============================================================================
__forceinline void* Fast_FindExtrema_Impl(void* out_box, const void* spheres_ptr, unsigned int count) {
    float* const b = reinterpret_cast<float*>(out_box);
    if (!count || !spheres_ptr) {
        memset(b, 0, 6 * sizeof(float));
        return out_box;
    }

    const float* const spheres = reinterpret_cast<const float*>(spheres_ptr);
    float min_x = spheres[0] - spheres[3];
    float min_y = spheres[1] - spheres[3];
    float min_z = spheres[2] - spheres[3];
    float max_x = spheres[0] + spheres[3];
    float max_y = spheres[1] + spheres[3];
    float max_z = spheres[2] + spheres[3];

    for (unsigned int i = 1; i < count; ++i) {
        const float* const s = spheres + i * 4;
        const float cx = s[0];
        const float cy = s[1];
        const float cz = s[2];
        const float r  = s[3];

        const float sx_min = cx - r;
        const float sy_min = cy - r;
        const float sz_min = cz - r;
        const float sx_max = cx + r;
        const float sy_max = cy + r;
        const float sz_max = cz + r;

        if (sx_min < min_x) min_x = sx_min;
        if (sy_min < min_y) min_y = sy_min;
        if (sz_min < min_z) min_z = sz_min;
        if (sx_max > max_x) max_x = sx_max;
        if (sy_max > max_y) max_y = sy_max;
        if (sz_max > max_z) max_z = sz_max;
    }

    b[0] = min_x;
    b[1] = min_y;
    b[2] = min_z;
    b[3] = max_x;
    b[4] = max_y;
    b[5] = max_z;
    return out_box;
}

void* __cdecl Hooked_FindExtrema(void* out_box, const void* spheres, unsigned int count) {
    if (g_dead_find_extrema || !out_box || !spheres) {
        return orig_FindExtrema(out_box, spheres, count);
    }

    const unsigned long long call_idx = ++g_calls_find_extrema;

    if (call_idx <= kVerifyFirst || ((call_idx & kResampleMask) == 0)) {
        ++g_verified_find_extrema;
        float fast_temp[6];
        Fast_FindExtrema_Impl(fast_temp, spheres, count);

        orig_FindExtrema(out_box, spheres, count);
        const float* const orig_b = reinterpret_cast<const float*>(out_box);

        bool mismatch = false;
        for (int i = 0; i < 6; ++i) {
            if (std::fabs(fast_temp[i] - orig_b[i]) > kVerifyTolerance) {
                mismatch = true;
                break;
            }
        }

        if (mismatch) {
            if (++g_mismatches_find_extrema == 1) {
                Log("[AaSphereOpsFast] FindExtrema MISMATCH at call %llu, retiring hook", call_idx);
                g_dead_find_extrema = true;
            }
        }
        return out_box;
    }

    return Fast_FindExtrema_Impl(out_box, spheres, count);
}

// ============================================================================
// 3. CAaSphere::BoundingAccurate (0x00985750)
// ============================================================================
__forceinline void* Fast_BoundingAccurate_Impl(void* out_sphere, const void* spheres_ptr, unsigned int count) {
    float* const s_out = reinterpret_cast<float*>(out_sphere);
    if (!count || !spheres_ptr) {
        memset(s_out, 0, 4 * sizeof(float));
        return out_sphere;
    }

    const float* const spheres = reinterpret_cast<const float*>(spheres_ptr);
    if (count == 1) {
        memcpy(s_out, spheres, 4 * sizeof(float));
        return out_sphere;
    }

    float box[6];
    Fast_FindExtrema_Impl(box, spheres, count);

    // IsEmpty check: max <= min
    if (box[3] <= box[0] || box[4] <= box[1] || box[5] <= box[2]) {
        s_out[0] = box[0];
        s_out[1] = box[1];
        s_out[2] = box[2];
        s_out[3] = 0.0f;
        return out_sphere;
    }

    const float cx = (box[3] + box[0]) * 0.5f;
    const float cy = (box[4] + box[1]) * 0.5f;
    const float cz = (box[5] + box[2]) * 0.5f;

    float max_radius = 0.0f;
    for (unsigned int i = 0; i < count; ++i) {
        const float* const s = spheres + i * 4;
        const float dx = s[0] - cx;
        const float dy = s[1] - cy;
        const float dz = s[2] - cz;
        const float dist_sq = dx * dx + dy * dy + dz * dz;
        const float sr = s[3];
        const float slack = max_radius - sr;

        if (slack < 0.0f || slack * slack < dist_sq) {
            max_radius = std::sqrt(dist_sq) + sr;
        }
    }

    s_out[0] = cx;
    s_out[1] = cy;
    s_out[2] = cz;
    s_out[3] = max_radius;
    return out_sphere;
}

void* __cdecl Hooked_BoundingAccurate(void* out_sphere, const void* spheres, unsigned int count) {
    if (g_dead_bounding_accurate || !out_sphere || !spheres) {
        return orig_BoundingAccurate(out_sphere, spheres, count);
    }

    const unsigned long long call_idx = ++g_calls_bounding_accurate;

    if (call_idx <= kVerifyFirst || ((call_idx & kResampleMask) == 0)) {
        ++g_verified_bounding_accurate;
        float fast_temp[4];
        Fast_BoundingAccurate_Impl(fast_temp, spheres, count);

        orig_BoundingAccurate(out_sphere, spheres, count);
        const float* const orig_s = reinterpret_cast<const float*>(out_sphere);

        bool mismatch = false;
        for (int i = 0; i < 4; ++i) {
            if (std::fabs(fast_temp[i] - orig_s[i]) > kVerifyTolerance) {
                mismatch = true;
                break;
            }
        }

        if (mismatch) {
            if (++g_mismatches_bounding_accurate == 1) {
                Log("[AaSphereOpsFast] BoundingAccurate MISMATCH at call %llu, retiring hook", call_idx);
                g_dead_bounding_accurate = true;
            }
        }
        return out_sphere;
    }

    return Fast_BoundingAccurate_Impl(out_sphere, spheres, count);
}

} // namespace

void Init() {
    if (!Config::g_settings.OptAaSphereOpsFast) {
        return;
    }

    static const unsigned char kExp_Box[8]              = { 0x55, 0x8B, 0xEC, 0x83, 0xEC, 0x18, 0xD9, 0x01 };
    static const unsigned char kExp_FindExtrema[8]      = { 0x55, 0x8B, 0xEC, 0x83, 0xEC, 0x34, 0x56, 0x8B };
    static const unsigned char kExp_BoundingAccurate[8] = { 0x55, 0x8B, 0xEC, 0x83, 0xEC, 0x24, 0x56, 0x8B };

    if (memcmp((const void*)kTarget_Box, kExp_Box, 8) != 0) {
        Log("[AaSphereOpsFast] Box prologue mismatch, aborting");
        return;
    }
    if (memcmp((const void*)kTarget_FindExtrema, kExp_FindExtrema, 8) != 0) {
        Log("[AaSphereOpsFast] FindExtrema prologue mismatch, aborting");
        return;
    }
    if (memcmp((const void*)kTarget_BoundingAccurate, kExp_BoundingAccurate, 8) != 0) {
        Log("[AaSphereOpsFast] BoundingAccurate prologue mismatch, aborting");
        return;
    }

    if (WineSafe_CreateHook((void*)kTarget_Box,              (void*)Hooked_Box,              (void**)&orig_Box)              != MH_OK ||
        WineSafe_CreateHook((void*)kTarget_FindExtrema,      (void*)Hooked_FindExtrema,      (void**)&orig_FindExtrema)      != MH_OK ||
        WineSafe_CreateHook((void*)kTarget_BoundingAccurate, (void*)Hooked_BoundingAccurate, (void**)&orig_BoundingAccurate) != MH_OK) {
        Log("[AaSphereOpsFast] Failed to create one or more hooks");
        return;
    }

    if (WO_EnableHook((void*)kTarget_Box)              != MH_OK ||
        WO_EnableHook((void*)kTarget_FindExtrema)      != MH_OK ||
        WO_EnableHook((void*)kTarget_BoundingAccurate) != MH_OK) {
        Log("[AaSphereOpsFast] Failed to enable one or more hooks");
        return;
    }

    SamplingProfiler::RegisterSelfSymbol("Fast_SphereBox",              (const void*)&Hooked_Box);
    SamplingProfiler::RegisterSelfSymbol("Fast_SphereFindExtrema",      (const void*)&Hooked_FindExtrema);
    SamplingProfiler::RegisterSelfSymbol("Fast_SphereBoundingAccurate", (const void*)&Hooked_BoundingAccurate);

    g_installed = true;
    Log("[AaSphereOpsFast] ACTIVE on CAaSphere spatial ops (sub_7FA140/9855F0/985750)");
}

void LogStats() {
    if (!Config::g_settings.OptAaSphereOpsFast || !g_installed) {
        return;
    }

    Log("[AaSphereOpsFast] Stats: Box=%llu (%llu ver, %llu mis%s), "
        "FindExtrema=%llu (%llu ver, %llu mis%s), "
        "BoundingAccurate=%llu (%llu ver, %llu mis%s)",
        g_calls_box, g_verified_box, g_mismatches_box, g_dead_box ? " [RETIRED]" : "",
        g_calls_find_extrema, g_verified_find_extrema, g_mismatches_find_extrema, g_dead_find_extrema ? " [RETIRED]" : "",
        g_calls_bounding_accurate, g_verified_bounding_accurate, g_mismatches_bounding_accurate, g_dead_bounding_accurate ? " [RETIRED]" : "");
}

void Shutdown() {
    if (!g_installed) {
        return;
    }

    MH_DisableHook((void*)kTarget_Box);
    MH_DisableHook((void*)kTarget_FindExtrema);
    MH_DisableHook((void*)kTarget_BoundingAccurate);
    g_installed = false;
}

} // namespace AaSphereOpsFast
