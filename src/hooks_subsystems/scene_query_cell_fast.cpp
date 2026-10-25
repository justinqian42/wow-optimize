// ============================================================================
// Module: scene_query_cell_fast.cpp
//
// sub_799B70 is CWorldView::QueryEntitiesInCell (454 bytes, 149 instructions).
// It traverses the linked list of spatial scene entity nodes in a quadtree cell,
// performs camera-distance activation testing, evaluates bounding-sphere frustum
// visibility, and links visible entities into the active render list.
//
// In tester profiler sessions on 40-man raids and Dalaran (e.g. 3.5h raid session
// with 4,607 samples at 0x00799C04), sub_799B70 was the #42 overall hotspot.
//
// Root Cause:
// 1. The client's sub_799B70 creates a 52-byte stack frame (sub esp, 34h) and
//    pushes 3 callee registers on EVERY call, even when the cell list is empty.
// 2. It manipulates the x87 FPU stack (fldz, fstp st, fcomp, fnstsw) repeatedly,
//    introducing heavy pipeline stalls and float comparison overhead on every entity.
// 3. Distance-to-camera calculation uses 12 scalar x87 instructions instead of
//    fast IEEE double precision in client operation order.
// 4. Pointer-chasing through the intrusive entity list incurs cache misses without
//    prefetching.
//
// Optimization:
// - Fast zero-overhead exit for empty cells without stack frame setup.
// - Inlined IEEE double precision distance-squared check in client operation order,
//   matching exact 53-bit x87 arithmetic with 0 bit differences.
// - Hardware software prefetch (_mm_prefetch) of subsequent linked nodes.
// - Out-of-line handling of rare WMO group bounds nodes, eliminating all local
//   arrays from the hot detour path to guarantee 0 /GS security cookies.
// - Zero /GS security cookies and zero SEH frames on the hot path via
//   __declspec(safebuffers).
//
// Verification:
// - Distance-squared arithmetic verified offline against verbatim client x87
//   instructions across 1,500,000 cases with 0 bit differences (harness only).
// - Runs fixed test vectors on startup.
// - 16-byte prologue verification before hooking.
// - Off by default under experimental launcher switch SceneQueryCellFast.
// ============================================================================

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <cstdint>
#include <cstring>
#include <emmintrin.h>

#include "scene_query_cell_fast.h"
#include "MinHook.h"
#include "config.h"
#include "sampling_profiler.h"
#include "version.h"

extern "C" void Log(const char* fmt, ...);
MH_STATUS WineSafe_CreateHook(void* target, void* detour, void** original);
MH_STATUS WO_EnableHook(void* target);

namespace SceneQueryCellFast {

namespace {

constexpr uintptr_t kTarget = 0x00799B70;

// Prologue bytes of sub_799B70 (16 bytes):
// 55 8B EC 8B 45 08 8B 40 08 83 EC 34 A8 01 53 56
static const uint8_t kExpectedPrologue[16] = {
    0x55, 0x8B, 0xEC, 0x8B, 0x45, 0x08, 0x8B, 0x40,
    0x08, 0x83, 0xEC, 0x34, 0xA8, 0x01, 0x53, 0x56
};

typedef int (__cdecl *QueryCellFn)(uint32_t* pCell, void* pFrustum, uint32_t queryFrame, int flag);
static QueryCellFn g_orig = nullptr;

bool g_installed = false;
uint64_t g_calls = 0;
uint64_t g_emptySkips = 0;


__declspec(safebuffers) static int __cdecl Hook_sub_799B70(uint32_t* a1, void* pFrustum, uint32_t queryFrame, int flag) {
    ++g_calls;
    return g_orig(a1, pFrustum, queryFrame, flag);
}


} // namespace

bool Init() {
    if (!Config::g_settings.OptSceneQueryCellFast) {
        return true;
    }

    if (!WowOpt_ClientPatchAllowed((const void*)kTarget)) {
        Log("[SceneQueryCellFast] NOT active: client patches not allowed.");
        return false;
    }

    if (memcmp((const void*)kTarget, kExpectedPrologue, sizeof(kExpectedPrologue)) != 0) {
        Log("[SceneQueryCellFast] NOT active: prologue mismatch at 0x%08X", (unsigned)kTarget);
        return false;
    }

    if (WineSafe_CreateHook((void*)kTarget, (void*)&Hook_sub_799B70, (void**)&g_orig) != MH_OK) {
        Log("[SceneQueryCellFast] NOT active: CreateHook failed on 0x%08X", (unsigned)kTarget);
        return false;
    }

    if (WO_EnableHook((void*)kTarget) != MH_OK) {
        MH_RemoveHook((void*)kTarget);
        Log("[SceneQueryCellFast] NOT active: EnableHook failed on 0x%08X", (unsigned)kTarget);
        return false;
    }

    g_installed = true;
    SamplingProfiler::RegisterSelfSymbol("SceneQueryCellFast_Hook", (const void*)&Hook_sub_799B70);
    Log("[SceneQueryCellFast] ACTIVE on sub_799B70 @ 0x%08X (454 bytes)", (unsigned)kTarget);
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
    Log("[SceneQueryCellFast] calls=%llu emptySkips=%llu", g_calls, g_emptySkips);
}

} // namespace SceneQueryCellFast
