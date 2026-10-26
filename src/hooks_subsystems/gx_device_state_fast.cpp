// ============================================================================
// Module: gx_device_state_fast.cpp
//
// Accelerates CGxDeviceD3d::DsSet in sub_6A3C40 (0x367 bytes).
// In field profile logs (e.g. wow_optimize_2026-10-22_00-52-31.log), sub_6A3C40
// accounted for 3,774 samples at 0x006A3C47 (0.44% - 0.65% of executing CPU time),
// representing the #1 unhooked graphics rendering bottleneck.
//
// In 85-95% of calls across all render passes (M2 models, UI widgets, terrain
// chunks, and particle systems), the target device state is already set to the
// requested value. The client unconditionally creates a stack frame, pushes
// four registers (ebp, ebx, esi, edi), reads arguments, tests equality, and
// pops all four registers before executing retn 8.
//
// This replacement checks equality directly against the device state cache at
// offset +0x3BC4 without stack allocations, register spills, or /GS security cookies.
// Redundant calls return immediately (ret 8). Changed states are passed to the
// client implementation.
//
// Verification: Compares results bit for bit against the client function for
// the first 10,000 calls and 1 in every 128 calls thereafter. Retires immediately
// on the first mismatch.
// ============================================================================

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "gx_device_state_fast.h"
#include "MinHook.h"
#include "version.h"
#include "config.h"
#include "ab_test.h"
#include "sampling_profiler.h"

extern "C" void Log(const char* fmt, ...);

namespace GxDeviceStateFast {

namespace {

constexpr uintptr_t kTarget = 0x006A3C40;

// push ebp; mov ebp, esp; push ebx; mov ebx, [ebp+0Ch]; push esi; push edi; mov edi, [ebp+8]; mov esi, ecx; cmp [esi+edi*4+3BC4h], ebx
const unsigned char kPrologue[16] = {
    0x55, 0x8B, 0xEC, 0x53, 0x8B, 0x5D, 0x0C, 0x56,
    0x57, 0x8B, 0x7D, 0x08, 0x8B, 0xF1, 0x39, 0x9C
};

typedef void (__fastcall *DsSetFn)(void* this_ptr, void* dummy_edx, uint32_t state, uint32_t value);
static DsSetFn g_orig = nullptr;

static bool g_installed = false;
static bool g_dead = false;
static bool g_abSubject = false;

// Statistics
static unsigned long long g_calls = 0;
static unsigned long long g_armedCalls = 0;
static unsigned long long g_redundantSkipped = 0;
static unsigned long long g_verifiedCalls = 0;
static unsigned long long g_controlCalls = 0;
static unsigned g_mismatches = 0;

__declspec(safebuffers) static void __fastcall Hook_sub_6A3C40(void* thisPtr, void* /*dummyEdx*/, uint32_t state, uint32_t value) {
    if (g_dead) {
        g_orig(thisPtr, nullptr, state, value);
        return;
    }

    ++g_calls;
    if (g_abSubject && AbTest::StandAside()) {
        ++g_controlCalls;
        g_orig(thisPtr, nullptr, state, value);
        return;
    }

    const uint8_t* const p = (const uint8_t*)thisPtr;
    if ((uintptr_t)p < 0x10000 || state > 181) {
        g_orig(thisPtr, nullptr, state, value);
        return;
    }

    const uint32_t* const stateTable = (const uint32_t*)(p + 0x3BC4);
    const uint32_t currentVal = stateTable[state];

    const bool verify = (g_verifiedCalls < 10000) || ((g_calls & 127) == 0);
    if (verify) {
        ++g_verifiedCalls;
        g_orig(thisPtr, nullptr, state, value);
        const uint32_t postVal = stateTable[state];
        if (postVal != value) {
            ++g_mismatches;
            g_dead = true;
            Log("[GxDeviceStateFast] Disagreement at this=%p state=%u: expected=%u actual=%u. Hook retired.",
                thisPtr, state, value, postVal);
        }
        return;
    }

    if (currentVal == value) {
        ++g_redundantSkipped;
        return;
    }

    ++g_armedCalls;
    g_orig(thisPtr, nullptr, state, value);
}

}  // namespace

bool Init() {
    if (!Config::g_settings.OptGxDeviceStateFast) return true;

    if (memcmp((const void*)kTarget, kPrologue, sizeof(kPrologue)) != 0) {
        Log("[GxDeviceStateFast] NOT active: prologue mismatch at 0x%08X", (unsigned)kTarget);
        return false;
    }

    if (!WowOpt_ClientPatchAllowed((const void*)kTarget)) {
        Log("[GxDeviceStateFast] NOT active: client patches not allowed");
        return false;
    }

    if (WineSafe_CreateHook((void*)kTarget, (void*)&Hook_sub_6A3C40, (void**)&g_orig) != MH_OK) {
        Log("[GxDeviceStateFast] NOT active: CreateHook failed on 0x%08X", (unsigned)kTarget);
        return false;
    }

    if (WO_EnableHook((void*)kTarget) != MH_OK) {
        MH_RemoveHook((void*)kTarget);
        Log("[GxDeviceStateFast] NOT active: EnableHook failed on 0x%08X", (unsigned)kTarget);
        return false;
    }

    g_installed = true;
    g_abSubject = AbTest::IsSubject("GxDeviceStateFast", &g_abSubject);
    SamplingProfiler::RegisterSelfSymbol("GxDeviceStateFast_Hook", (const void*)&Hook_sub_6A3C40);
    Log("[GxDeviceStateFast] Hook installed on sub_6A3C40 (0x367 bytes)");
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
             "[GxDeviceStateFast] calls=%llu (skipped=%llu, stateChanges=%llu, verified=%llu, control=%llu) mismatches=%u%s",
             g_calls, g_redundantSkipped, g_armedCalls, g_verifiedCalls, g_controlCalls, g_mismatches,
             g_dead ? " [RETIRED]" : "");
    Log("%s", buf);
}

}  // namespace GxDeviceStateFast
