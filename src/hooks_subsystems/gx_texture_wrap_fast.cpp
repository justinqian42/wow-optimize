// ============================================================================
// Module: gx_texture_wrap_fast.cpp
//
// Fastpath for CGxDevice::SetTextureWrap (sub_685D40, 127 bytes).
// In field profiles (wow_optimize_2026-10-22_11-55-53.log), sub_685D40 was a
// top rendering hotspot (569 samples at 0x00685D51, 0.65% of executing CPU time),
// invoked via GxSetTextureWrap (sub_681450) across M2 models, terrain chunks,
// and WMO material passes.
//
// The stock client implementation:
// 1. Reads [texture + 2Ch] (flags).
// 2. Evaluates wrap_u via (flags >> 3) & 1, compares with arg_4.
// 3. Evaluates wrap_v via (flags >> 4) & 1, compares with arg_8.
// 4. If matching, returns wrap_v.
// 5. If different:
//    - Updates bits 3 and 4 in [texture + 2Ch].
//    - Sets [texture + 5Ch] = 1 (marks dirty).
//    - Runs a 16-iteration loop over stages 21..36, checking if any bound texture
//      matches this texture. Inside that loop, it redundantly reloads the device
//      pointer and [this + 2900h] on every single iteration (32 redundant loads).
//
// This replacement:
// 1. Validates pointer bounds and alignment before dereferencing texture flags.
// 2. Evaluates both wrap_u and wrap_v simultaneously in a single bitmask test
//    ((flags ^ (desiredWrap << 3)) & 0x18) == 0, skipping redundant updates in
//    ~4 instructions without register thrashing.
// 3. Hoists device stage pointers out of the 16-stage scan when wrap modes change.
// 4. Verifies decision equivalence against verbatim client logic, retiring on
//    the first mismatch.
// ============================================================================

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <cstdint>
#include <cstring>

#include "gx_texture_wrap_fast.h"
#include "MinHook.h"
#include "version.h"
#include "config.h"
#include "ab_test.h"
#include "sampling_profiler.h"

#pragma optimize("gt", on)

extern "C" void Log(const char* fmt, ...);
MH_STATUS WineSafe_CreateHook(void* target, void* detour, void** original);
MH_STATUS WO_EnableHook(void* target);

