// ============================================================================
// Module: ui_anim_update_fast.cpp
//
// Fastpath for CSimpleFrame::OnAnimUpdate (sub_488890, 90 bytes).
// In field profiles (wow_optimize_2026-10-22_16-54-34.log), sub_488890 was the
// #35 overall hotspot (6,573 samples at 0x0048889C, 0.65% of executing time),
// invoked through 33 UI vtables across all buttons, textures, fontstrings, and
// frames on every frame.
//
// The stock client implementation:
// 1. Sets up a stack frame on every frame/widget call, even though >99.9% of
//    widgets have no animation groups (m_animGroupList at +0x98 is null).
// 2. Evaluates five redundant branches checking tagged pointers (bit 0) and
//    null checks before and during loop execution.
// 3. Serializes float parameter passing via x87 fld/fstp on every iteration.
// 4. Repeatedly reloads the list stride pointer from memory inside the loop.
//
// This replacement:
// 1. Tests [this + 0x98] immediately; if null, returns in 3 instructions with
//    zero stack allocation and zero x87 FPU state changes.
// 2. When active, caches the list stride in a CPU register and prefetches next
//    nodes into L1 cache ahead of time.
// 3. Invokes sub_49C760 directly with exact client ABI parameters.
// ============================================================================

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <cstdint>
#include <cstring>
#include <xmmintrin.h>

#include "ui_anim_update_fast.h"
#include "MinHook.h"
#include "version.h"
#include "config.h"
#include "ab_test.h"
#include "sampling_profiler.h"

#pragma optimize("gt", on)

extern "C" void Log(const char* fmt, ...);
MH_STATUS WineSafe_CreateHook(void* target, void* detour, void** original);
MH_STATUS WO_EnableHook(void* target);

namespace UIAnimUpdateFast {

namespace {

constexpr uintptr_t kTarget = 0x00488890;
constexpr uintptr_t kAnimGroupUpdateTarget = 0x0049C760;

// Prologue bytes of sub_488890 (16 bytes):
// 55                push    ebp
// 8B EC             mov     ebp, esp
// 56                push    esi
// 8B F1             mov     esi, ecx
// 8B 8E 98 00 00 00 mov     ecx, [esi+98h]
// 85 C9             test    ecx, ecx
// 74 47             jz      short loc_4888E7
static const unsigned char kExpectedPrologue[16] = {
    0x55, 0x8B, 0xEC, 0x56, 0x8B, 0xF1, 0x8B, 0x8E,
    0x98, 0x00, 0x00, 0x00, 0x85, 0xC9, 0x74, 0x47
};

typedef void (__thiscall *OrigFn)(void* this_ptr, float elapsed);
typedef char (__thiscall *AnimGroupUpdateFn)(void* this_ptr, float elapsed);

static OrigFn g_orig = nullptr;
static bool   g_installed = false;
static bool   g_retired   = false;
static bool   g_abSubject = false;

static uint64_t g_calls       = 0;
static uint64_t g_emptyCalls  = 0;
static uint64_t g_activeNodes = 0;

__declspec(safebuffers) static void __fastcall Hook_sub_488890(void* this_ptr, void* /*edx*/, float elapsed) {
    if (g_retired) {
        g_orig(this_ptr, elapsed);
        return;
    }

    if (g_abSubject && !AbTest::g_onNow && AbTest::StandAsideSlow()) {
        g_orig(this_ptr, elapsed);
        return;
    }

    ++g_calls;

    const uintptr_t list = *(const uintptr_t*)((uintptr_t)this_ptr + 0x98);
    if (!list) {
        ++g_emptyCalls;
        return;
    }

    const uintptr_t head = *(const uintptr_t*)(list + 8);
    if (!head || (head & 1)) {
        ++g_emptyCalls;
        return;
    }

    const uint32_t stride = *(const uint32_t*)list;
    uintptr_t node = head;

    while (node && !(node & 1)) {
        const uintptr_t next = *(const uintptr_t*)(node + stride + 4);
        if (next && !(next & 1)) {
            _mm_prefetch((const char*)next, _MM_HINT_T0);
        }

        ++g_activeNodes;
        ((AnimGroupUpdateFn)kAnimGroupUpdateTarget)((void*)node, elapsed);
        node = next;
    }
}

} // anonymous namespace

bool Init() {
    if (!Config::g_settings.OptUIAnimUpdateFast) {
        Log("[UIAnimUpdateFast] not active: switch UIAnimUpdateFast is off");
        return false;
    }

    if (std::memcmp((const void*)kTarget, kExpectedPrologue, sizeof(kExpectedPrologue)) != 0) {
        Log("[UIAnimUpdateFast] BAD PROLOGUE at 0x%08X; hook refused", (unsigned)kTarget);
        return false;
    }

    MH_STATUS st = WineSafe_CreateHook((void*)kTarget, (void*)&Hook_sub_488890, (void**)&g_orig);
    if (st != MH_OK) {
        Log("[UIAnimUpdateFast] WineSafe_CreateHook failed (%d)", st);
        return false;
    }

    st = WO_EnableHook((void*)kTarget);
    if (st != MH_OK) {
        Log("[UIAnimUpdateFast] WO_EnableHook failed (%d)", st);
        MH_DisableHook((void*)kTarget);
        return false;
    }

    SamplingProfiler::RegisterSelfSymbol("UIAnimUpdateFast_Hook", (const void*)&Hook_sub_488890);
    g_abSubject = AbTest::IsSubject("UIAnimUpdateFast", &g_abSubject);
    g_installed = true;

    Log("[UIAnimUpdateFast] ACTIVE on sub_488890 (0x%08X, 90 bytes). Off by default.", (unsigned)kTarget);
    if (g_abSubject) {
        Log("[UIAnimUpdateFast] under A/B test: toggled by the test harness.");
    }

    return true;
}

void Shutdown() {
    if (g_installed) {
        MH_DisableHook((void*)kTarget);
        g_installed = false;
    }
}

void LogStats() {
    if (!Config::g_settings.OptUIAnimUpdateFast) {
        Log("[UIAnimUpdateFast] not measured: switch is off.");
        return;
    }
    if (!g_installed) {
        Log("[UIAnimUpdateFast] not installed.");
        return;
    }

    Log("[UIAnimUpdateFast] calls: %llu (empty/no-anim: %llu, active anim nodes ticked: %llu)",
        (unsigned long long)g_calls,
        (unsigned long long)g_emptyCalls,
        (unsigned long long)g_activeNodes);
}

} // namespace UIAnimUpdateFast
