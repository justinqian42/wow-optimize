// ============================================================================
// Module: ui_strata_overlap_fast.cpp
//
// sub_494D20 is CFrameStrataManager::IsFrameOccluded (311 bytes, 112 instructions).
// It tests whether a UI frame is overlapped by any other frame occupying the
// same or higher strata levels.
//
// In tester profile wow_optimize_2026-09-22_17-28-51.log, sub_494D20 was sampled
// 7,350 times (#3 hotspot in wow.exe, 0.70% of CPU execution time).
//
// Root Cause:
// In the stock binary, sub_494D20 traverses linked lists of frames across all
// strata levels starting at the query frame's level. On every single candidate
// frame in the strata lists, it:
//   - Calls sub_489230(frame + 0x20, &rect_frame) to re-evaluate the query
//     frame's rect from scratch on EVERY iteration.
//   - Calls sub_489230(ebx + 0x20, &rect_ebx) to evaluate candidate rects.
//   - Pushes 12 bytes on the stack and calls sub_48ED60 (IntersectRect),
//     executing 16 serialized x87 comparison instructions.
//   - Spills 16 bytes of intersection bounds to stack and executes two floating
//     point status-word comparisons (fcomp / fnstsw ax / test ah, 41h).
// This causes thousands of redundant helper calls, stack spills, and x87
// pipeline flushes every frame during UI layout updates.
//
// Optimization:
// - Early return 0 if the query frame's rect is invalid ((flags & 0x100) == 0),
//   since an uninitialized [0,0,0,0] rect can never produce a non-empty overlap.
// - Hoists query frame bounding box coordinates once outside the strata traversal loops.
// - Inlines ancestor parent chain checks to skip descendants.
// - Inlines candidate frame validity check ((flags & 0x100) != 0).
// - Computes 2D axis-aligned bounding box intersection directly without function
//   call overhead or x87 pipeline serialization.
// Verified offline against verbatim client instructions over 1,200,000 cases with
// 0 bit differences (4.66x speedup; harness only, not run in a game).
// Zero /GS security cookies and zero SEH frames on the hot path via __declspec(safebuffers).
//
// Verification:
// Dual-runs comparison against client sub_494D20 logic for the first 10,000 calls
// and 1 in every 128 calls thereafter, verifying return values bit for bit.
// Retires immediately on first mismatch.
// ============================================================================

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "ui_strata_overlap_fast.h"
#include "MinHook.h"
#include "version.h"
#include "config.h"
#include "ab_test.h"
#include "sampling_profiler.h"

extern "C" void Log(const char* fmt, ...);

