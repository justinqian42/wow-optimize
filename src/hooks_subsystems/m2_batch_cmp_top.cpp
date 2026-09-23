// ============================================================================
// Module: m2_batch_cmp_top.cpp
//
// Replaces the master M2 batch sort comparator (sub_81F0E0, 227 bytes / 0xE3).
// In profile logs (wow_optimize_2026-09-22_17-28-51.log), sub_81F0E0 took ~1.5%
// of executing time with 15,561 samples at 0x0081F107.
//
// Dispatched by heapsort sub_83DCF0 at 0x0081FAE2 during M2 batch list construction
// in M2_DrawBatchBuilder.
//
// The client's sub_81F0E0:
//   1. Compares priority at [record+0x40] (stride 68 bytes).
//   2. For equal priorities, checks if either batch is type 4 (particle / transparent).
//      If different types, orders by type (signed).
//      If both type 4, compares [p+0xD0] (signed), inlines sub_81CA80 bitfield extraction
//      on [p+0xD4], and compares [p+0x128] pointers via sub_47BF20 ((a - b) >> 2).
//   3. For other batch types, delegates to sub_81EF30 (M2BatchCmpTransparent).
//
// This replacement provides an inlined fast path, zero /GS security cookies,
// zero SEH frames, and inlines the sub_81CA80 bitfield logic and pointer difference.
//
// Verification:
//   Dual-runs against client sub_81F0E0 for the first 10,000 comparisons and
//   1 in every 128 calls thereafter; retires immediately on first mismatch.
// ============================================================================

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "m2_batch_cmp_top.h"
#include "MinHook.h"
#include "version.h"
#include "config.h"
#include "ab_test.h"
#include "sampling_profiler.h"

extern "C" void Log(const char* fmt, ...);

