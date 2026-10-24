// ============================================================================
// Module: scene_node_active_fast.cpp
//
// Fastpath for SceneNode::SetActive (sub_823F10, 119 bytes).
// In field profile wow_optimize_2026-10-12_08-56-28.log, sub_823F10 was a top
// hotspot (5,343 samples at 0x00823F1A, 0.53% of executing CPU time), invoked
// by 29 callers across the engine whenever doodads, terrain map chunks, M2 models,
// spell visuals, missiles, or game objects toggle visibility/active state.
//
// Bottlenecks in the client implementation (0x00823F10 -> 0x00823F87, 119 bytes):
//   1. The client unconditionally creates a stack frame (push ebp / mov ebp, esp /
//      push esi / push edi) and evaluates active (edi) across redundant branches.
//   2. In culling loops (such as sub_799980 map chunk doodad processing), invisible
//      objects repeatedly call SetActive(0) when they are already inactive
//      (node->prev == nullptr). The client executes 15 instructions and 4 branches
//      to perform a no-op return.
//   3. When active == 0, the client's test of [esi+4] & 0x20 branches to the exact
//      same unlink path as when the flag is clear, introducing needless branch
//      mispredictions.
//
// Optimization:
//   - Fastpaths active == 0: if node->prev is null, immediately returns without
//     any stack frame allocation or register spills.
//   - When unlinking or linking, performs bit-exact pointer operations matching
//     the client's intrusive doubly linked list structure.
//   - Zero /GS security cookies (__declspec(safebuffers)) and zero SEH frames.
//   - Validates intrusive list invariant (*prev == this) on deactivation and
//     retires immediately on any discrepancy.
//   - Off by default under experimental switch SceneNodeActiveFast.
// ============================================================================

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <cstdint>
#include <cstring>

#include "scene_node_active_fast.h"
#include "MinHook.h"
#include "version.h"
#include "config.h"
#include "sampling_profiler.h"

extern "C" void Log(const char* fmt, ...);

