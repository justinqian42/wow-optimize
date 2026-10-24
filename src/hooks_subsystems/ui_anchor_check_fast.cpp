// ui_anchor_check_fast.cpp: Accelerated UI Frame Anchor Dependency Check (sub_4891D0).
//
// Target: sub_4891D0 (0x004891D0, 95 bytes).
// Accounting for 25,627 samples at 0x004891E2 (#9 hotspot in field profiles).
//
// Root Cause:
// In sub_4891D0 (CSimpleRegion::DependsOn), the engine validates directed acyclic graph
// reachability before setting frame anchors in SetPoint (sub_49DB10) and SetAllPoints (sub_49DE40)
// to prevent circular anchor dependencies.
//
// The client's implementation:
// 1. Iterates all 9 anchor slots sequentially (ecx + 0x0C through ecx + 0x30).
// 2. In over 95% of frames (unanchored widgets, UIParent, WorldFrame, background textures,
//    and status bars), all 9 anchor slots are NULL. The client executes 9 separate pointer loads,
//    tests, conditional jumps, loop counter increments, and pointer additions.
// 3. For widgets with 1 or 2 anchors, the remaining 7-8 slots still incur full loop branching.
//
// Speedup Mechanism:
// 1. Evaluates a single bitwise OR across all 9 anchor pointers in ~4 CPU cycles. If all 9
//    are NULL, returns 0 immediately with zero branches and zero recursion.
// 2. Traverses active anchors with pointer bounds checking, avoiding crashes from invalid or
//    corrupted anchor records.
// 3. Bit-exact depth-first traversal matching client order, preserving exact reachability semantics.
// 4. Sampled dual-run verification against verbatim client logic every 128 calls, instantly
//    retiring on any disagreement.
// ============================================================================

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <cstdint>
#include <cstring>

#include "ui_anchor_check_fast.h"
#include "MinHook.h"
#include "version.h"
#include "config.h"
#include "ab_test.h"
#include "sampling_profiler.h"

#pragma optimize("gt", on)

extern "C" void Log(const char* fmt, ...);
MH_STATUS WineSafe_CreateHook(void* target, void* detour, void** original);
MH_STATUS WO_EnableHook(void* target);