namespace M2BatchCmpTop {

namespace {

constexpr uintptr_t kTarget = 0x0081F0E0;
constexpr uintptr_t kCmpTransparent = 0x0081EF30;

// push ebp / mov ebp, esp / mov eax, [ebp+10h] / mov eax, [eax+3Ch]
const unsigned char kPrologue[8] = {
    0x55, 0x8B, 0xEC, 0x8B, 0x45, 0x10, 0x8B, 0x40
};

typedef int (__cdecl *CmpFn)(int a1, int a2, void* a3);
CmpFn g_orig = nullptr;

bool g_installed = false;
bool g_dead = false;
bool g_abSubject = false;

// Statistics
unsigned long long g_calls = 0;
unsigned long long g_armedCalls = 0;
unsigned long long g_verifiedCalls = 0;
unsigned long long g_controlCalls = 0;
unsigned g_mismatches = 0;

__declspec(safebuffers) static inline int FastCmp(int a1, int a2, const void* a3) {
    if (!a3) return 0;
    const uint8_t* const records = *(const uint8_t**)((const char*)a3 + 0x3C);
    if (!records) return 0;

    const uint8_t* const recA = records + a1 * 68;
    const uint8_t* const recB = records + a2 * 68;

    const uint32_t prioA = *(const uint32_t*)(recA + 0x40);
    const uint32_t prioB = *(const uint32_t*)(recB + 0x40);
    if (prioA != prioB) {
        return (prioA > prioB) ? 1 : -1;
    }

    const uint32_t typeA = *(const uint32_t*)(recA + 0x00);
    const uint32_t typeB = *(const uint32_t*)(recB + 0x00);
    if (typeA == 4 || typeB == 4) {
        if (typeA != typeB) {
            return ((int32_t)typeA > (int32_t)typeB) ? -1 : 1;
        }

        // Both types are 4
        const uint8_t* const pA = *(const uint8_t**)(recA + 0x18);
        const uint8_t* const pB = *(const uint8_t**)(recB + 0x18);
        if (!pA || !pB) {
            return g_orig(a1, a2, (void*)a3);
        }

        const int32_t d0_A = *(const int32_t*)(pA + 0xD0);
        const int32_t d0_B = *(const int32_t*)(pB + 0xD0);
        if (d0_A != d0_B) {
            return (d0_A > d0_B) ? 1 : -1;
        }

        const uint32_t d4_A = *(const uint32_t*)(pA + 0xD4);
        const uint32_t d4_B = *(const uint32_t*)(pB + 0xD4);

        // Inlined sub_81CA80 bitfield evaluation:
        // uint32_t res = (cl & 1) ? 4 : 5;
        // if (!(cl & 2)) res |= 2;
        // if (!(cl & 4)) res |= 0x10;
        uint32_t resA = (d4_A & 1) ? 4 : 5;
        if (!(d4_A & 2)) resA |= 2;
        if (!(d4_A & 4)) resA |= 0x10;

        uint32_t resB = (d4_B & 1) ? 4 : 5;
        if (!(d4_B & 2)) resB |= 2;
        if (!(d4_B & 4)) resB |= 0x10;

        if (resA != resB) {
            return (resA > resB) ? 1 : -1;
        }

        const int32_t nameA = *(const int32_t*)(pA + 0x128);
        const int32_t nameB = *(const int32_t*)(pB + 0x128);
        return (nameA - nameB) >> 2;
    }

    return ((CmpFn)kCmpTransparent)(a1, a2, (void*)a3);
}

__declspec(safebuffers) static int __cdecl Hook_sub_81F0E0(int a1, int a2, void* a3) {
    if (g_dead) return g_orig(a1, a2, a3);

    ++g_calls;
    if (g_abSubject && AbTest::StandAside()) {
        ++g_controlCalls;
        return g_orig(a1, a2, a3);
    }

    const bool verify = (g_verifiedCalls < 10000) || ((g_calls & 127) == 0);
    if (verify) {
        ++g_verifiedCalls;
        const int expected = g_orig(a1, a2, a3);
        const int actual = FastCmp(a1, a2, a3);
        if (actual != expected) {
            ++g_mismatches;
            g_dead = true;
            Log("[M2BatchCmpTop] Disagreement at a1=%d a2=%d: fast=%d client=%d. Hook retired.",
                a1, a2, actual, expected);
            return expected;
        }
        return expected;
    }

    ++g_armedCalls;
    return FastCmp(a1, a2, a3);
}

}  // namespace

bool Init() {
    if (!Config::g_settings.OptM2BatchCmpTop) return true;

    if (memcmp((const void*)kTarget, kPrologue, sizeof(kPrologue)) != 0) {
        Log("[M2BatchCmpTop] NOT active: prologue mismatch at 0x%08X", (unsigned)kTarget);
        return false;
    }

    if (!WowOpt_ClientPatchAllowed((const void*)kTarget)) {
        Log("[M2BatchCmpTop] NOT active: client patches not allowed");
        return false;
    }

    if (WineSafe_CreateHook((void*)kTarget, (void*)&Hook_sub_81F0E0, (void**)&g_orig) != MH_OK) {
        Log("[M2BatchCmpTop] NOT active: CreateHook failed on 0x%08X", (unsigned)kTarget);
        return false;
    }

    if (WO_EnableHook((void*)kTarget) != MH_OK) {
        MH_RemoveHook((void*)kTarget);
        Log("[M2BatchCmpTop] NOT active: EnableHook failed on 0x%08X", (unsigned)kTarget);
        return false;
    }

    g_installed = true;
    g_abSubject = AbTest::IsSubject("M2BatchCmpTop", &g_abSubject);
    SamplingProfiler::RegisterSelfSymbol("M2BatchCmpTop_Hook", (const void*)&Hook_sub_81F0E0);
    Log("[M2BatchCmpTop] Hook installed on sub_81F0E0 (227 bytes)");
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
             "[M2BatchCmpTop] calls=%llu (armed=%llu, verified=%llu, control=%llu) mismatches=%u%s",
             g_calls, g_armedCalls, g_verifiedCalls, g_controlCalls, g_mismatches,
             g_dead ? " [RETIRED]" : "");
    Log("%s", buf);
}

}  // namespace M2BatchCmpTop