namespace SceneNodeActiveFast {

namespace {

constexpr uintptr_t kTarget      = 0x00823F10;
constexpr uintptr_t kFnSub823ED0 = 0x00823ED0;

const uint8_t kTargetPrologue[16] = {
    0x55, 0x8B, 0xEC, 0x56, 0x8B, 0xF1, 0xF6, 0x46, 0x04, 0x20, 0x57, 0x8B, 0x7D, 0x08, 0x74, 0x11
};

typedef void (__fastcall* SceneNodeSetActive_fn)(void* this_ptr, void* dummy_edx, int active);
typedef int  (__fastcall* FnSub823ED0)(void* this_ptr, void* dummy_edx, int a2);

SceneNodeSetActive_fn g_orig_SetActive = nullptr;
const FnSub823ED0 fn_sub_823ED0 = (FnSub823ED0)kFnSub823ED0;

bool g_installed = false;
bool g_retired = false;

uint64_t g_calls = 0;
uint64_t g_fastpath_inactive_skips = 0;
uint64_t g_deactivations = 0;
uint64_t g_activations = 0;
uint64_t g_already_active_skips = 0;

__declspec(safebuffers) void __fastcall Hooked_SetActive(void* this_ptr, void* dummy_edx, int active) {
    if (g_retired || !this_ptr) {
        if (g_orig_SetActive && this_ptr) {
            g_orig_SetActive(this_ptr, dummy_edx, active);
        }
        return;
    }

    ++g_calls;

    if (!active) {
        uintptr_t* prev = *(uintptr_t**)((char*)this_ptr + 0x40);
        if (!prev) {
            ++g_fastpath_inactive_skips;
            return;
        }

        // Safety invariant check: if node is in a list, *prev must point to this node
        if (*prev != (uintptr_t)this_ptr) {
            g_retired = true;
            Log("[SceneNodeActiveFast] RETIRED: list invariant violation on deactivate (prev %p -> %p != this %p)",
                prev, (void*)*prev, this_ptr);
            g_orig_SetActive(this_ptr, dummy_edx, active);
            return;
        }

        uintptr_t next = *(uintptr_t*)((char*)this_ptr + 0x44);
        *prev = next;
        if (next) {
            *(uintptr_t*)((char*)next + 0x40) = (uintptr_t)prev;
        }
        *(uintptr_t*)((char*)this_ptr + 0x40) = 0;
        *(uintptr_t*)((char*)this_ptr + 0x44) = 0;
        ++g_deactivations;
        return;
    }

    // active != 0
    if ((*(const uint8_t*)((char*)this_ptr + 4) & 0x20) != 0) {
        if ((*(const uint8_t*)((char*)this_ptr + 0x10) & 1) == 0) {
            fn_sub_823ED0(this_ptr, nullptr, 0);
        }
    }

    if (*(uintptr_t*)((char*)this_ptr + 0x40) != 0) {
        ++g_already_active_skips;
        return;
    }

    uintptr_t parent = *(uintptr_t*)((char*)this_ptr + 0x28);
    if (!parent) {
        g_retired = true;
        Log("[SceneNodeActiveFast] RETIRED: null parent on activate (this %p)", this_ptr);
        g_orig_SetActive(this_ptr, dummy_edx, active);
        return;
    }

    uintptr_t* head = (uintptr_t*)(parent + 0x28);
    *(uintptr_t**)((char*)this_ptr + 0x40) = head;
    uintptr_t next = *head;
    *(uintptr_t*)((char*)this_ptr + 0x44) = next;
    *head = (uintptr_t)this_ptr;
    if (next) {
        *(uintptr_t*)((char*)next + 0x40) = (uintptr_t)((char*)this_ptr + 0x44);
    }
    ++g_activations;
}

} // namespace

bool Init() {
    if (!Config::g_settings.OptSceneNodeActiveFast) {
        return true;
    }

    if (memcmp((const void*)kTarget, kTargetPrologue, sizeof(kTargetPrologue)) != 0) {
        Log("[SceneNodeActiveFast] NOT active: prologue bytes mismatch at 0x%08X", kTarget);
        return false;
    }

    MH_STATUS status = WineSafe_CreateHook((void*)kTarget, (void*)Hooked_SetActive, (void**)&g_orig_SetActive);
    if (status != MH_OK) {
        Log("[SceneNodeActiveFast] NOT active: WineSafe_CreateHook failed (%d) at 0x%08X", status, kTarget);
        return false;
    }

    status = WO_EnableHook((void*)kTarget);
    if (status != MH_OK) {
        Log("[SceneNodeActiveFast] NOT active: WO_EnableHook failed (%d) at 0x%08X", status, kTarget);
        return false;
    }

    SamplingProfiler::RegisterSelfSymbol("SceneNodeActiveFast", (const void*)&Hooked_SetActive);
    g_installed = true;
    Log("[SceneNodeActiveFast] ACTIVE on sub_823F10 (SceneNode::SetActive, 119 bytes, off by default).");
    return true;
}

void Shutdown() {
    if (g_installed) {
        g_installed = false;
        Log("[SceneNodeActiveFast] Shutdown completed.");
    }
}

void LogStats() {
    if (!Config::g_settings.OptSceneNodeActiveFast) {
        return;
    }
    if (!g_installed) {
        Log("[SceneNodeActiveFast] not installed.");
        return;
    }
    if (g_retired) {
        Log("[SceneNodeActiveFast] RETIRED after inconsistency.");
        return;
    }

    Log("[SceneNodeActiveFast] %llu calls (%llu fastpath inactive skips, %llu deactivations, %llu activations, %llu already-active skips).",
        g_calls, g_fastpath_inactive_skips, g_deactivations, g_activations, g_already_active_skips);
}

} // namespace SceneNodeActiveFast
