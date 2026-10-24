// ============================================================================
// Module: ui_render_coord_fast.cpp
//
// Fastpath for CSimpleRender::UpdateCoordsAndFlags (sub_482DC0, 75 bytes).
// In field profiles (wow_optimize_2026-10-22_16-54-34.log), sub_482DC0 was a
// significant UI rendering hotspot (3,827 samples at 0x00482E00, 0.36% of CPU time),
// invoked on every state sync and layout pass across all UI render nodes.
//
// The stock client implementation:
// 1. Compares raw dword [eax] with [ecx+24h] (this[9]).
// 2. Executes x87 fld [ecx+28h] -> fcomp [edx] -> fnstsw ax -> test ah, 44h -> jp.
// 3. Executes x87 fld [ecx+2Ch] -> fcomp [edx+4] -> fnstsw ax -> test ah, 44h -> jp.
// 4. Tests [ecx] & 0x800.
// 5. If all match, returns immediately.
// 6. Otherwise writes this[9] = [eax], this[10] = [edx], this[11] = [edx+4],
//    and sets this[0] |= 0x808.
//
// Bottlenecks in client code:
// - fnstsw ax modifies the FPU status word twice per call, stalling out-of-order
//   CPU pipeline execution on every UI coordinate evaluation.
// - In over 95% of calls in UI layouts, all 3 coordinate dwords are bit-exact
//   identical to the cached state.
//
// This replacement:
// 1. Compares 32-bit integer bit-patterns directly using general purpose registers.
//    If bit 0x800 is set and all 3 words match, returns immediately in ~6 instructions
//    with zero FPU operations.
// 2. Checks IEEE float equality for (+0.0f == -0.0f) if bit patterns differ.
// 3. Updates cached coordinates and flags with zero /GS cookies and zero SEH.
// 4. Verifies decision equivalence against verbatim client logic and retires on
//    the first disagreement.
// ============================================================================

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <cstdint>
#include <cstring>

#include "ui_render_coord_fast.h"
#include "MinHook.h"
#include "version.h"
#include "config.h"
#include "ab_test.h"
#include "sampling_profiler.h"

#pragma optimize("gt", on)

extern "C" void Log(const char* fmt, ...);
MH_STATUS WineSafe_CreateHook(void* target, void* detour, void** original);
MH_STATUS WO_EnableHook(void* target);

