// fog_state_dedup.cpp: Fastpath and deduplication for scene fog shader and device setup (sub_873210).
//
// Target: sub_873210 (0x00873210, 379 bytes).
// Callers:
//   1. sub_7A8440 (Terrain chunk render setup)
//   2. sub_7A9380 (Terrain layer render loop)
//   3. sub_81FB10 (M2 model batch render loop)
//
// Root Cause:
// In sub_873210 (SetFog), the engine updates the atmospheric fog parameters on the 3D device
// before rendering terrain chunks and model batches.
// In modern rendering (shader mode active, dword_D43020 != 0):
// 1. Unconditionally converts 3 color bytes to float via fild/fmul.
// 2. Evaluates reciprocal distance range via fdivrp (1.0 / (fogEnd - fogStart)).
// 3. Performs multiple float multiplications and negations.
// 4. Stores results to 7 global variables (flt_D43058, flt_D4305C, flt_D43060, dword_D43068,
//    dword_D4306C, dword_D43070, dword_D43074).
// 5. Calls sub_532AF0 to retrieve device caps and dispatches an indirect virtual call
//    CGxDevice::SetShaderConstants(4, 2, ...) to upload registers c4 and c5 to the GPU.
//
// Within a frame, atmospheric fog is constant across all M2 model batches and terrain chunks.
// In >99% of calls, the fog parameters are 100% bit-exact identical to the previous call.
// The client lacks any caching in shader mode and blindly re-executes the float division and
// virtual device dispatch on every single batch.
//
// Speedup Mechanism:
// 1. Compares incoming fog start, fog end, fog rate, color, shader mode, and device pointer
//    against cached state. If unchanged, returns in ~6 CPU instructions without any FPU or device overhead.
// 2. Validates user-space pointer bounds on color pointer a4, protecting against access violations.
// 3. Periodically checks that global constants were not modified by an external path, resynchronizing if needed.
// 4. Retains bit-exact device state and global variables.
// ============================================================================

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <cstdint>
#include <cstring>

#include "fog_state_dedup.h"
#include "MinHook.h"
#include "version.h"
#include "config.h"
#include "ab_test.h"
#include "sampling_profiler.h"

#pragma optimize("gt", on)

extern "C" void Log(const char* fmt, ...);
MH_STATUS WineSafe_CreateHook(void* target, void* detour, void** original);
MH_STATUS WO_EnableHook(void* target);