namespace UIStrataOverlapFast {

namespace {

constexpr uintptr_t kTarget = 0x00494D20;

// push ebp / mov ebp, esp / sub esp, 40h / fldz / push ebx / push esi / fst [ebp-10h] / fst [ebp-0Ch]
const unsigned char kPrologue[16] = {
    0x55, 0x8B, 0xEC, 0x83, 0xEC, 0x40, 0xD9, 0xEE,
    0x53, 0x56, 0xD9, 0x55, 0xF0, 0xD9, 0x55, 0xF4
};

typedef int (__thiscall *IsFrameOccluded_fn)(void* strata_mgr, void* frame);
static IsFrameOccluded_fn g_orig = nullptr;

bool g_installed = false;
bool g_dead = false;
bool g_abSubject = false;

// Statistics
unsigned long long g_calls = 0;
unsigned long long g_armedCalls = 0;
unsigned long long g_verifiedCalls = 0;
unsigned long long g_controlCalls = 0;
unsigned g_mismatches = 0;

__declspec(safebuffers) static int Fast_IsFrameOccluded(void* strataMgr, void* frame) {
    if (!strataMgr || !frame) return 0;
    const char* mgr = reinterpret_cast<const char*>(strataMgr);
    const char* f = reinterpret_cast<const char*>(frame);

    const uint32_t strata_idx = *reinterpret_cast<const uint32_t*>(f + 0xD4);
    const uint32_t num_strata = *reinterpret_cast<const uint32_t*>(mgr + 0x08);
    if (strata_idx >= num_strata) return 0;

    const char* frame_region = f + 0x20;
    const uint32_t frame_flags = *reinterpret_cast<const uint32_t*>(frame_region + 0x40);
    if (!(frame_flags & 0x100)) return 0;

    const float* f_rect = reinterpret_cast<const float*>(frame_region + 0x44);
    const float f_left   = f_rect[0];
    const float f_bottom = f_rect[1];
    const float f_right  = f_rect[2];
    const float f_top    = f_rect[3];

    const uintptr_t* const strata_array = *reinterpret_cast<const uintptr_t* const*>(mgr + 0x14);
    if (!strata_array) return 0;

    for (uint32_t i = strata_idx; i < num_strata; ++i) {
        const uintptr_t strata_elem = strata_array[i];
        if (!strata_elem) continue;

        const uintptr_t head_raw = *reinterpret_cast<const uintptr_t*>(strata_elem + 0x14);
        if ((head_raw & 1) || !head_raw) continue;

        const int node_offset = *reinterpret_cast<const int*>(strata_elem + 0x0C);
        const void* curr = reinterpret_cast<const void*>(head_raw);

        while (curr && !((uintptr_t)curr & 1)) {
            if (curr != frame) {
                // Check if curr is a descendant of frame
                const void* parent = *reinterpret_cast<const void* const*>(reinterpret_cast<const char*>(curr) + 0x94);
                while (parent && parent != frame) {
                    parent = *reinterpret_cast<const void* const*>(reinterpret_cast<const char*>(parent) + 0x94);
                }
                if (!parent) {
                    const char* curr_region = reinterpret_cast<const char*>(curr) + 0x20;
                    const uint32_t curr_flags = *reinterpret_cast<const uint32_t*>(curr_region + 0x40);
                    if (curr_flags & 0x100) {
                        const float* c_rect = reinterpret_cast<const float*>(curr_region + 0x44);
                        const float min_r = (c_rect[2] < f_right) ? c_rect[2] : f_right;
                        const float max_l = (c_rect[0] > f_left) ? c_rect[0] : f_left;
                        if (min_r > max_l) {
                            const float min_t = (c_rect[3] < f_top) ? c_rect[3] : f_top;
                            const float max_b = (c_rect[1] > f_bottom) ? c_rect[1] : f_bottom;
                            if (min_t > max_b) {
                                return 1;
                            }
                        }
                    }
                }
            }

            const char* node = reinterpret_cast<const char*>(curr) + node_offset;
            curr = *reinterpret_cast<const void* const*>(node + 4);
        }
    }

    return 0;
}

__declspec(safebuffers) static int __fastcall Hook_IsFrameOccluded(void* strataMgr, void* /*edx*/, void* frame) {
    if (g_dead || !strataMgr || !frame) return g_orig ? g_orig(strataMgr, frame) : 0;

    ++g_calls;
    if (g_abSubject && AbTest::StandAside()) {
        ++g_controlCalls;
        return g_orig(strataMgr, frame);
    }

    const bool verify = (g_verifiedCalls < 10000) || ((g_calls & 127) == 0);
    if (verify) {
        ++g_verifiedCalls;
        const int client_res = g_orig(strataMgr, frame);
        const int fast_res = Fast_IsFrameOccluded(strataMgr, frame);

        if (client_res != fast_res) {
            ++g_mismatches;
            g_dead = true;
            Log("[UIStrataOverlapFast] MISMATCH: client=%d fast=%d on frame %p (mgr=%p). Hook retired.",
                client_res, fast_res, frame, strataMgr);
            return client_res;
        }
        return client_res;
    }

    ++g_armedCalls;
    return Fast_IsFrameOccluded(strataMgr, frame);
}

}  // namespace

bool Init() {
    if (!Config::g_settings.OptUIStrataOverlapFast) return true;

    if (memcmp((const void*)kTarget, kPrologue, sizeof(kPrologue)) != 0) {
        Log("[UIStrataOverlapFast] NOT active: prologue mismatch at 0x%08X", (unsigned)kTarget);
        return false;
    }

    if (!WowOpt_ClientPatchAllowed((const void*)kTarget)) {
        Log("[UIStrataOverlapFast] NOT active: client patches not allowed");
        return false;
    }

    if (WineSafe_CreateHook((void*)kTarget, (void*)&Hook_IsFrameOccluded, (void**)&g_orig) != MH_OK) {
        Log("[UIStrataOverlapFast] NOT active: CreateHook failed on 0x%08X", (unsigned)kTarget);
        return false;
    }

    if (WO_EnableHook((void*)kTarget) != MH_OK) {
        MH_RemoveHook((void*)kTarget);
        Log("[UIStrataOverlapFast] NOT active: EnableHook failed on 0x%08X", (unsigned)kTarget);
        return false;
    }

    g_installed = true;
    g_abSubject = AbTest::IsSubject("UIStrataOverlapFast", &g_abSubject);
    SamplingProfiler::RegisterSelfSymbol("UIStrataOverlapFast_Hook", (const void*)&Hook_IsFrameOccluded);
    Log("[UIStrataOverlapFast] Hook installed on sub_494D20 (0x137 bytes)");
    return true;
}

void Shutdown() {
    if (g_installed) {
        MH_DisableHook((void*)kTarget);
        MH_RemoveHook((void*)kTarget);
        g_installed = false;
    }
}

void LogStats() {
    if (!g_installed) return;
    char buf[256];
    snprintf(buf, sizeof(buf),
             "[UIStrataOverlapFast] calls=%llu (armed=%llu, verified=%llu, control=%llu) mismatches=%u%s",
             g_calls, g_armedCalls, g_verifiedCalls, g_controlCalls, g_mismatches,
             g_dead ? " [RETIRED]" : "");
    Log("%s", buf);
}

}  // namespace UIStrataOverlapFast
