// ============================================================================
// Module: horizon_test_sse2.cpp
//
// Full AABB and Sphere horizon occlusion queries:
//   - sub_78FDC0: AABB horizon query (0x1D9 / 473 bytes)
//   - sub_78FC40: Sphere horizon query (0x170 / 368 bytes)
//
// In profile logs, horizon raster occlusion testing (sub_78FDC0, sub_78FC40)
// and their projection loops accounted for substantial CPU execution time.
// Every terrain chunk, WMO building, doodad (trees, rocks, props), and entity
// (players, creatures, NPCs) that survives frustum testing evaluates horizon
// occlusion before being admitted into render passes.
//
// The stock client functions project corners/extents by calling sub_4C21B0
// (3D point * 4x4 matrix, 37 instructions each), execute scalar x87 divisions
// (1.0 / out.z), track min/max projected coordinates using x87 comparisons with
// status-word transfers (fnstsw ax), derive integer screen column bounds
// [min_col, max_col], and scan the 384-column terrain horizon array.
//
// This replacement:
//   - Evaluates bounding geometry with inlined 3D point * 4x4 matrix math using
//     IEEE double precision in client operation order, achieving bit-exact
//     parity with the client's 53-bit x87 precision.
//   - Uses vector SSE2 instructions for column coordinate rounding and 4-wide
//     packed horizon column buffer scanning.
//   - Avoids all per-corner/per-sphere helper call overhead and x87 status-word
//     pipeline stalls.
//
// Verification: Compared with the client on the first 10,000 calls and one
// call in 128 after, retiring on the first difference.
// ============================================================================

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <emmintrin.h>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cmath>

#include "horizon_test_sse2.h"
#include "MinHook.h"
#include "version.h"
#include "config.h"
#include "sampling_profiler.h"
#include "ab_test.h"

extern "C" void Log(const char* fmt, ...);

