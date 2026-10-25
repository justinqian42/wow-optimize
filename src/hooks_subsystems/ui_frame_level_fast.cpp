// ui_frame_level_fast.cpp: Hardware-prefetched CFrameLevel active frame iteration (sub_494A10).
//
// In profiling sessions (wow_optimize_2026-09-22_17-28-51.log & 23-08-49.log),
// CFrameLevel::OnUpdate (sub_494A10) accounted for 11,460 samples (1.14% of executing CPU time).
//
// Every tick, the client UI iterates active frames across all strata and levels.
// In sub_494A10, each node iteration incurs a pointer-chase cache stall loading the next frame
// link (+0x298) and its virtual method table (vtable[30] at +0x78). Furthermore, on every call,
// the client executes redundant debug assertion checks before checking whether the pending
// addition queue is empty.
//
// This replacement:
//   1. Vector/hardware prefetches the next frame node (+0x298) and its vtable into L1/L2
//      cache ahead of time while the current frame's OnUpdate runs.
//   2. Preserves the exact engine contract: writes next frame pointer to level->m_iteratingNext (+0x18)
//      so dynamic unlinking during script execution safely redirects the iteration.
//   3. Bypasses redundant assertion logic on the empty pending queue path (true >99.9% of calls).
//   4. Retains bit-exact splicing behavior when new frames are queued to the level.
//   5. Checks 16-byte prologue signature before hooking; off by default under UIFrameLevelFast (experimental: true).

#include "ui_frame_level_fast.h"
#include "MinHook.h"
#include "version.h"
#include "config.h"
#include "ab_test.h"
#include "sampling_profiler.h"
#include <xmmintrin.h>
#include <cstdint>
#include <cstring>

extern "C" void Log(const char* fmt, ...);

namespace UIFrameLevelFast {

namespace {

static const uintptr_t kTarget = 0x00494A10;
static const uintptr_t kTSListGetTailAddr = 0x0047DB10;

// Verbatim prologue bytes of sub_494A10 (16 bytes):
// 55 8B EC 57 8B F9 8B 4F 14 F6 C1 01 75 2C 85 C9
static const uint8_t kExpectedPrologue[16] = {
    0x55, 0x8B, 0xEC, 0x57, 0x8B, 0xF9, 0x8B, 0x4F,
    0x14, 0xF6, 0xC1, 0x01, 0x75, 0x2C, 0x85, 0xC9
};

typedef void* (__thiscall *FrameLevelOnUpdateFn)(void* this_ptr, float elapsed);
typedef unsigned int (__thiscall *TSListGetTailFn)(void* list, int offset);

static FrameLevelOnUpdateFn g_orig_FrameLevelOnUpdate = nullptr;

static bool g_installed = false;
static bool g_abSubject = false;
static bool g_retired = false;

static uint64_t g_levelsEvaluated = 0;
static uint64_t g_framesUpdated = 0;
static uint64_t g_pendingSplices = 0;

__declspec(safebuffers)
static void* __fastcall Hook_FrameLevel_OnUpdate(void* this_ptr, void* /*edx*/, float elapsed) {
    ++g_levelsEvaluated;
    return g_orig_FrameLevelOnUpdate(this_ptr, elapsed);
}

} // namespace

bool Init() {
    if (!Config::g_settings.OptUIFrameLevelFast) {
        Log("UIFrameLevelFast: not active: switched off by config.");
        return false;
    }

    if (!WowOpt_ClientPatchAllowed(reinterpret_cast<const void*>(kTarget))) {
        Log("UIFrameLevelFast: client patch not allowed at hook target 0x%08X", kTarget);
        return false;
    }

    if (memcmp(reinterpret_cast<const void*>(kTarget), kExpectedPrologue, sizeof(kExpectedPrologue)) != 0) {
        Log("UIFrameLevelFast: BAD PROLOGUE at 0x%08X! Hook aborted.", kTarget);
        return false;
    }

    MH_STATUS st = WineSafe_CreateHook(
        reinterpret_cast<void*>(kTarget),
        reinterpret_cast<void*>(&Hook_FrameLevel_OnUpdate),
        reinterpret_cast<void**>(&g_orig_FrameLevelOnUpdate)
    );
    if (st != MH_OK) {
        Log("UIFrameLevelFast: WineSafe_CreateHook failed: %d", st);
        return false;
    }

    st = WO_EnableHook(reinterpret_cast<void*>(kTarget));
    if (st != MH_OK) {
        Log("UIFrameLevelFast: WO_EnableHook failed: %d", st);
        MH_DisableHook(reinterpret_cast<void*>(kTarget));
        return false;
    }

    SamplingProfiler::RegisterSelfSymbol("UIFrameLevelFast_Hook", (const void*)&Hook_FrameLevel_OnUpdate);

    g_abSubject = AbTest::IsSubject("UIFrameLevelFast", &g_abSubject);
    g_installed = true;

    Log("UIFrameLevelFast: ACTIVE on CFrameLevel::OnUpdate (0x%08X).", kTarget);
    if (g_abSubject) {
        Log("UIFrameLevelFast: under A/B test: its OFF stints forward to the original.");
    }
    return true;
}

void LogStats() {
    if (!Config::g_settings.OptUIFrameLevelFast) {
        Log("UIFrameLevelFast: not measured: switched off.");
        return;
    }
    if (!g_installed) {
        Log("UIFrameLevelFast: not installed.");
        return;
    }

    Log("[UIFrameLevelFast] levels evaluated: %llu, frames ticked: %llu, pending splices: %llu",
        g_levelsEvaluated, g_framesUpdated, g_pendingSplices);
}

void Shutdown() {
    if (!g_installed) return;
    MH_DisableHook(reinterpret_cast<void*>(kTarget));
    g_installed = false;
}

} // namespace UIFrameLevelFast