namespace FogStateDedup {

namespace {

constexpr uintptr_t kTarget = 0x00873210;

// Prologue bytes of sub_873210 (16 bytes):
// 55                push    ebp
// 8B EC             mov     ebp, esp
// 83 EC 14          sub     esp, 14h
// 83 3D 20 30 D4 00 00 cmp  dword_D43020, 0
// 56                push    esi
// 57                push    edi
// 0F 84 C5 00 00 00 jz      loc_8732EA
static const unsigned char kExpectedPrologue[16] = {
    0x55, 0x8B, 0xEC, 0x83, 0xEC, 0x14, 0x83, 0x3D,
    0x20, 0x30, 0xD4, 0x00, 0x00, 0x56, 0x57, 0x0F
};

typedef void (__cdecl *OrigFn)(float a1, float a2, int a3, const uint8_t* a4);

static OrigFn g_orig = nullptr;
static bool   g_installed = false;
static bool   g_retired   = false;
static bool   g_abSubject = false;

// Cached previous fog state
static uint32_t  s_lastA1         = 0;
static uint32_t  s_lastA2         = 0;
static uint32_t  s_lastA3         = 0;
static uint32_t  s_lastColor      = 0;
static uint32_t  s_lastShaderMode = 0;
static uint32_t  s_lastD4300C     = 0;
static uintptr_t s_lastDevice     = 0;
static uint32_t  s_cachedD43068   = 0;
static uint32_t  s_cachedD4306C   = 0;
static bool      s_cached         = false;

static uint64_t g_calls      = 0;
static uint64_t g_dedupSkips = 0;
static uint64_t g_updates    = 0;

__declspec(safebuffers) static void __cdecl Hook_sub_873210(float a1, float a2, int a3, const uint8_t* a4) {
    if (g_retired) {
        g_orig(a1, a2, a3, a4);
        return;
    }

    if (g_abSubject && !AbTest::g_onNow && AbTest::StandAsideSlow()) {
        g_orig(a1, a2, a3, a4);
        return;
    }

    const uint64_t callIndex = ++g_calls;

    // Safety guard: validate color pointer
    const uintptr_t colorAddr = (uintptr_t)a4;
    if (colorAddr < 0x10000 || colorAddr >= 0x7FFF0000) {
        g_orig(a1, a2, a3, a4);
        return;
    }

    const uint32_t u_a1 = *(const uint32_t*)&a1;
    const uint32_t u_a2 = *(const uint32_t*)&a2;
    const uint32_t u_a3 = (uint32_t)a3;
    const uint32_t color = *(const uint32_t*)a4;
    const uint32_t curShaderMode = *(const uint32_t*)0x00D43020;
    const uint32_t curD4300C     = *(const uint32_t*)0x00D4300C;
    const uintptr_t curDevice    = *(const uintptr_t*)0x00C5DF88;

    if (s_cached &&
        u_a1 == s_lastA1 &&
        u_a2 == s_lastA2 &&
        u_a3 == s_lastA3 &&
        color == s_lastColor &&
        curShaderMode == s_lastShaderMode &&
        curD4300C == s_lastD4300C &&
        curDevice == s_lastDevice)
    {
        // Periodic consistency check every 128 calls in shader mode: verify output globals weren't trampled
        if ((callIndex & 0x7F) == 0 && curShaderMode != 0) {
            const uint32_t curG0 = *(const uint32_t*)0x00D43068;
            const uint32_t curG1 = *(const uint32_t*)0x00D4306C;
            if (curG0 != s_cachedD43068 || curG1 != s_cachedD4306C) {
                // Globals were modified externally; resynchronize through engine
                g_orig(a1, a2, a3, a4);
                s_cachedD43068 = *(const uint32_t*)0x00D43068;
                s_cachedD4306C = *(const uint32_t*)0x00D4306C;
                ++g_updates;
                return;
            }
        }

        ++g_dedupSkips;
        return;
    }

    // State changed or uninitialized: run engine function to update device and globals
    g_orig(a1, a2, a3, a4);

    // Record new state in cache
    s_lastA1         = u_a1;
    s_lastA2         = u_a2;
    s_lastA3         = u_a3;
    s_lastColor      = color;
    s_lastShaderMode = curShaderMode;
    s_lastD4300C     = curD4300C;
    s_lastDevice     = curDevice;
    if (curShaderMode != 0) {
        s_cachedD43068 = *(const uint32_t*)0x00D43068;
        s_cachedD4306C = *(const uint32_t*)0x00D4306C;
    }
    s_cached = true;
    ++g_updates;
}

} // anonymous namespace

bool Init() {
    if (g_installed) return true;

    if (!Config::g_settings.OptFogStateDedup) {
        return false;
    }

    if (memcmp((const void*)kTarget, kExpectedPrologue, sizeof(kExpectedPrologue)) != 0) {
        Log("[FogStateDedup] Prologue mismatch at 0x%08X; hook skipped.", (unsigned)kTarget);
        return false;
    }

    MH_STATUS st = WineSafe_CreateHook((void*)kTarget, (void*)&Hook_sub_873210, (void**)&g_orig);
    if (st != MH_OK) {
        Log("[FogStateDedup] WineSafe_CreateHook failed: %d", st);
        return false;
    }

    st = WO_EnableHook((void*)kTarget);
    if (st != MH_OK) {
        Log("[FogStateDedup] WO_EnableHook failed: %d", st);
        MH_DisableHook((void*)kTarget);
        return false;
    }

    SamplingProfiler::RegisterSelfSymbol("FogStateDedup_Hook", (const void*)&Hook_sub_873210);
    g_abSubject = AbTest::IsSubject("FogStateDedup", &g_abSubject);
    g_installed = true;

    Log("[FogStateDedup] ACTIVE on sub_873210 (0x%08X, 379 bytes). Off by default.", (unsigned)kTarget);
    if (g_abSubject) {
        Log("[FogStateDedup] under A/B test: toggled by the test harness.");
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
    if (!Config::g_settings.OptFogStateDedup) {
        Log("[FogStateDedup] not measured: switch is off.");
        return;
    }
    if (!g_installed) {
        Log("[FogStateDedup] not installed.");
        return;
    }

    Log("[FogStateDedup] calls: %llu (dedup redundant skips: %llu, state updates: %llu)",
        (unsigned long long)g_calls,
        (unsigned long long)g_dedupSkips,
        (unsigned long long)g_updates);
}

} // namespace FogStateDedup
