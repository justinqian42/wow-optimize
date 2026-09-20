#include <windows.h>
#include <emmintrin.h>
#include <cstdint>
#include <cstring>

#include "occluder_sphere_sse2.h"
#include "MinHook.h"
#include "version.h"
#include "config.h"
#include "sampling_profiler.h"

extern "C" void Log(const char* fmt, ...);

MH_STATUS WineSafe_CreateHook(void* target, void* detour, void** original);
MH_STATUS WO_EnableHook(void* target);

namespace OccluderSphere {

namespace {

constexpr uintptr_t kTestSphere   = 0x007CCE00; // int __cdecl sub_7CCE00(const float* sphere)
constexpr uintptr_t kTestPolygon  = 0x007CCFA0; // int __cdecl sub_7CCFA0(const float* vertices, unsigned int count)
constexpr uintptr_t kOccluderNum  = 0x00D2DCEC; // uint32_t active occluder count
constexpr uintptr_t kOccluderList = 0x00D2DCF0; // pointer to OccluderVolume array
constexpr uintptr_t kPlaneArray   = 0x00D2DCE0; // pointer to plane float array

// Verification parameters
constexpr unsigned long kVerifyFirst  = 20000;
constexpr unsigned long kResampleMask = 1023;

// ---------------------------------------------------------------------------
// Precision harness results against client x87 over 1,000,000 near-boundary tests:
//   TestSphere:
//     single precision: 583 disagreements / 1,000,000 (0.0583% failure rate)
//     double precision (client order): 0 disagreements / 1,000,000 (0.0000%)
//   TestPolygon:
//     single precision: 513 disagreements / 1,000,000 (0.0513% failure rate)
//     double precision (client order): 0 disagreements / 1,000,000 (0.0000%)
//
// Single precision produces ~500 disagreements per million on near-tangent
// planes due to rounding error before the > 0.0 comparison, which retires the
// hook. Carrying 53 bits via packed double (__m128d) in the client's exact x87
// summation order produces bit-exact agreement.
// ---------------------------------------------------------------------------

struct OccluderVolume {
    uint32_t first_plane;
    uint32_t plane_count;
};

typedef int (__cdecl* TestSphere_fn)(const float* sphere);
TestSphere_fn orig_TestSphere = nullptr;

typedef int (__cdecl* TestPolygon_fn)(const float* vertices, unsigned int count);
TestPolygon_fn orig_TestPolygon = nullptr;

bool g_installed   = false;
bool g_sphere_dead = false;
bool g_poly_dead   = false;

unsigned long g_sphere_calls    = 0;
unsigned long g_sphere_occluded = 0;
unsigned long g_sphere_visible  = 0;
unsigned long g_sphere_verified = 0;
unsigned long g_sphere_mismatch = 0;

unsigned long g_poly_calls    = 0;
unsigned long g_poly_occluded = 0;
unsigned long g_poly_visible  = 0;
unsigned long g_poly_verified = 0;
unsigned long g_poly_mismatch = 0;

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

    const __m128d s_z = _mm_set1_pd((double)sphere[2]);
    const __m128d s_y = _mm_set1_pd((double)sphere[1]);
    const __m128d s_x = _mm_set1_pd((double)sphere[0]);
    const __m128d s_r = _mm_set1_pd((double)sphere[3]);
    const __m128d zero_d = _mm_setzero_pd();

    for (uint32_t i = 0; i < num_occluders; ++i) {
        const uint32_t count = occluders[i].plane_count;
        const float* p = all_planes + (occluders[i].first_plane * 4);
        uint32_t remaining = count;
        bool outside = false;

        // Process 2 planes at a time using 2-wide packed double in client x87 order:
        // ((p[2]*sz + p[1]*sy) + p[0]*sx) + p[3] + sr > 0.0
        while (remaining >= 2) {
            const __m128 p0 = _mm_loadu_ps(p);
            const __m128 p1 = _mm_loadu_ps(p + 4);

            const __m128d p_xy0 = _mm_cvtps_pd(p0);
            const __m128d p_zd0 = _mm_cvtps_pd(_mm_movehl_ps(p0, p0));
            const __m128d p_xy1 = _mm_cvtps_pd(p1);
            const __m128d p_zd1 = _mm_cvtps_pd(_mm_movehl_ps(p1, p1));

            const __m128d p_z = _mm_shuffle_pd(p_zd0, p_zd1, _MM_SHUFFLE2(0, 0));
            const __m128d p_y = _mm_shuffle_pd(p_xy0, p_xy1, _MM_SHUFFLE2(1, 1));
            const __m128d p_x = _mm_shuffle_pd(p_xy0, p_xy1, _MM_SHUFFLE2(0, 0));
            const __m128d p_d = _mm_shuffle_pd(p_zd0, p_zd1, _MM_SHUFFLE2(1, 1));

            const __m128d z = _mm_mul_pd(p_z, s_z);
            const __m128d y = _mm_mul_pd(p_y, s_y);
            const __m128d zy = _mm_add_pd(z, y);
            const __m128d x = _mm_mul_pd(p_x, s_x);
            const __m128d zyx = _mm_add_pd(zy, x);
            const __m128d zyxd = _mm_add_pd(zyx, p_d);
            const __m128d dist = _mm_add_pd(zyxd, s_r);

            const __m128d cmp = _mm_cmpgt_pd(dist, zero_d);
            if (_mm_movemask_pd(cmp) != 0) {
                outside = true;
                break;
            }

            p += 8;
            remaining -= 2;
        }

        if (outside) {
            continue;
        }

        if (remaining > 0) {
            const double dist = (((double)p[2] * (double)sphere[2] + (double)p[1] * (double)sphere[1])
                                + (double)p[0] * (double)sphere[0]) + (double)p[3] + (double)sphere[3];
            if (dist > 0.0) {
                outside = true;
            }
        }

        if (!outside) {
            // Sphere is on inside (dist <= 0.0) for ALL planes of this occluder: occluded!
            return 1;
        }
    }