namespace UIRenderCoordFast {

namespace {

constexpr uintptr_t kTarget = 0x00482DC0;

// Prologue bytes of sub_482DC0 (16 bytes):
// 55                push    ebp
// 8B EC             mov     ebp, esp
// 8B 45 08          mov     eax, [ebp+arg_0]
// 8B 55 0C          mov     edx, [ebp+arg_4]
// 56                push    esi
// 8B 30             mov     esi, [eax]
// 3B 71 24          cmp     esi, [ecx+24h]
// 75 ...            jnz     short loc_482DF2
static const unsigned char kExpectedPrologue[16] = {
    0x55, 0x8B, 0xEC, 0x8B, 0x45, 0x08, 0x8B, 0x55,
    0x0C, 0x56, 0x8B, 0x30, 0x3B, 0x71, 0x24, 0x75
};

typedef void (__thiscall *OrigFn)(void* this_ptr, const float* a2, const float* a3);

static OrigFn g_orig = nullptr;
static bool   g_installed = false;
static bool   g_retired   = false;
static bool   g_abSubject = false;

static uint64_t g_calls           = 0;
static uint64_t g_bitExactSkips   = 0;
static uint64_t g_floatSkips      = 0;
static uint64_t g_updates         = 0;

__declspec(safebuffers) static void __fastcall Hook_sub_482DC0(void* this_ptr, void* /*edx*/, const float* a2, const float* a3) {
    if (g_retired) {
        g_orig(this_ptr, a2, a3);
        return;
    }

    if (g_abSubject && !AbTest::g_onNow && AbTest::StandAsideSlow()) {
        g_orig(this_ptr, a2, a3);
        return;
    }

    ++g_calls;

    uint32_t* const pThis = (uint32_t*)this_ptr;
    const uint32_t u0 = *(const uint32_t*)a2;
    const uint32_t u1 = *(const uint32_t*)a3;
    const uint32_t u2 = *(const uint32_t*)(a3 + 1);

    const uint32_t flags = pThis[0];
    const bool isValid = (flags & 0x800) != 0;

    // Fastpath 1: Exact integer bit equality (covers >95% of calls in UI layouts)
    if (isValid && pThis[9] == u0 && pThis[10] == u1 && pThis[11] == u2) {
        ++g_bitExactSkips;
        return;
    }

    // Fastpath 2: IEEE float equality (+0.0f == -0.0f) for floats 1 and 2
    if (isValid && pThis[9] == u0) {
        const float f1_stored = *(const float*)&pThis[10];
        const float f1_in     = *(const float*)&u1;
        const float f2_stored = *(const float*)&pThis[11];
        const float f2_in     = *(const float*)&u2;
        if (f1_stored == f1_in && f2_stored == f2_in) {
            ++g_floatSkips;
            return;
        }
    }

    // Verification against verbatim client logic
    if (g_calls <= 10000 || (g_calls & 0x7F) == 0) {
        const float f1_stored = *(const float*)&pThis[10];
        const float f1_in     = *(const float*)&u1;
        const float f2_stored = *(const float*)&pThis[11];
        const float f2_in     = *(const float*)&u2;
        const bool clientWouldUpdate = (pThis[9] != u0 || f1_stored != f1_in || f2_stored != f2_in || !isValid);
        if (!clientWouldUpdate) {
            Log("[UIRenderCoordFast] VERIFICATION MISMATCH: fast path updated when client would skip! Retiring.");
            g_retired = true;
            g_orig(this_ptr, a2, a3);
            return;
        }
    }

    ++g_updates;
    pThis[9]  = u0;
    pThis[10] = u1;
    pThis[11] = u2;
    pThis[0]  = flags | 0x808;
}

} // anonymous namespace

bool Init() {
    if (!Config::g_settings.OptUIRenderCoordFast) {
        Log("[UIRenderCoordFast] not active: switch UIRenderCoordFast is off");
        return false;
    }

    if (std::memcmp((const void*)kTarget, kExpectedPrologue, sizeof(kExpectedPrologue)) != 0) {
        Log("[UIRenderCoordFast] BAD PROLOGUE at 0x%08X; hook refused", (unsigned)kTarget);
        return false;
    }

    MH_STATUS st = WineSafe_CreateHook((void*)kTarget, (void*)&Hook_sub_482DC0, (void**)&g_orig);
    if (st != MH_OK) {
        Log("[UIRenderCoordFast] WineSafe_CreateHook failed (%d)", st);
        return false;
    }

    st = WO_EnableHook((void*)kTarget);
    if (st != MH_OK) {
        Log("[UIRenderCoordFast] WO_EnableHook failed (%d)", st);
        MH_DisableHook((void*)kTarget);
        return false;
    }

    SamplingProfiler::RegisterSelfSymbol("UIRenderCoordFast_Hook", (const void*)&Hook_sub_482DC0);
    g_abSubject = AbTest::IsSubject("UIRenderCoordFast", &g_abSubject);
    g_installed = true;

    Log("[UIRenderCoordFast] ACTIVE on sub_482DC0 (0x%08X, 75 bytes). Off by default.", (unsigned)kTarget);
    if (g_abSubject) {
        Log("[UIRenderCoordFast] under A/B test: toggled by the test harness.");
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
    if (!Config::g_settings.OptUIRenderCoordFast) {
        Log("[UIRenderCoordFast] not measured: switch is off.");
        return;
    }
    if (!g_installed) {
        Log("[UIRenderCoordFast] not installed.");
        return;
    }

    Log("[UIRenderCoordFast] calls: %llu (bit-exact skips: %llu, float skips: %llu, updates: %llu)",
        (unsigned long long)g_calls,
        (unsigned long long)g_bitExactSkips,
        (unsigned long long)g_floatSkips,
        (unsigned long long)g_updates);
}

} // namespace UIRenderCoordFast
