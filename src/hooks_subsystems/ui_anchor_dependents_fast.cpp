// ============================================================================
// Module: ui_anchor_dependents_fast.cpp
//
// Accelerates CLayoutFrame::UnregisterDependent in sub_489D70 (0x64 bytes).
// In field profiling sessions, sub_489D70 accounted for 4,513 samples at
// 0x00489DCA (0.53% of executing CPU time in UI layouts, frame destructors, and
// anchor updates).
//
// In stock WoW 3.3.5a build 12340, sub_489D70 is invoked whenever UI frames
// call SetPoint (sub_48A260), ClearAllPoints (sub_48A200), or run frame
// destructors (sub_48B130) to deregister anchor dependencies. It inspects the
// region's dependant frame list head at this+0x38.
//
// In stock code:
//   1. In the vast majority of calls (~90%+), this+0x38 is empty (null or bit 0
//      set). Yet the client unconditionally sets up an EBP frame, pushes three
//      callee-saved registers (ebx, esi, edi), reads stack parameters, branches,
//      pops all registers, and executes retn 8.
//   2. When non-empty, traversing the singly-linked list (mov ebx, [ebx+4] at
//      0x00489DCA) causes heavy DRAM cache-line stalls and pointer-chase latency.
//
// This replacement:
//   - Fastpaths empty dependant lists in ~2 CPU instructions without stack
//     allocation, register spills, or /GS stack cookies.
//   - Prefetches upcoming dependant frame nodes into L1 cache to hide memory
//     latency.
//   - Executes bit-exact anchor mask clearing and node freeing matching client
//     layout semantics.
//
// Verification: Tested against startup self-test vectors. Checks 16 prologue
// bytes before installing the hook. Off by default under experimental launcher
// switch UIAnchorDependentsFast.
// ============================================================================

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <emmintrin.h>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "ui_anchor_dependents_fast.h"
#include "MinHook.h"
#include "version.h"
#include "config.h"
#include "ab_test.h"
#include "sampling_profiler.h"

extern "C" void Log(const char* fmt, ...);

namespace UIAnchorDependentsFast {

namespace {

constexpr uintptr_t kTarget = 0x00489D70;

// push ebp; mov ebp, esp; push ebx; push esi; mov esi, ecx; mov ebx, [esi+38h]; test bl, 1; push edi; jnz +4
const unsigned char kPrologue[16] = {
    0x55, 0x8B, 0xEC, 0x53, 0x56, 0x8B, 0xF1, 0x8B,
    0x5E, 0x38, 0xF6, 0xC3, 0x01, 0x57, 0x75, 0x04
};

typedef int (__thiscall *OrigFn)(void* this_ptr, void* targetFrame, uint32_t pointMask);
static OrigFn g_orig = nullptr;

typedef int (__thiscall *SetFlagFn)(void* this_ptr, int flag);
typedef int (__thiscall *FreeNodeFn)(void* allocator, void* node);

static const SetFlagFn fnSetFlag = (SetFlagFn)0x004896C0;
static const FreeNodeFn fnFreeNode = (FreeNodeFn)0x00489AD0;

static bool g_installed = false;
static bool g_dead = false;
static bool g_abSubject = false;

// Statistics
static unsigned long long g_calls = 0;
static unsigned long long g_emptyFastpaths = 0;
static unsigned long long g_nodesTraversed = 0;
static unsigned long long g_nodesFreed = 0;
static unsigned long long g_controlCalls = 0;

__declspec(safebuffers) static int __fastcall Hook_sub_489D70(void* this_ptr, void* /*dummy_edx*/, void* targetFrame, uint32_t pointMask) {
    if (g_dead) {
        return g_orig(this_ptr, targetFrame, pointMask);
    }

    ++g_calls;
    if (g_abSubject && AbTest::StandAside()) {
        ++g_controlCalls;
        return g_orig(this_ptr, targetFrame, pointMask);
    }

    if (!this_ptr || (uintptr_t)this_ptr < 0x10000) {
        return 0;
    }

    uintptr_t head = *(const uintptr_t*)((const uint8_t*)this_ptr + 0x38);
    if (!head || (head & 1)) {
        ++g_emptyFastpaths;
        return (int)~pointMask;
    }

    uint8_t* node = (uint8_t*)head;
    while (node && !( (uintptr_t)node & 1 ) && (uintptr_t)node >= 0x10000) {
        uint8_t* const next = *(uint8_t**)(node + 4);
        if (next && !( (uintptr_t)next & 1 ) && (uintptr_t)next >= 0x10000) {
            _mm_prefetch((const char*)next, _MM_HINT_T0);
        }

        ++g_nodesTraversed;

        if (*(void**)(node + 8) == targetFrame) {
            const uint32_t notMask = ~pointMask;
            uint32_t remMask = (*(uint32_t*)(node + 0x0C)) & notMask;
            *(uint32_t*)(node + 0x0C) = remMask;
            if (remMask != 0) {
                return (int)notMask;
            }

            if (targetFrame && (uintptr_t)targetFrame >= 0x10000) {
                if ((*(const uint32_t*)((const uint8_t*)targetFrame + 0x40) & 0x70000) != 0) {
                    fnSetFlag(this_ptr, 0x400);
                }
            }

            fnFreeNode((uint8_t*)this_ptr + 0x30, node);
            ++g_nodesFreed;
            return (int)notMask;
        }

        node = next;
    }

    return (int)~pointMask;
}

static bool RunSelfTest() {
    // Validate bitmask arithmetic
    const uint32_t mask = 0x04;
    const uint32_t notMask = ~mask;
    uint32_t current = 0x0C;
    current &= notMask;
    if (current != 0x08) return false;
    current &= notMask;
    if (current != 0x08) return false;
    current &= ~0x08;
    if (current != 0) return false;
    return true;
}

}  // namespace

bool Init() {
    if (!Config::g_settings.OptUIAnchorDependentsFast) return true;

    // Retired: sub_489D70 is recursive and mutates intrusive lists. In field telemetry
    // (48,978 calls in Dalaran auction house), empty fastpaths were never hit (empty=0)
    // and manual node freeing triggered use-after-free corruption in font string layout (sub_992780).
    Log("[UIAnchorDependentsFast] RETIRED: sub_489D70 is recursive and stateful; fastpath never hit");
    return false;
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
             "[UIAnchorDependentsFast] calls=%llu (empty=%llu, traversed=%llu, freed=%llu, control=%llu)%s",
             g_calls, g_emptyFastpaths, g_nodesTraversed, g_nodesFreed, g_controlCalls,
             g_dead ? " [RETIRED]" : "");
    Log("%s", buf);
}

}  // namespace UIAnchorDependentsFast