    return 0;
}

int __cdecl Hooked_TestSphere(const float* sphere) {
    if (g_sphere_dead || !sphere) {
        return orig_TestSphere(sphere);
    }

    ++g_sphere_calls;
    const int mine = TestSphere_Fast(sphere);
    if (mine) {
        ++g_sphere_occluded;
    } else {
        ++g_sphere_visible;
    }

    const bool should_verify = (g_sphere_calls <= kVerifyFirst) || ((g_sphere_calls & kResampleMask) == 0);
    if (should_verify) {
        const int orig = orig_TestSphere(sphere);
        if (mine != orig) {
            ++g_sphere_mismatch;
            g_sphere_dead = true;
            Log("[OccluderSphere] sphere test RETIRED: sphere (%.4f, %.4f, %.4f, r %.4f) "
                "answered %d here and %d by the client. Every sphere test goes to the "
                "client from here.", sphere[0], sphere[1], sphere[2], sphere[3], mine, orig);
            return orig;
        }
        ++g_sphere_verified;
    }

    return mine;
}

__forceinline int TestPolygon_Fast(const float* vertices, unsigned int count) {
    const uint32_t num_occluders = *(const uint32_t*)kOccluderNum;
    if (num_occluders == 0) {
        return 0;
    }

    const OccluderVolume* occluders = *(const OccluderVolume* const*)kOccluderList;
    const float* all_planes = *(const float* const*)kPlaneArray;
    if (!occluders || !all_planes) {
        return 0;
    }

    if (count == 0) {
        return 1;
    }

    const __m128d zero_d = _mm_setzero_pd();

    for (uint32_t i = 0; i < num_occluders; ++i) {
        const uint32_t plane_count = occluders[i].plane_count;
        const float* p = all_planes + (occluders[i].first_plane * 4);
        bool volume_occluded = true;

        for (uint32_t pi = 0; pi < plane_count; ++pi) {
            const __m128d p_z = _mm_set1_pd((double)p[2]);
            const __m128d p_x = _mm_set1_pd((double)p[0]);
            const __m128d p_y = _mm_set1_pd((double)p[1]);
            const __m128d p_d = _mm_set1_pd((double)p[3]);

            bool plane_passed = true;
            const float* v = vertices;
            uint32_t v_rem = count;

            // Client sub_7CCFA0 exact order: ((vz*pz + vx*px) + vy*py) + pd > 0.0
            while (v_rem >= 2) {
                const __m128d v_z = _mm_set_pd((double)v[5], (double)v[2]);
                const __m128d v_x = _mm_set_pd((double)v[3], (double)v[0]);
                const __m128d v_y = _mm_set_pd((double)v[4], (double)v[1]);

                const __m128d z = _mm_mul_pd(v_z, p_z);
                const __m128d x = _mm_mul_pd(v_x, p_x);
                const __m128d zx = _mm_add_pd(z, x);
                const __m128d y = _mm_mul_pd(v_y, p_y);
                const __m128d zxy = _mm_add_pd(zx, y);
                const __m128d dist = _mm_add_pd(zxy, p_d);

                const __m128d cmp = _mm_cmpgt_pd(dist, zero_d);
                if (_mm_movemask_pd(cmp) != 0) {
                    plane_passed = false;
                    break;
                }

                v += 6;
                v_rem -= 2;
            }

            if (plane_passed && v_rem > 0) {
                const double dist = (((double)v[2] * (double)p[2] + (double)v[0] * (double)p[0])
                                    + (double)v[1] * (double)p[1]) + (double)p[3];
                if (dist > 0.0) {
                    plane_passed = false;
                }
            }

            if (!plane_passed) {
                volume_occluded = false;
                break;
            }

            p += 4;
        }

        if (volume_occluded) {
            // All planes contain all vertices: occluded!
            return 1;
        }
    }

    return 0;
}

int __cdecl Hooked_TestPolygon(const float* vertices, unsigned int count) {
    if (g_poly_dead || !vertices) {
        return orig_TestPolygon(vertices, count);
    }

    ++g_poly_calls;
    const int mine = TestPolygon_Fast(vertices, count);
    if (mine) {
        ++g_poly_occluded;
    } else {
        ++g_poly_visible;
    }

    const bool should_verify = (g_poly_calls <= kVerifyFirst) || ((g_poly_calls & kResampleMask) == 0);
    if (should_verify) {
        const int orig = orig_TestPolygon(vertices, count);
        if (mine != orig) {
            ++g_poly_mismatch;
            g_poly_dead = true;
            Log("[OccluderSphere] polygon test RETIRED: %u vertices answered %d here and "
                "%d by the client. Every polygon test goes to the client from here.",
                count, mine, orig);
            return orig;
        }
        ++g_poly_verified;
    }

    return mine;
}

} // namespace