namespace HorizonTestSSE2 {

namespace {

constexpr uintptr_t kTargetAABB   = 0x0078FDC0;
constexpr uintptr_t kTargetSphere = 0x0078FC40;

// push ebp / mov ebp, esp / sub esp, 3Ch / test byte ptr ds:CD774C, 20h
const unsigned char kPrologueAABB[8] = {
    0x55, 0x8B, 0xEC, 0x83, 0xEC, 0x3C, 0xF6, 0x05
};

// push ebp / mov ebp, esp / sub esp, 2Ch / test byte ptr ds:CD774C, 20h
const unsigned char kPrologueSphere[8] = {
    0x55, 0x8B, 0xEC, 0x83, 0xEC, 0x2C, 0xF6, 0x05
};

// Client global pointers
static const uint32_t* const dword_CD774C = (const uint32_t*)0x00CD774C;
static const float*    const flt_CD8F7C   = (const float*)0x00CD8F7C;
static const float*    const kMatrix      = (const float*)0x00ADF460;
static const float*    const kProjMatrix  = (const float*)0x00ADF628;
static const float*    const kHorizon     = (const float*)0x00CD8938;

// Corner permutation tables from client (0x00ADF3F4, 0x00ADF414, 0x00ADF434)
static const uint32_t kCornerX[8] = { 0, 1, 1, 0, 0, 1, 1, 0 };
static const uint32_t kCornerY[8] = { 0, 0, 1, 1, 0, 0, 1, 1 };
static const uint32_t kCornerZ[8] = { 0, 0, 0, 0, 1, 1, 1, 1 };

typedef int (__cdecl* HorizonTest_fn)(const float* box, char flags);
static HorizonTest_fn g_orig_HorizonTest = nullptr;

typedef int (__cdecl* HorizonTestSphere_fn)(const float* center, float radius, char flags);
static HorizonTestSphere_fn g_orig_HorizonTestSphere = nullptr;

static bool g_active = false;
static bool g_dead_aabb   = false;
static bool g_dead_sphere = false;
static bool g_abSubject   = false;

static unsigned long long g_calls_aabb    = 0;
static unsigned long      g_verified_aabb = 0;
static unsigned long      g_mismatch_aabb = 0;

static unsigned long long g_calls_sphere    = 0;
static unsigned long      g_verified_sphere = 0;
static unsigned long      g_mismatch_sphere = 0;

static const unsigned long kVerifyCount      = 10000;
static const unsigned long kVerifySampleMask = 0x7F; // 1 in 128 thereafter

// Inlined 3D point * 4x4 matrix matching client sub_4C21B0 operation order
static inline void TransformPoint(const float* M, float x, float y, float z, float* out) {
    const double d_x = (double)x;
    const double d_y = (double)y;
    const double d_z = (double)z;

    // x: ((M[8]*z + M[4]*y) + M[0]*x) + M[12]
    const double sx = ((double)M[8] * d_z + (double)M[4] * d_y) + (double)M[0] * d_x + (double)M[12];
    out[0] = (float)sx;

    // y: ((M[9]*z + M[5]*y) + M[1]*x) + M[13]
    const double sy = ((double)M[9] * d_z + (double)M[5] * d_y) + (double)M[1] * d_x + (double)M[13];
    out[1] = (float)sy;

    // z: ((M[10]*z + M[6]*y) + M[2]*x) + M[14]
    const double sz = ((double)M[10] * d_z + (double)M[6] * d_y) + (double)M[2] * d_x + (double)M[14];
    out[2] = (float)sz;
}

// Scans horizon columns [min_col, max_col] using 4-wide SSE2 vector comparisons
static inline int ScanHorizon(int min_col, int max_col, float max_proj_y) {
    if (min_col >= 384 || max_col < 0) return 0;
    if (min_col < 0) min_col = 0;
    if (max_col >= 384) max_col = 383;
    if (min_col > max_col) return 2;

    const __m128 v = _mm_set1_ps(max_proj_y);
    int c = min_col;
    for (; c + 3 <= max_col; c += 4) {
        const __m128 h = _mm_loadu_ps(kHorizon + c);
        const int m = _mm_movemask_ps(_mm_cmpgt_ps(v, h));
        if (m) {
            return 0;
        }
    }
    for (; c <= max_col; ++c) {
        if (max_proj_y > kHorizon[c]) return 0;
    }

    return 2; // occluded
}

// Fast horizon visibility query matching client sub_78FDC0 bit for bit
static inline int Fast_HorizonTest(const float* box, char flags) {
    if (!(*dword_CD774C & 0x20)) return 0;

    const float pitch = *flt_CD8F7C;
    if (pitch < -0.8999999761581421f || pitch > 0.8999999761581421f) return 0;

    const float* b_min = box;
    const float* b_max = box + 3;
    const bool allow_near = (flags & 8) != 0;

    double min_proj_x = 3.4028234663852886e+38;
    double max_proj_x = -3.4028234663852886e+38;
    float  max_proj_y = -3.4028234663852886e+38f;

    for (int i = 0; i < 8; ++i) {
        const float px = kCornerX[i] ? b_max[0] : b_min[0];
        const float py = kCornerY[i] ? b_max[1] : b_min[1];
        const float pz = kCornerZ[i] ? b_max[2] : b_min[2];

        float out[3];
        TransformPoint(kMatrix, px, py, pz, out);

        if (!allow_near && out[2] < 50.0f) {
            return 0;
        }

        const double inv_z = 1.0 / (double)out[2];
        const double proj_x = (double)out[0] * inv_z;
        const double proj_y = (double)out[1] * inv_z;

        if (proj_x < min_proj_x) min_proj_x = proj_x;
        if (proj_x > max_proj_x) max_proj_x = proj_x;

        const float f_proj_y = (float)proj_y;
        if (f_proj_y > max_proj_y) max_proj_y = f_proj_y;
    }

    const float min_scaled = (float)(min_proj_x * 64.0);
    const int min_col_raw  = _mm_cvtss_si32(_mm_sub_ss(_mm_set_ss(min_scaled), _mm_set_ss(0.5f)));
    int min_col = min_col_raw + 192;

    const float max_scaled = (float)(max_proj_x * 64.0);
    const int max_col_raw  = _mm_cvtss_si32(_mm_sub_ss(_mm_set_ss(max_scaled), _mm_set_ss(0.5f)));
    int max_col = max_col_raw + 193;

    return ScanHorizon(min_col, max_col, max_proj_y);
}

// Fast sphere horizon visibility query matching client sub_78FC40 bit for bit
static inline int Fast_HorizonTestSphere(const float* center, float radius, char flags) {
    if (!(*dword_CD774C & 0x20)) return 0;

    if (std::abs(radius) < 0.00000023841858f) return 0;

    const float pitch = *flt_CD8F7C;
    if (pitch < -0.8999999761581421f || pitch > 0.8999999761581421f) return 0;

    float view[3];
    TransformPoint(kMatrix, center[0], center[1], center[2], view);

    float extent[3];
    TransformPoint(kProjMatrix, radius, radius, 0.0f, extent);

    const bool allow_near = (flags & 8) != 0;
    if (!allow_near && view[2] < 50.0f) {
        return 0;
    }

    const double inv_z = 1.0 / (double)view[2];
    const double v_x = (double)view[0] * inv_z;
    const double ext_x = (double)extent[0] * inv_z;
    const double ext_y = (double)extent[1] * inv_z;
    const float  v_y = (float)(ext_x + inv_z * (double)view[1]);

    const float min_scaled = (float)((v_x - ext_y) * 64.0);
    const int min_col_raw  = _mm_cvtss_si32(_mm_sub_ss(_mm_set_ss(min_scaled), _mm_set_ss(0.5f)));
    int min_col = min_col_raw + 192;

    const float max_scaled = (float)((ext_y + v_x) * 64.0);
    const int max_col_raw  = _mm_cvtss_si32(_mm_sub_ss(_mm_set_ss(max_scaled), _mm_set_ss(0.5f)));
    int max_col = max_col_raw + 193;

    return ScanHorizon(min_col, max_col, v_y);
}

// Detour hook for sub_78FDC0. safebuffers ensures no /GS cookie on hot path.
__declspec(safebuffers) static int __cdecl Hook_HorizonTest(const float* box, char flags) {
    ++g_calls_aabb;

    if (g_dead_aabb || (g_abSubject && AbTest::StandAside())) {
        return g_orig_HorizonTest(box, flags);
    }

    const int mine = Fast_HorizonTest(box, flags);

    if (g_verified_aabb < kVerifyCount || ((g_calls_aabb & kVerifySampleMask) == 0)) {
        const int theirs = g_orig_HorizonTest(box, flags);
        if (mine != theirs) {
            ++g_mismatch_aabb;
            g_dead_aabb = true;
            Log("[HorizonTest] AABB DISAGREEMENT: mine=%d, client=%d, flags=0x%02X. "
                "Retiring replacement for session.", mine, theirs, (uint8_t)flags);
            return theirs;
        }
        ++g_verified_aabb;
    }

    return mine;
}

// Detour hook for sub_78FC40. safebuffers ensures no /GS cookie on hot path.
__declspec(safebuffers) static int __cdecl Hook_HorizonTestSphere(const float* center, float radius, char flags) {
    ++g_calls_sphere;

    if (g_dead_sphere || (g_abSubject && AbTest::StandAside())) {
        return g_orig_HorizonTestSphere(center, radius, flags);
    }

    const int mine = Fast_HorizonTestSphere(center, radius, flags);

    if (g_verified_sphere < kVerifyCount || ((g_calls_sphere & kVerifySampleMask) == 0)) {
        const int theirs = g_orig_HorizonTestSphere(center, radius, flags);
        if (mine != theirs) {
            ++g_mismatch_sphere;
            g_dead_sphere = true;
            Log("[HorizonTest] Sphere DISAGREEMENT: mine=%d, client=%d, r=%f, flags=0x%02X. "
                "Retiring replacement for session.", mine, theirs, radius, (uint8_t)flags);
            return theirs;
        }
        ++g_verified_sphere;
    }

    return mine;
}

} // anonymous namespace

bool Init() {
    if (!Config::g_settings.OptHorizonTestAABB) {
        return true;
    }

    bool aabb_ok = false;
    bool sphere_ok = false;

    // 1. Hook AABB Horizon Test (sub_78FDC0)
    if (WowOpt_ClientPatchAllowed((const void*)kTargetAABB)) {
        if (memcmp((const void*)kTargetAABB, kPrologueAABB, sizeof(kPrologueAABB)) == 0) {
            if (WineSafe_CreateHook((void*)kTargetAABB, (void*)Hook_HorizonTest, (void**)&g_orig_HorizonTest) == MH_OK &&
                WO_EnableHook((void*)kTargetAABB) == MH_OK) {
                aabb_ok = true;
            } else {
                Log("[HorizonTest] Failed to create/enable hook at 0x%08X", (unsigned)kTargetAABB);
            }
        } else {
            Log("[HorizonTest] NOT hooked: prologue mismatch at 0x%08X", (unsigned)kTargetAABB);
        }
    }

    // 2. Hook Sphere Horizon Test (sub_78FC40)
    if (WowOpt_ClientPatchAllowed((const void*)kTargetSphere)) {
        if (memcmp((const void*)kTargetSphere, kPrologueSphere, sizeof(kPrologueSphere)) == 0) {
            if (WineSafe_CreateHook((void*)kTargetSphere, (void*)Hook_HorizonTestSphere, (void**)&g_orig_HorizonTestSphere) == MH_OK &&
                WO_EnableHook((void*)kTargetSphere) == MH_OK) {
                sphere_ok = true;
            } else {
                Log("[HorizonTest] Failed to create/enable hook at 0x%08X", (unsigned)kTargetSphere);
            }
        } else {
            Log("[HorizonTest] NOT hooked: prologue mismatch at 0x%08X", (unsigned)kTargetSphere);
        }
    }

    if (aabb_ok || sphere_ok) {
        g_active = true;
        g_abSubject = AbTest::IsSubject("HorizonTestAABB", &g_abSubject);
        Log("[HorizonTest] ACTIVE on AABB (%s @ 0x%08X) and Sphere (%s @ 0x%08X). Verifying first %lu calls.",
            aabb_ok ? "OK" : "SKIP", (unsigned)kTargetAABB,
            sphere_ok ? "OK" : "SKIP", (unsigned)kTargetSphere,
            kVerifyCount);
    }

    return g_active;
}

void LogStats() {
    if (!g_active) return;

    if (g_orig_HorizonTest) {
        if (g_dead_aabb) {
            Log("[HorizonTest] AABB RETIRED due to %lu mismatch(es) after %llu call(s).",
                g_mismatch_aabb, g_calls_aabb);
        } else {
            Log("[HorizonTest] AABB: %llu call(s), %lu verified against client (0 mismatches).",
                g_calls_aabb, g_verified_aabb);
        }
    }

    if (g_orig_HorizonTestSphere) {
        if (g_dead_sphere) {
            Log("[HorizonTest] Sphere RETIRED due to %lu mismatch(es) after %llu call(s).",
                g_mismatch_sphere, g_calls_sphere);
        } else {
            Log("[HorizonTest] Sphere: %llu call(s), %lu verified against client (0 mismatches).",
                g_calls_sphere, g_verified_sphere);
        }
    }
}

void Shutdown() {
    if (!g_active) return;
    g_active = false;
    if (g_orig_HorizonTest) {
        MH_DisableHook((void*)kTargetAABB);
    }
    if (g_orig_HorizonTestSphere) {
        MH_DisableHook((void*)kTargetSphere);
    }
}

} // namespace HorizonTestSSE2
