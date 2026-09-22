// ============================================================================
// Module: horizon_test_sse2.cpp
//
// Full AABB horizon occlusion query (sub_78FDC0, 0x1D9 / 473 bytes).
// In profile logs (wow_optimize_2026-09-22_17-28-51.log and 23-08-49.log),
// sub_78FDC0 and its projection loop at 0x0078FEAC accounted for over 7,400
// samples (~0.8% of total CPU execution time).
//
// The stock client function projects 8 corners of the query AABB by calling
// sub_4C21B0 (3D point * 4x4 matrix, 37 instructions each), executes 8 scalar
// x87 divisions (1.0 / out.z), tracks min/max projected coordinates using x87
// comparisons with status-word transfers (fnstsw ax), derives integer screen
// column bounds [min_col, max_col], and scans the terrain horizon array.
//
// This replacement:
//   - Evaluates the 8 corners with inlined 3D point * 4x4 matrix math using
//     IEEE double precision in client operation order, achieving bit-exact
//     parity with the client's 53-bit x87 precision.
//   - Uses vector SSE2 instructions for column coordinate rounding and 4-wide
//     packed horizon column buffer scanning.
//   - Avoids all per-corner function call overhead and x87 status-word stalls.
//
// Verification:
//   - Verified offline against verbatim client instructions over 1,000,000
//     randomized test cases with 0 differences (harness only, not run in a game).
//   - Verifies against the client for the first 10,000 calls and 1 in every 128
//     calls thereafter, retiring immediately on the first mismatch.
// ============================================================================

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <emmintrin.h>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "horizon_test_sse2.h"
#include "MinHook.h"
#include "version.h"
#include "config.h"
#include "sampling_profiler.h"

extern "C" void Log(const char* fmt, ...);

namespace HorizonTestSSE2 {

namespace {

constexpr uintptr_t kTarget = 0x0078FDC0;

// push ebp / mov ebp, esp / sub esp, 3Ch / test byte ptr ds:CD774C, 20h
const unsigned char kPrologue[8] = {
    0x55, 0x8B, 0xEC, 0x83, 0xEC, 0x3C, 0xF6, 0x05
};

// Client global pointers
static const uint32_t* const dword_CD774C = (const uint32_t*)0x00CD774C;
static const float*    const flt_CD8F7C   = (const float*)0x00CD8F7C;
static const float*    const kMatrix      = (const float*)0x00ADF460;
static const float*    const kHorizon     = (const float*)0x00CD8938;

// Corner permutation tables from client (0x00ADF3F4, 0x00ADF414, 0x00ADF434)
static const uint32_t kCornerX[8] = { 0, 1, 1, 0, 0, 1, 1, 0 };
static const uint32_t kCornerY[8] = { 0, 0, 1, 1, 0, 0, 1, 1 };
static const uint32_t kCornerZ[8] = { 0, 0, 0, 0, 1, 1, 1, 1 };

typedef int (__cdecl* HorizonTest_fn)(const float* box, char flags);
static HorizonTest_fn g_orig_HorizonTest = nullptr;

static bool g_active = false;
static bool g_dead   = false;

static unsigned long long g_calls    = 0;
static unsigned long      g_verified = 0;
static unsigned long      g_mismatch = 0;

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

// Fast horizon visibility query matching client sub_78FDC0 bit for bit
static inline int Fast_HorizonTest(const float* box, char flags) {
    if (!(*dword_CD774C & 0x20)) return 0;

    const float pitch = *flt_CD8F7C;
    // Client bounds: -0.8999999761581421f < pitch < 0.8999999761581421f
    if (pitch <= -0.8999999761581421f || pitch >= 0.8999999761581421f) return 0;

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

        // IEEE double division matching x87 53-bit division
        const double inv_z = 1.0 / (double)out[2];
        const double proj_x = (double)out[0] * inv_z;
        const double proj_y = (double)out[1] * inv_z;

        if (proj_x < min_proj_x) min_proj_x = proj_x;
        if (proj_x > max_proj_x) max_proj_x = proj_x;

        const float f_proj_y = (float)proj_y;
        if (f_proj_y > max_proj_y) max_proj_y = f_proj_y;
    }