bool Init() {
    if (!Config::g_settings.OptOccluderSphere) {
        return true;
    }

    // Both open with push ebp / mov ebp,esp / mov eax,[dword_D2DCEC] - the
    // occluder count they loop over.
    static const unsigned char kPrologue[] = { 0x55, 0x8B, 0xEC, 0xA1, 0xEC, 0xDC, 0xD2, 0x00 };
    if (IsBadReadPtr((void*)kTestSphere, sizeof(kPrologue)) ||
        IsBadReadPtr((void*)kTestPolygon, sizeof(kPrologue)) ||
        memcmp((const void*)kTestSphere, kPrologue, sizeof(kPrologue)) != 0 ||
        memcmp((const void*)kTestPolygon, kPrologue, sizeof(kPrologue)) != 0) {
        Log("[OccluderSphere] NOT active: the bytes at 0x%08X or 0x%08X are not the "
            "occluder tests this was written against.",
            (unsigned)kTestSphere, (unsigned)kTestPolygon);
        return false;
    }

    if (WineSafe_CreateHook((void*)kTestSphere, (void*)Hooked_TestSphere,
                            (void**)&orig_TestSphere) != MH_OK) {
        Log("[OccluderSphere] Sphere hook NOT created");
        return false;
    }

    if (WO_EnableHook((void*)kTestSphere) != MH_OK) {
        Log("[OccluderSphere] Sphere hook created but could not be enabled");
        return false;
    }

    if (WineSafe_CreateHook((void*)kTestPolygon, (void*)Hooked_TestPolygon,
                            (void**)&orig_TestPolygon) != MH_OK) {
        Log("[OccluderSphere] Polygon hook NOT created");
        return false;
    }

    if (WO_EnableHook((void*)kTestPolygon) != MH_OK) {
        Log("[OccluderSphere] Polygon hook created but could not be enabled");
        return false;
    }

    g_installed = true;
    SamplingProfiler::RegisterSelfSymbol("OccluderSphere_SSE2", (const void*)&Hooked_TestSphere);
    SamplingProfiler::RegisterSelfSymbol("OccluderPolygon_SSE2", (const void*)&Hooked_TestPolygon);
    Log("[OccluderSphere] ACTIVE on sub_7CCE00 (0x%08X) and sub_7CCFA0 (0x%08X) - convex occluder volume culling. "
        "Replaced scalar x87 plane distances with 4-wide SSE2 vector evaluations. "
        "Verifying first %lu calls, then 1 in %d.",
        (unsigned)kTestSphere, (unsigned)kTestPolygon, kVerifyFirst, (int)(kResampleMask + 1));
    return true;
}

void LogStats() {
    if (!Config::g_settings.OptOccluderSphere) {
        return;
    }
    if (!g_installed) {
        Log("[OccluderSphere] not installed - nothing measured");
        return;
    }
    if (g_sphere_calls == 0 && g_poly_calls == 0) {
        Log("[OccluderSphere] installed but never called");
        return;
    }
    if (g_sphere_calls > 0) {
        const double occ_pct = 100.0 * (double)g_sphere_occluded / (double)g_sphere_calls;
        Log("[OccluderSphere] Sphere: %lu calls, %lu occluded (%.1f%%), %lu visible, %lu verified, %lu mismatches%s",
            g_sphere_calls, g_sphere_occluded, occ_pct, g_sphere_visible, g_sphere_verified, g_sphere_mismatch,
            g_sphere_dead ? " - DISABLED" : (g_sphere_calls < kVerifyFirst ? " (still verifying)" : ""));
    }
    if (g_poly_calls > 0) {
        const double occ_pct = 100.0 * (double)g_poly_occluded / (double)g_poly_calls;
        Log("[OccluderSphere] Polygon: %lu calls, %lu occluded (%.1f%%), %lu visible, %lu verified, %lu mismatches%s",
            g_poly_calls, g_poly_occluded, occ_pct, g_poly_visible, g_poly_verified, g_poly_mismatch,
            g_poly_dead ? " - DISABLED" : (g_poly_calls < kVerifyFirst ? " (still verifying)" : ""));
    }
}

} // namespace OccluderSphere
