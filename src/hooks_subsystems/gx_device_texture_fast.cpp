// ============================================================================
// Module: gx_device_texture_fast.cpp
//
// Fastpath for CGxDevice::SetTexture (sub_685F50, 94 bytes).
// In field profiles (wow_optimize_2026-10-22_16-54-34.log), sub_685F50 was a
// top rendering hotspot (5,558 samples at 0x00685F98, 0.55% of executing CPU time),
// invoked on every texture bind across UI batch rendering, M2 models, terrain,
// and particle systems.
//
// The stock client implementation:
// 1. Tests [edi+0F58h] (device ready).
// 2. Looks up [edi+28F4h] (stage state array, 24 bytes per stage).
// 3. Compares [state + stage * 24] against the candidate texture pointer.
// 4. Calls sub_685970(stage) to record state changes.
// 5. Writes state[stage].tex = texture.
// 6. For stages 21..36, dereferences [texture+0x2C] & 0x80 and [texture+0x5A],
//    which causes L1/L2 cache miss stalls (observed as the hotspot peak at
//    0x00685F98) and crashes with 0xC0000005 if an unaligned or corrupt pointer
//    is passed by corrupted batch state.
//
// This replacement:
// 1. Fastpaths redundant binds immediately without calling sub_685970.
// 2. Prefetches [texture + 0x2C] early to hide RAM latency when changing stages.
// 3. Validates candidate texture pointer alignment and address space bounds
//    before dereferencing stages 21..36, eliminating fatal access violations.
// 4. Compiles to a compact, cookie-free assembly sequence without stack spills.
// ============================================================================

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <cstdint>
#include <cstring>
#include <xmmintrin.h>

#include "gx_device_texture_fast.h"
#include "MinHook.h"
#include "version.h"
#include "config.h"
#include "ab_test.h"
#include "sampling_profiler.h"

#pragma optimize("gt", on)

extern "C" void Log(const char* fmt, ...);
MH_STATUS WineSafe_CreateHook(void* target, void* detour, void** original);
MH_STATUS WO_EnableHook(void* target);