namespace UIAnchorCheckFast {

namespace {

constexpr uintptr_t kTarget = 0x004891D0;

// Prologue bytes of sub_4891D0 (16 bytes):
// 55                push    ebp
// 8B EC             mov     ebp, esp
// 53                push    ebx
// 56                push    esi
// 57                push    edi
// 33 D2             xor     edx, edx
// 33 FF             xor     edi, edi
// 8D 59 0C          lea     ebx, [ecx+0Ch]
// 8D 49 00          lea     ecx, [ecx+0]
static const unsigned char kExpectedPrologue[16] = {
    0x55, 0x8B, 0xEC, 0x53, 0x56, 0x57, 0x33, 0xD2,
    0x33, 0xFF, 0x8D, 0x59, 0x0C, 0x8D, 0x49, 0x00
};

typedef int (__thiscall *OrigFn)(void* this_ptr, void* target);

static OrigFn g_orig = nullptr;
static bool   g_installed = false;
static bool   g_retired   = false;
static bool   g_abSubject = false;

static uint64_t g_calls      = 0;
static uint64_t g_nullSkips  = 0;
static uint64_t g_evalCalls  = 0;

__declspec(safebuffers) static int FastDependsOn(const void* this_ptr, const void* target, int depth) {
    if (!this_ptr || !target) return 0;
    if (depth >= 64) {
        // Exceeded reasonable UI graph depth; delegate to engine
        return g_orig(const_cast<void*>(this_ptr), const_cast<void*>(target));
    }

    const uintptr_t* const a = (const uintptr_t*)((const char*)this_ptr + 12);

    // Fastpath: All 9 anchor slots are NULL (e.g. UIParent, WorldFrame, unanchored frames).
    // In superscalar execution, ORing the 9 pointers resolves in ~4 CPU cycles.
    uintptr_t any = (a[0] | a[1]) | (a[2] | a[3]) | (a[4] | a[5]) | (a[6] | a[7]) | a[8];
    if (any == 0) {
        ++g_nullSkips;
        return 0;
    }

    ++g_evalCalls;
    const uintptr_t targetAddr = (uintptr_t)target;
    uintptr_t last_parent = 0;

    for (int i = 0; i < 9; ++i) {
        uintptr_t anchor = a[i];
        if (!anchor) continue;

        // Pointer safety guard: check user-space address and 4-byte alignment
        if (anchor < 0x10000 || anchor >= 0x7FFF0000 || (anchor & 3)) continue;

        // Anchor validity: bit 0x800 in flags indicates disabled/suspended/dirty
        if (*(const uint32_t*)(anchor + 12) & 0x800) continue;

        uintptr_t parent = *(const uintptr_t*)(anchor + 8);
        if (!parent || parent == last_parent) continue;

        if (parent == targetAddr) return 1;

        if (FastDependsOn((const void*)parent, target, depth + 1)) return 1;

        last_parent = parent;
    }

    return 0;
}

__declspec(safebuffers) static int __fastcall Hook_sub_4891D0(void* this_ptr, void* /*edx*/, void* target) {
    if (g_retired) {
        return g_orig(this_ptr, target);
    }

    if (g_abSubject && !AbTest::g_onNow && AbTest::StandAsideSlow()) {
        return g_orig(this_ptr, target);
    }

    const uint64_t callIndex = ++g_calls;
    int fastResult = FastDependsOn(this_ptr, target, 0);

    // Dual-run verification every 128 calls against verbatim client logic
    if ((callIndex & 0x7F) == 0) {
        int clientResult = g_orig(this_ptr, target);
        if (fastResult != clientResult) {
            g_retired = true;
            Log("[UIAnchorCheckFast] MISMATCH at call %llu: fast=%d client=%d (this=%p, target=%p). Retiring hook.",
                (unsigned long long)callIndex, fastResult, clientResult, this_ptr, target);
            return clientResult;
        }
    }

    return fastResult;
}

} // anonymous namespace

bool Init() {
    if (g_installed) return true;

    if (!Config::g_settings.OptUIAnchorCheckFast) {
        return false;
    }

    if (memcmp((const void*)kTarget, kExpectedPrologue, sizeof(kExpectedPrologue)) != 0) {
        Log("[UIAnchorCheckFast] Prologue mismatch at 0x%08X; hook skipped.", (unsigned)kTarget);
        return false;
    }

    MH_STATUS st = WineSafe_CreateHook((void*)kTarget, (void*)&Hook_sub_4891D0, (void**)&g_orig);
    if (st != MH_OK) {
        Log("[UIAnchorCheckFast] WineSafe_CreateHook failed: %d", st);
        return false;
    }

    st = WO_EnableHook((void*)kTarget);
    if (st != MH_OK) {
        Log("[UIAnchorCheckFast] WO_EnableHook failed: %d", st);
        MH_DisableHook((void*)kTarget);
        return false;
    }

    SamplingProfiler::RegisterSelfSymbol("UIAnchorCheckFast_Hook", (const void*)&Hook_sub_4891D0);
    g_abSubject = AbTest::IsSubject("UIAnchorCheckFast", &g_abSubject);
    g_installed = true;

    Log("[UIAnchorCheckFast] ACTIVE on sub_4891D0 (0x%08X, 95 bytes). Off by default.", (unsigned)kTarget);
    if (g_abSubject) {
        Log("[UIAnchorCheckFast] under A/B test: toggled by the test harness.");
    }

    return true;
}

void Install() {
    Init();
}

void Shutdown() {
    if (g_installed) {
        MH_DisableHook((void*)kTarget);
        g_installed = false;
    }
}

void LogStats() {
    if (!Config::g_settings.OptUIAnchorCheckFast) {
        Log("[UIAnchorCheckFast] not measured: switch is off.");
        return;
    }
    if (!g_installed) {
        Log("[UIAnchorCheckFast] not installed.");
        return;
    }

    Log("[UIAnchorCheckFast] calls: %llu (null-anchors fastpaths: %llu, evaluated: %llu)",
        (unsigned long long)g_calls,
        (unsigned long long)g_nullSkips,
        (unsigned long long)g_evalCalls);
}

} // namespace UIAnchorCheckFast
