#include <windows.h>
#include <emmintrin.h>
#include <cstdint>
#include <cstring>

#include "occluder_sphere_sse2.h"
#include "MinHook.h"
#include "version.h"
#include "config.h"

extern "C" void Log(const char* fmt, ...);

MH_STATUS WineSafe_CreateHook(void* target, void* detour, void** original);
MH_STATUS WO_EnableHook(void* target);

namespace OccluderSphere {

namespace {

constexpr uintptr_t kTestSphere   = 0x007CCE00; // int __cdecl sub_7CCE00(const float* sphere)
constexpr uintptr_t kOccluderNum  = 0x00D2DCEC; // uint32_t active occluder count
constexpr uintptr_t kOccluderList = 0x00D2DCF0; // pointer to OccluderVolume array
constexpr uintptr_t kPlaneArray   = 0x00D2DCE0; // pointer to plane float array

// Verification parameters
constexpr unsigned long kVerifyFirst  = 20000;
constexpr unsigned long kResampleMask = 1023;

struct OccluderVolume {
    uint32_t first_plane;
    uint32_t plane_count;
};

typedef int (__cdecl* TestSphere_fn)(const float* sphere);
TestSphere_fn orig_TestSphere = nullptr;

bool g_installed = false;
bool g_dead      = false;

unsigned long g_calls    = 0;
unsigned long g_occluded = 0;
unsigned long g_visible  = 0;
unsigned long g_verified = 0;
unsigned long g_mismatch = 0;

__forceinline int TestSphere_Fast(const float* sphere) {
    const uint32_t num_occluders = *(const uint32_t*)kOccluderNum;
    if (num_occluders == 0) {
        return 0;
    }

    const OccluderVolume* occluders = *(const OccluderVolume* const*)kOccluderList;
    const float* all_planes = *(const float* const*)kPlaneArray;
    if (!occluders || !all_planes) {
        return 0;
    }

    const __m128 sx = _mm_set1_ps(sphere[0]);
    const __m128 sy = _mm_set1_ps(sphere[1]);
    const __m128 sz = _mm_set1_ps(sphere[2]);
    const __m128 sr = _mm_set1_ps(sphere[3]);
    const __m128 zero = _mm_setzero_ps();
    for (uint32_t i = 0; i < num_occluders; ++i) {
        const uint32_t count = occluders[i].plane_count;
        const float* p = all_planes + (occluders[i].first_plane * 4);
        uint32_t remaining = count;
        bool outside = false;

        // Process 4 planes at a time using transposed SSE2 dot products
        while (remaining >= 4) {
            const __m128 p0 = _mm_loadu_ps(p);
            const __m128 p1 = _mm_loadu_ps(p + 4);
            const __m128 p2 = _mm_loadu_ps(p + 8);
            const __m128 p3 = _mm_loadu_ps(p + 12);

            const __m128 t0 = _mm_unpacklo_ps(p0, p1);
            const __m128 t1 = _mm_unpackhi_ps(p0, p1);
            const __m128 t2 = _mm_unpacklo_ps(p2, p3);
            const __m128 t3 = _mm_unpackhi_ps(p2, p3);

            const __m128 nx = _mm_movelh_ps(t0, t2);
            const __m128 ny = _mm_movehl_ps(t2, t0);
            const __m128 nz = _mm_movelh_ps(t1, t3);
            const __m128 nd = _mm_movehl_ps(t3, t1);

            const __m128 dot4 = _mm_add_ps(_mm_add_ps(_mm_mul_ps(nx, sx), _mm_mul_ps(ny, sy)),
                                           _mm_add_ps(_mm_mul_ps(nz, sz), _mm_add_ps(nd, sr)));

            // If dist > 0.0f for any plane, sphere is outside this occluder
            const __m128 cmp = _mm_cmpgt_ps(dot4, zero);
            if (_mm_movemask_ps(cmp) != 0) {
                outside = true;
                break;
            }

            p += 16;
            remaining -= 4;
        }

        if (outside) {
            continue;
        }

        // Tail planes (1..3)
        while (remaining > 0) {
            const float dot = p[0] * sphere[0] + p[1] * sphere[1] + p[2] * sphere[2] + p[3] + sphere[3];
            if (dot > 0.0f) {
                outside = true;
                break;
            }
            p += 4;
            --remaining;
        }

        if (!outside) {
            // Sphere is on inside (dist <= 0.0f) for ALL planes of this occluder volume: occluded!
            return 1;
        }
    }

    return 0;
}

int __cdecl Hooked_TestSphere(const float* sphere) {
    if (g_dead || !sphere) {
        return orig_TestSphere(sphere);
    }

    ++g_calls;
    int mine = 0;

    __try {
        mine = TestSphere_Fast(sphere);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        g_dead = true;
        Log("[OccluderSphere] Exception during fast test, retiring hook\n");
        return orig_TestSphere(sphere);
    }

    if (mine) {
        ++g_occluded;
    } else {
        ++g_visible;
    }

    const bool should_verify = (g_calls <= kVerifyFirst) || ((g_calls & kResampleMask) == 0);
    if (should_verify) {
        const int orig = orig_TestSphere(sphere);
        if (mine != orig) {
            ++g_mismatch;
            Log("[OccluderSphere] Mismatch! sphere=(%.2f, %.2f, %.2f, %.2f) mine=%d orig=%d\n",
                sphere[0], sphere[1], sphere[2], sphere[3], mine, orig);
            if (g_mismatch > 10) {
                g_dead = true;
                Log("[OccluderSphere] Too many mismatches, retiring hook\n");
            }
            return orig;
        }
        ++g_verified;
    }

    return mine;
}

} // namespace

bool Init() {
    if (!Config::g_settings.OptOccluderSphere) {
        return true;
    }

    if (IsBadReadPtr((void*)kTestSphere, 8)) {
        Log("[OccluderSphere] 0x%08X unreadable - not installing\n", (unsigned)kTestSphere);
        return false;
    }

    if (WineSafe_CreateHook((void*)kTestSphere, (void*)Hooked_TestSphere,
                            (void**)&orig_TestSphere) != MH_OK) {
        Log("[OccluderSphere] hook NOT created\n");
        return false;
    }

    if (WO_EnableHook((void*)kTestSphere) != MH_OK) {
        Log("[OccluderSphere] hook created but could not be enabled\n");
        return false;
    }

    g_installed = true;
    Log("[OccluderSphere] ACTIVE on sub_7CCE00 (0x%08X) - convex occluder volume sphere culling. "
        "Replaced scalar x87 plane distances with 4-wide transposed SSE2 vector evaluations. "
        "Verifying first %lu calls, then 1 in %d.\n",
        (unsigned)kTestSphere, kVerifyFirst, (int)(kResampleMask + 1));
    return true;
}

void LogStats() {
    if (!Config::g_settings.OptOccluderSphere) {
        return;
    }
    if (!g_installed) {
        Log("[OccluderSphere] not installed - nothing measured\n");
        return;
    }
    if (g_calls == 0) {
        Log("[OccluderSphere] installed but never called\n");
        return;
    }
    const double occ_pct = (g_calls > 0) ? (100.0 * (double)g_occluded / (double)g_calls) : 0.0;
    Log("[OccluderSphere] %lu calls, %lu occluded (%.1f%%), %lu visible, %lu verified, %lu mismatches%s\n",
        g_calls, g_occluded, occ_pct, g_visible, g_verified, g_mismatch,
        g_dead ? " - DISABLED" : (g_calls < kVerifyFirst ? " (still verifying)" : ""));
}

} // namespace OccluderSphere
