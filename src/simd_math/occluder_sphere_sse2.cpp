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

struct OccluderVolume {
    uint32_t first_plane;
    uint32_t plane_count;
};

struct VertexBlock4 {
    __m128 vx;
    __m128 vy;
    __m128 vz;
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
    if (g_sphere_dead || !sphere) {
        return orig_TestSphere(sphere);
    }

    ++g_sphere_calls;
    int mine = 0;

    __try {
        mine = TestSphere_Fast(sphere);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        g_sphere_dead = true;
        Log("[OccluderSphere] Exception during fast sphere test, retiring hook");
        return orig_TestSphere(sphere);
    }

    if (mine) {
        ++g_sphere_occluded;
    } else {
        ++g_sphere_visible;
    }

    const bool should_verify = (g_sphere_calls <= kVerifyFirst) || ((g_sphere_calls & kResampleMask) == 0);
    if (should_verify) {
        const int orig = orig_TestSphere(sphere);
        if (mine != orig) {
            // One is enough: every call between two samples returns this answer
            // unchecked, and a wrong answer here is an object culled that should
            // be drawn. The plane sums below are in single precision and the
            // client's are in 53-bit x87, so a sphere on a plane's boundary is
            // exactly where the two can disagree.
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

    // Typical bounding vertices passed from sub_7A85E0 are <= 12.
    // Support up to 32 vertices (8 blocks) on stack.
    if (count > 32) {
        return orig_TestPolygon(vertices, count);
    }

    const uint32_t num_blocks = (count + 3) / 4;
    VertexBlock4 blocks[8];

    if (count == 8) {
        // Direct fast-path for standard 8-vertex bounding box
        blocks[0].vx = _mm_set_ps(vertices[9],  vertices[6],  vertices[3],  vertices[0]);
        blocks[0].vy = _mm_set_ps(vertices[10], vertices[7],  vertices[4],  vertices[1]);
        blocks[0].vz = _mm_set_ps(vertices[11], vertices[8],  vertices[5],  vertices[2]);
        blocks[1].vx = _mm_set_ps(vertices[21], vertices[18], vertices[15], vertices[12]);
        blocks[1].vy = _mm_set_ps(vertices[22], vertices[19], vertices[16], vertices[13]);
        blocks[1].vz = _mm_set_ps(vertices[23], vertices[20], vertices[17], vertices[14]);
    } else if (count == 4) {
        // Direct fast-path for 4-vertex quad / portal
        blocks[0].vx = _mm_set_ps(vertices[9],  vertices[6],  vertices[3],  vertices[0]);
        blocks[0].vy = _mm_set_ps(vertices[10], vertices[7],  vertices[4],  vertices[1]);
        blocks[0].vz = _mm_set_ps(vertices[11], vertices[8],  vertices[5],  vertices[2]);
    } else {
        // Pad unused lanes in the final block with vertex 0 coordinates so that
        // the padding lanes never evaluate dist > 0.0f when vertex 0 is inside.
        const float pad_x = vertices[0];
        const float pad_y = vertices[1];
        const float pad_z = vertices[2];

        for (uint32_t b = 0; b < num_blocks; ++b) {
            alignas(16) float xs[4];
            alignas(16) float ys[4];
            alignas(16) float zs[4];
            for (uint32_t lane = 0; lane < 4; ++lane) {
                const uint32_t v_idx = b * 4 + lane;
                if (v_idx < count) {
                    xs[lane] = vertices[v_idx * 3 + 0];
                    ys[lane] = vertices[v_idx * 3 + 1];
                    zs[lane] = vertices[v_idx * 3 + 2];
                } else {
                    xs[lane] = pad_x;
                    ys[lane] = pad_y;
                    zs[lane] = pad_z;
                }
            }
            blocks[b].vx = _mm_load_ps(xs);
            blocks[b].vy = _mm_load_ps(ys);
            blocks[b].vz = _mm_load_ps(zs);
        }
    }

    const __m128 zero = _mm_setzero_ps();
    for (uint32_t i = 0; i < num_occluders; ++i) {
        const uint32_t plane_count = occluders[i].plane_count;
        const float* p = all_planes + (occluders[i].first_plane * 4);
        bool volume_occluded = true;

        for (uint32_t pi = 0; pi < plane_count; ++pi) {
            const __m128 nx = _mm_set1_ps(p[0]);
            const __m128 ny = _mm_set1_ps(p[1]);
            const __m128 nz = _mm_set1_ps(p[2]);
            const __m128 nd = _mm_set1_ps(p[3]);

            bool plane_passed = true;
            for (uint32_t b = 0; b < num_blocks; ++b) {
                // dot4 = (nx*vx + ny*vy) + (nz*vz + nd)
                const __m128 dot4 = _mm_add_ps(
                    _mm_add_ps(_mm_mul_ps(nx, blocks[b].vx), _mm_mul_ps(ny, blocks[b].vy)),
                    _mm_add_ps(_mm_mul_ps(nz, blocks[b].vz), nd)
                );

                // If any vertex is in front of the plane (dist > 0.0f), the plane fails
                const __m128 cmp = _mm_cmpgt_ps(dot4, zero);
                if (_mm_movemask_ps(cmp) != 0) {
                    plane_passed = false;
                    break;
                }
            }

            if (!plane_passed) {
                volume_occluded = false;
                break;
            }

            p += 4;
        }

        if (volume_occluded) {
            // All planes of this occluder volume contain all vertices: occluded!
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
    int mine = 0;

    __try {
        mine = TestPolygon_Fast(vertices, count);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        g_poly_dead = true;
        Log("[OccluderSphere] Exception during fast polygon test, retiring hook");
        return orig_TestPolygon(vertices, count);
    }

    if (mine) {
        ++g_poly_occluded;
    } else {
        ++g_poly_visible;
    }

    const bool should_verify = (g_poly_calls <= kVerifyFirst) || ((g_poly_calls & kResampleMask) == 0);
    if (should_verify) {
        const int orig = orig_TestPolygon(vertices, count);
        if (mine != orig) {
            // One is enough, for the same reason as the sphere test above.
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
