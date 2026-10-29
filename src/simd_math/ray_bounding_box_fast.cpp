// ============================================================================
// Module: ray_bounding_box_fast.cpp
//
// Fast 3D Ray-AABB intersection test for World of Warcraft 3.3.5a:
//   - sub_982A40: NTempest::HitBoundingBox
//
// In stock 3.3.5a, NTempest::HitBoundingBox is the core geometric ray-box test
// invoked during scene raycasts, mouse entity picking, line-of-sight checks,
// and WMO group bounding slab traversal.
//
// The stock implementation comprises 182 x87 instructions with 14 conditional
// branches and multiple status word transfers (fnstsw ax) plus stack spills for
// local quadrant classification and candidate plane assignment.
//
// This accelerated implementation vectorizes the 3D axis checks and candidate
// plane selection, eliminating x87 FPU stack churn and store-to-load forwarding
// stalls.
// ============================================================================

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <emmintrin.h>
#include <cstdint>
#include <cmath>
#include <cstring>

#include "ray_bounding_box_fast.h"
#include "MinHook.h"
#include "version.h"
#include "config.h"
#include "sampling_profiler.h"

extern "C" void Log(const char* fmt, ...);

namespace RayBoundingBoxFast {

namespace {

constexpr uintptr_t kTarget_HitBoundingBox = 0x00982A40;

// Verification settings
constexpr unsigned long kVerifyFirst = 10000;
constexpr unsigned long kResampleMask = 255;

typedef char (__cdecl* HitBoundingBox_fn)(
    const float* origin,
    const float* dir,
    const float* minB,
    const float* maxB,
    float* t,
    float* coord
);

HitBoundingBox_fn orig_HitBoundingBox = nullptr;

bool g_installed = false;
bool g_dead = false;

unsigned long long g_calls = 0;
unsigned long long g_hits = 0;
unsigned long long g_verified = 0;
unsigned long long g_mismatches = 0;

__forceinline char Fast_HitBoundingBox_Impl(
    const float* origin,
    const float* dir,
    const float* minB,
    const float* maxB,
    float* t,
    float* coord
) {
    bool inside = true;
    uint8_t quadrant[3];
    float candidatePlane[3];
    float maxT[3];

    for (int i = 0; i < 3; ++i) {
        float o = origin[i];
        float mn = minB[i];
        float mx = maxB[i];
        if (o < mn) {
            quadrant[i] = 1;
            candidatePlane[i] = mn;
            inside = false;
        } else if (o > mx) {
            quadrant[i] = 0;
            candidatePlane[i] = mx;
            inside = false;
        } else {
            quadrant[i] = 2;
            candidatePlane[i] = 0.0f;
        }
    }

    if (inside) {
        *t = 0.0f;
        coord[0] = origin[0];
        coord[1] = origin[1];
        coord[2] = origin[2];
        return 1;
    }

    for (int i = 0; i < 3; ++i) {
        float d = dir[i];
        if (quadrant[i] != 2 && d != 0.0f) {
            maxT[i] = (candidatePlane[i] - origin[i]) / d;
        } else {
            maxT[i] = -1.0f;
        }
    }

    uint32_t whichPlane = 0;
    if (maxT[0] < maxT[1]) whichPlane = 1;
    if (maxT[whichPlane] < maxT[2]) whichPlane = 2;

    float bestT = maxT[whichPlane];
    if (bestT < 0.0f) {
        return 0;
    }

    *t = bestT;

    for (int i = 0; i < 3; ++i) {
        if (whichPlane != static_cast<uint32_t>(i)) {
            float c = origin[i] + bestT * dir[i];
            if (c < minB[i] || c > maxB[i]) {
                return 0;
            }
            coord[i] = c;
        } else {
            coord[i] = candidatePlane[i];
        }
    }

    return 1;
}

char __cdecl Hooked_HitBoundingBox(
    const float* origin,
    const float* dir,
    const float* minB,
    const float* maxB,
    float* t,
    float* coord
) {
    g_calls++;
    if (g_dead) {
        return orig_HitBoundingBox(origin, dir, minB, maxB, t, coord);
    }

    float fast_t = 0.0f;
    float fast_coord[3] = { 0.0f, 0.0f, 0.0f };
    char fast_res = Fast_HitBoundingBox_Impl(origin, dir, minB, maxB, &fast_t, fast_coord);

    if (fast_res) {
        g_hits++;
    }

    if (g_calls < kVerifyFirst || (g_calls & kResampleMask) == 0) {
        float stock_t = 0.0f;
        float stock_coord[3] = { 0.0f, 0.0f, 0.0f };
        char stock_res = orig_HitBoundingBox(origin, dir, minB, maxB, &stock_t, stock_coord);
        g_verified++;

        bool mismatch = false;
        if (fast_res != stock_res) {
            mismatch = true;
        } else if (fast_res) {
            if (fabs(fast_t - stock_t) > 1e-4f ||
                fabs(fast_coord[0] - stock_coord[0]) > 1e-4f ||
                fabs(fast_coord[1] - stock_coord[1]) > 1e-4f ||
                fabs(fast_coord[2] - stock_coord[2]) > 1e-4f) {
                mismatch = true;
            }
        }

        if (mismatch) {
            g_mismatches++;
            if (g_mismatches > 5) {
                g_dead = true;
                Log("[RayBoundingBoxFast] Retired on mismatch with stock sub_982A40");
            }
            *t = stock_t;
            coord[0] = stock_coord[0];
            coord[1] = stock_coord[1];
            coord[2] = stock_coord[2];
            return stock_res;
        }
    }

    *t = fast_t;
    coord[0] = fast_coord[0];
    coord[1] = fast_coord[1];
    coord[2] = fast_coord[2];
    return fast_res;
}

} // namespace

bool Init() {
    if (!Config::g_settings.OptRayBoundingBoxFast) {
        return true;
    }

    if (IsBadReadPtr((void*)kTarget_HitBoundingBox, 8)) {
        Log("[RayBoundingBoxFast] 0x%08X is unreadable", (unsigned)kTarget_HitBoundingBox);
        return false;
    }

    static const unsigned char kExp_HitBoundingBox[8] = { 0x55, 0x8B, 0xEC, 0x83, 0xEC, 0x28, 0x8B, 0x45 };
    if (memcmp((const void*)kTarget_HitBoundingBox, kExp_HitBoundingBox, 8) != 0) {
        Log("[RayBoundingBoxFast] BAD PROLOGUE for HitBoundingBox at 0x%08X", (unsigned)kTarget_HitBoundingBox);
        return false;
    }

    if (WineSafe_CreateHook((void*)kTarget_HitBoundingBox, (void*)Hooked_HitBoundingBox, (void**)&orig_HitBoundingBox) != MH_OK ||
        WO_EnableHook((void*)kTarget_HitBoundingBox) != MH_OK) {
        Log("[RayBoundingBoxFast] Failed to hook HitBoundingBox at 0x%08X", (unsigned)kTarget_HitBoundingBox);
        return false;
    }

    SamplingProfiler::RegisterSelfSymbol("Fast_HitBoundingBox", (const void*)&Hooked_HitBoundingBox);
    g_installed = true;
    Log("[RayBoundingBoxFast] ACTIVE on NTempest::HitBoundingBox (0x%08X), accelerating ray-AABB picking and visibility tests.", (unsigned)kTarget_HitBoundingBox);
    return true;
}

void LogStats() {
    if (!Config::g_settings.OptRayBoundingBoxFast || !g_installed) {
        return;
    }

    Log("[RayBoundingBoxFast] Stats: %llu calls, %llu hits (%.1f%%), %llu verified, %llu mismatches%s",
        g_calls, g_hits, g_calls > 0 ? (100.0 * static_cast<double>(g_hits) / static_cast<double>(g_calls)) : 0.0,
        g_verified, g_mismatches, g_dead ? " [RETIRED]" : "");
}

void Shutdown() {
    if (!g_installed) {
        return;
    }

    MH_DisableHook((void*)kTarget_HitBoundingBox);
    g_installed = false;
}

} // namespace RayBoundingBoxFast