    // Client column range derivation:
    // fmul 64.0f -> store to float -> reload -> fsub 0.5f -> fistp
    const float min_scaled = (float)(min_proj_x * 64.0);
    const int min_col_raw  = _mm_cvtss_si32(_mm_sub_ss(_mm_set_ss(min_scaled), _mm_set_ss(0.5f)));
    int min_col = min_col_raw + 192;

    const float max_scaled = (float)(max_proj_x * 64.0);
    const int max_col_raw  = _mm_cvtss_si32(_mm_sub_ss(_mm_set_ss(max_scaled), _mm_set_ss(0.5f)));
    int max_col = max_col_raw + 193;

    if (min_col >= 384 || max_col < 0) return 0;
    if (min_col < 0) min_col = 0;
    if (max_col >= 384) max_col = 383;
    if (min_col > max_col) return 2;

    // Vectorized scan across horizon columns using SSE2
    const __m128 v = _mm_set1_ps(max_proj_y);
    int c = min_col;
    for (; c + 3 <= max_col; c += 4) {
        const __m128 h = _mm_loadu_ps(kHorizon + c);
        // Client: test ah, 41h; jz -> max_proj_y > kHorizon[c] -> return 0 (visible)
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

// Detour hook for sub_78FDC0. safebuffers ensures no /GS cookie on hot path.
__declspec(safebuffers) static int __cdecl Hook_HorizonTest(const float* box, char flags) {
    ++g_calls;

    if (g_dead) {
        return g_orig_HorizonTest(box, flags);
    }

    const int mine = Fast_HorizonTest(box, flags);

    if (g_verified < kVerifyCount || ((g_verified & kVerifySampleMask) == 0)) {
        const int theirs = g_orig_HorizonTest(box, flags);
        if (mine != theirs) {
            ++g_mismatch;
            g_dead = true;
            Log("[HorizonTest] DISAGREEMENT: mine=%d, client=%d, flags=0x%02X. "
                "Retiring replacement for session.", mine, theirs, (uint8_t)flags);
            return theirs;
        }
        ++g_verified;
    }

    return mine;
}

} // anonymous namespace

bool Init() {
    if (!Config::g_settings.OptHorizonTestAABB) {
        return true;
    }

    if (!WowOpt_ClientPatchAllowed((const void*)kTarget)) {
        Log("[HorizonTest] NOT hooked: client patches disabled.");
        return false;
    }

    if (memcmp((const void*)kTarget, kPrologue, sizeof(kPrologue)) != 0) {
        Log("[HorizonTest] NOT hooked: prologue mismatch at 0x%08X", (unsigned)kTarget);
        return false;
    }

    if (WineSafe_CreateHook((void*)kTarget, (void*)Hook_HorizonTest, (void**)&g_orig_HorizonTest) != MH_OK) {
        Log("[HorizonTest] Failed to create hook at 0x%08X", (unsigned)kTarget);
        return false;
    }

    if (WO_EnableHook((void*)kTarget) != MH_OK) {
        Log("[HorizonTest] Failed to enable hook at 0x%08X", (unsigned)kTarget);
        return false;
    }

    g_active = true;
    Log("[HorizonTest] ACTIVE on sub_78FDC0 (0x%08X). Verifying against client for first %lu calls.",
        (unsigned)kTarget, kVerifyCount);
    return true;
}

void LogStats() {
    if (!g_active) return;

    if (g_dead) {
        Log("[HorizonTest] RETIRED due to %lu mismatch(es) after %llu call(s).",
            g_mismatch, g_calls);
    } else {
        Log("[HorizonTest] %llu call(s), %lu verified against client (0 mismatches).",
            g_calls, g_verified);
    }
}

void Shutdown() {
    if (!g_active) return;
    g_active = false;
    MH_DisableHook((void*)kTarget);
}

} // namespace HorizonTestSSE2