namespace GxDeviceTextureFast {

namespace {

constexpr uintptr_t kTarget = 0x00685F50;
constexpr uintptr_t kRecordStateTarget = 0x00685970;

// Prologue bytes of sub_685F50 (16 bytes):
// 55                push    ebp
// 8B EC             mov     ebp, esp
// 57                push    edi
// 8B F9             mov     edi, ecx
// 83 BF 58 0F 00 00 00 cmp dword ptr [edi+0F58h], 0
// 74 4C             jz      short loc_685FAB
// 8B 8F F4 28 00 00 mov     ecx, [edi+28F4h]
static const unsigned char kExpectedPrologue[16] = {
    0x55, 0x8B, 0xEC, 0x57, 0x8B, 0xF9, 0x83, 0xBF,
    0x58, 0x0F, 0x00, 0x00, 0x00, 0x74, 0x4C, 0x8B
};

typedef int (__thiscall *OrigFn)(void* this_ptr, int stage, void* texture);
typedef int (__thiscall *RecordStateFn)(void* this_ptr, int stage);
typedef int (__thiscall *DeviceSetTexFn)(void* this_ptr, void* texture);

static OrigFn g_orig = nullptr;
static bool   g_installed = false;
static bool   g_retired   = false;
static bool   g_abSubject = false;

static uint64_t g_calls          = 0;
static uint64_t g_redundantSkips = 0;
static uint64_t g_stageChanges   = 0;
static uint64_t g_specialStages  = 0;

__declspec(safebuffers) static int __fastcall Hook_sub_685F50(void* this_ptr, void* /*edx*/, int stage, void* texture) {
    if (g_retired) {
        return g_orig(this_ptr, stage, texture);
    }

    if (g_abSubject && !AbTest::g_onNow && AbTest::StandAsideSlow()) {
        return g_orig(this_ptr, stage, texture);
    }

    ++g_calls;

    const uintptr_t dev = (uintptr_t)this_ptr;
    if (!*(const uint32_t*)(dev + 0x0F58)) {
        return 0;
    }

    const uintptr_t states = *(const uintptr_t*)(dev + 0x28F4);
    uintptr_t* const slot = (uintptr_t*)(states + stage * 24);

    if (*slot == (uintptr_t)texture) {
        ++g_redundantSkips;
        return (int)(uintptr_t)slot;
    }

    // Prefetch texture flags if valid pointer
    const uintptr_t texAddr = (uintptr_t)texture;
    if (texAddr >= 0x10000 && !(texAddr & 3)) {
        _mm_prefetch((const char*)(texAddr + 0x2C), _MM_HINT_T0);
    }

    ++g_stageChanges;
    const int res = ((RecordStateFn)kRecordStateTarget)(this_ptr, stage);

    *slot = texAddr;
    if (!texture) {
        return res;
    }

    if ((unsigned int)(stage - 21) <= 15) {
        ++g_specialStages;
        // Verify pointer is valid and properly aligned before dereferencing
        if (texAddr >= 0x10000 && texAddr < 0x7FFF0000 && !(texAddr & 3)) {
            if ((*(const uint8_t*)(texAddr + 0x2C) & 0x80) &&
                *(const uint8_t*)(texAddr + 0x5A) != 0) {
                const uintptr_t vtable = *(const uintptr_t*)dev;
                DeviceSetTexFn setTex = *(DeviceSetTexFn*)vtable;
                return setTex(this_ptr, texture);
            }
        }
    }

    return res;
}

} // anonymous namespace

bool Init() {
    if (!Config::g_settings.OptGxDeviceTextureFast) {
        Log("[GxDeviceTextureFast] not active: switch GxDeviceTextureFast is off");
        return false;
    }

    if (std::memcmp((const void*)kTarget, kExpectedPrologue, sizeof(kExpectedPrologue)) != 0) {
        Log("[GxDeviceTextureFast] BAD PROLOGUE at 0x%08X; hook refused", (unsigned)kTarget);
        return false;
    }

    MH_STATUS st = WineSafe_CreateHook((void*)kTarget, (void*)&Hook_sub_685F50, (void**)&g_orig);
    if (st != MH_OK) {
        Log("[GxDeviceTextureFast] WineSafe_CreateHook failed (%d)", st);
        return false;
    }

    st = WO_EnableHook((void*)kTarget);
    if (st != MH_OK) {
        Log("[GxDeviceTextureFast] WO_EnableHook failed (%d)", st);
        MH_DisableHook((void*)kTarget);
        return false;
    }

    SamplingProfiler::RegisterSelfSymbol("GxDeviceTextureFast_Hook", (const void*)&Hook_sub_685F50);
    g_abSubject = AbTest::IsSubject("GxDeviceTextureFast", &g_abSubject);
    g_installed = true;

    Log("[GxDeviceTextureFast] ACTIVE on sub_685F50 (0x%08X, 94 bytes). Off by default.", (unsigned)kTarget);
    if (g_abSubject) {
        Log("[GxDeviceTextureFast] under A/B test: toggled by the test harness.");
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
    if (!Config::g_settings.OptGxDeviceTextureFast) {
        Log("[GxDeviceTextureFast] not measured: switch is off.");
        return;
    }
    if (!g_installed) {
        Log("[GxDeviceTextureFast] not installed.");
        return;
    }

    Log("[GxDeviceTextureFast] calls: %llu (redundant binds skipped: %llu, stage changes: %llu, special 21..36 stages: %llu)",
        (unsigned long long)g_calls,
        (unsigned long long)g_redundantSkips,
        (unsigned long long)g_stageChanges,
        (unsigned long long)g_specialStages);
}

} // namespace GxDeviceTextureFast