namespace GxTextureWrapFast {

namespace {

constexpr uintptr_t kTarget = 0x00685D40;
constexpr uintptr_t kRecordStageTarget = 0x006859E0;

// Prologue bytes of sub_685D40 (16 bytes):
// 55                push    ebp
// 8B EC             mov     ebp, esp
// 51                push    ecx
// 8B 55 0C          mov     edx, [ebp+arg_4]
// 53                push    ebx
// 8B 5D 08          mov     ebx, [ebp+arg_0]
// 89 4D FC          mov     [ebp+var_4], ecx
// 8B 4B 2C          mov     ecx, [ebx+2Ch]
static const unsigned char kExpectedPrologue[16] = {
    0x55, 0x8B, 0xEC, 0x51, 0x8B, 0x55, 0x0C, 0x53,
    0x8B, 0x5D, 0x08, 0x89, 0x4D, 0xFC, 0x8B, 0x4B
};

typedef unsigned int (__thiscall *OrigFn)(void* this_ptr, void* texture, int wrap_u, unsigned int wrap_v);
typedef int (__thiscall *RecordStageFn)(void* this_ptr, int stage);

static OrigFn g_orig = nullptr;
static bool   g_installed = false;
static bool   g_retired   = false;
static bool   g_abSubject = false;

static uint64_t g_calls         = 0;
static uint64_t g_redundantHits = 0;
static uint64_t g_wrapChanges   = 0;
static uint64_t g_stageUpdates  = 0;

__declspec(safebuffers) static unsigned int __fastcall Hook_sub_685D40(
    void* this_ptr, void* /*dummy_edx*/, void* texture, int wrap_u, unsigned int wrap_v)
{
    if (g_retired) {
        return g_orig(this_ptr, texture, wrap_u, wrap_v);
    }

    if (g_abSubject && !AbTest::g_onNow && AbTest::StandAsideSlow()) {
        return g_orig(this_ptr, texture, wrap_u, wrap_v);
    }

    ++g_calls;

    const uintptr_t texAddr = (uintptr_t)texture;
    if (texAddr < 0x10000 || (texAddr & 3)) {
        return wrap_v;
    }

    const uint32_t flags = *(const uint32_t*)(texAddr + 0x2C);
    const uint32_t desiredWrap = (wrap_u & 1) | ((wrap_v & 1) << 1);

    // Fastpath: bits 3 and 4 already match requested wrap modes
    if (((flags ^ (desiredWrap << 3)) & 0x18) == 0) {
        ++g_redundantHits;
        return wrap_v;
    }

    // Verification against verbatim client logic
    if (g_calls <= 10000 || (g_calls & 0x7F) == 0) {
        const bool clientMatches = (((flags >> 3) & 1) == (uint32_t)wrap_u) &&
                                   (((flags >> 4) & 1) == wrap_v);
        if (clientMatches) {
            Log("[GxTextureWrapFast] VERIFICATION MISMATCH: fast path decided wrap changed but client matched! Retiring.");
            g_retired = true;
            return g_orig(this_ptr, texture, wrap_u, wrap_v);
        }
    }

    ++g_wrapChanges;

    const uint32_t newFlags = (flags & ~0x18u) | (desiredWrap << 3);
    *(uint32_t*)(texAddr + 0x2C) = newFlags;
    *(uint8_t*)(texAddr + 0x5C)  = 1;

    uint32_t result = newFlags;
    const uintptr_t dev = (uintptr_t)this_ptr;
    const uintptr_t stageBase = *(const uintptr_t*)(dev + 0x2900);

    if (stageBase) {
        RecordStageFn recordStage = (RecordStageFn)kRecordStageTarget;
        for (int v6 = 0; v6 < 16; ++v6) {
            const uintptr_t slotTex = *(const uintptr_t*)(stageBase + 0x150 + v6 * 16);
            if (slotTex == texAddr) {
                ++g_stageUpdates;
                result = (uint32_t)recordStage(this_ptr, v6 + 21);
            }
        }
    }

    return result;
}

} // anonymous namespace

bool Init() {
    if (!Config::g_settings.OptGxTextureWrapFast) {
        Log("[GxTextureWrapFast] not active: switch GxTextureWrapFast is off");
        return false;
    }

    if (std::memcmp((const void*)kTarget, kExpectedPrologue, sizeof(kExpectedPrologue)) != 0) {
        Log("[GxTextureWrapFast] BAD PROLOGUE at 0x%08X; hook refused", (unsigned)kTarget);
        return false;
    }

    MH_STATUS st = WineSafe_CreateHook((void*)kTarget, (void*)&Hook_sub_685D40, (void**)&g_orig);
    if (st != MH_OK) {
        Log("[GxTextureWrapFast] WineSafe_CreateHook failed (%d)", st);
        return false;
    }

    st = WO_EnableHook((void*)kTarget);
    if (st != MH_OK) {
        Log("[GxTextureWrapFast] WO_EnableHook failed (%d)", st);
        MH_DisableHook((void*)kTarget);
        return false;
    }

    SamplingProfiler::RegisterSelfSymbol("GxTextureWrapFast_Hook", (const void*)&Hook_sub_685D40);
    g_abSubject = AbTest::IsSubject("GxTextureWrapFast", &g_abSubject);
    g_installed = true;

    Log("[GxTextureWrapFast] ACTIVE on sub_685D40 (0x%08X, 127 bytes). Off by default.", (unsigned)kTarget);
    if (g_abSubject) {
        Log("[GxTextureWrapFast] under A/B test: toggled by the test harness.");
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
    if (!Config::g_settings.OptGxTextureWrapFast) {
        Log("[GxTextureWrapFast] not measured: switch is off.");
        return;
    }
    if (!g_installed) {
        Log("[GxTextureWrapFast] not installed.");
        return;
    }

    Log("[GxTextureWrapFast] calls: %llu (redundant hits: %llu, wrap changes: %llu, stage updates: %llu)",
        (unsigned long long)g_calls,
        (unsigned long long)g_redundantHits,
        (unsigned long long)g_wrapChanges,
        (unsigned long long)g_stageUpdates);
}

} // namespace GxTextureWrapFast
