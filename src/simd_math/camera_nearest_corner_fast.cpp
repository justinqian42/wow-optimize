// ============================================================================
// Module: camera_nearest_corner_fast.cpp
//
// Hardware-accelerated nearest AABB corner selection for WoW 3.3.5a:
//   - sub_790650: CWorldScene::GetNearestCornerToCamera(CAaBox* box, C3Vector* outCorner)
//
// In stock 3.3.5a:
//   - sub_790650 determines which corner of an Axis-Aligned Bounding Box (AABB)
//     is nearest to the camera along each axis for distance sorting and occlusion:
//       outCorner->x = (camTarget.x >= activeWorldView.x) ? box->b.x : box->t.x;
//       outCorner->y = (camTarget.y >= activeWorldView.y) ? box->b.y : box->t.y;
//       outCorner->z = (camTarget.z >= activeWorldView.z) ? box->b.z : box->t.z;
//   - The stock implementation uses three serial x87 comparisons (fld, fcomp,
//     fnstsw, test ah, 5, jp) re-reading global camera coordinates (0x00CD8F68
//     and 0x00CD8F5C) for every single model and terrain chunk in the scene.
//
// This optimization eliminates x87 pipeline stalls and branch mispredictions
// by using conditional moves / branchless selection directly from the AABB
// boundaries. Dual-run verification and automatic fail-safe retirement guards
// ensure strict parity with client output.
// ============================================================================

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <cstdint>
#include <cstring>

#include "camera_nearest_corner_fast.h"
#include "MinHook.h"
#include "version.h"
#include "config.h"
#include "sampling_profiler.h"

extern "C" void Log(const char* fmt, ...);

namespace CameraNearestCornerFast {

namespace {

constexpr uintptr_t kTarget_GetNearestCorner = 0x00790650;
constexpr uintptr_t kAddr_CamTarget          = 0x00CD8F68;
constexpr uintptr_t kAddr_ActiveWorldView    = 0x00CD8F5C;

// Verification settings
constexpr unsigned long kVerifyFirst = 20000;
constexpr unsigned long kResampleMask = 4095;

typedef void (__cdecl* GetNearestCorner_fn)(const float* box, float* outCorner);

GetNearestCorner_fn orig_GetNearestCorner = nullptr;

bool g_installed = false;
bool g_dead = false;

unsigned long long g_calls = 0;
unsigned long long g_verified = 0;
unsigned long long g_mismatches = 0;

__forceinline void Fast_GetNearestCorner_Impl(const float* box, float* outCorner) {
    const float* const camTarget = reinterpret_cast<const float*>(kAddr_CamTarget);
    const float* const activeWorldView = reinterpret_cast<const float*>(kAddr_ActiveWorldView);

    const bool pick_b_x = (camTarget[0] >= activeWorldView[0]);
    const bool pick_b_y = (camTarget[1] >= activeWorldView[1]);
    const bool pick_b_z = (camTarget[2] >= activeWorldView[2]);

    outCorner[0] = pick_b_x ? box[0] : box[3];
    outCorner[1] = pick_b_y ? box[1] : box[4];
    outCorner[2] = pick_b_z ? box[2] : box[5];
}

void __cdecl Hooked_GetNearestCorner(const float* box, float* outCorner) {
    if (g_dead || !box || !outCorner) {
        if (orig_GetNearestCorner && box && outCorner) {
            orig_GetNearestCorner(box, outCorner);
        }
        return;
    }

    const unsigned long long call_idx = ++g_calls;

    if (call_idx <= kVerifyFirst || ((call_idx & kResampleMask) == 0)) {
        float fast_out[3];
        Fast_GetNearestCorner_Impl(box, fast_out);

        float orig_out[3];
        orig_GetNearestCorner(box, orig_out);

        ++g_verified;

        if (fast_out[0] != orig_out[0] || fast_out[1] != orig_out[1] || fast_out[2] != orig_out[2]) {
            if (++g_mismatches == 1) {
                Log("[CameraNearestCornerFast] MISMATCH at call %llu: fast=(%f,%f,%f), orig=(%f,%f,%f), retiring hook",
                    call_idx, fast_out[0], fast_out[1], fast_out[2],
                    orig_out[0], orig_out[1], orig_out[2]);
                g_dead = true;
            }
            outCorner[0] = orig_out[0];
            outCorner[1] = orig_out[1];
            outCorner[2] = orig_out[2];
            return;
        }

        outCorner[0] = fast_out[0];
        outCorner[1] = fast_out[1];
        outCorner[2] = fast_out[2];
        return;
    }

    Fast_GetNearestCorner_Impl(box, outCorner);
}

} // namespace

void Init() {
    if (!Config::g_settings.OptCameraNearestCornerFast) {
        return;
    }

    static const unsigned char kExp_Prologue[8] = { 0x55, 0x8B, 0xEC, 0xD9, 0x05, 0x68, 0x8F, 0xCD };

    if (memcmp((const void*)kTarget_GetNearestCorner, kExp_Prologue, 8) != 0) {
        Log("[CameraNearestCornerFast] Prologue mismatch at 0x%08X, aborting installation", kTarget_GetNearestCorner);
        return;
    }

    if (WineSafe_CreateHook((void*)kTarget_GetNearestCorner, (void*)Hooked_GetNearestCorner, (void**)&orig_GetNearestCorner) != MH_OK) {
        Log("[CameraNearestCornerFast] Failed to create hook");
        return;
    }

    if (WO_EnableHook((void*)kTarget_GetNearestCorner) != MH_OK) {
        Log("[CameraNearestCornerFast] Failed to enable hook");
        return;
    }

    SamplingProfiler::RegisterSelfSymbol("Fast_GetNearestCornerToCamera", (const void*)&Hooked_GetNearestCorner);

    g_installed = true;
    Log("[CameraNearestCornerFast] ACTIVE on CWorldScene GetNearestCornerToCamera (0x00790650)");
}

void LogStats() {
    if (!Config::g_settings.OptCameraNearestCornerFast || !g_installed) {
        return;
    }

    Log("[CameraNearestCornerFast] Stats: calls=%llu (verified=%llu, mismatches=%llu%s)",
        g_calls, g_verified, g_mismatches, g_dead ? " [RETIRED]" : "");
}

void Shutdown() {
    if (!g_installed) {
        return;
    }

    MH_DisableHook((void*)kTarget_GetNearestCorner);
    g_installed = false;
}

} // namespace CameraNearestCornerFast
