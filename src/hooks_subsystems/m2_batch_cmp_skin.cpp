// ============================================================================
// Module: m2_batch_cmp_skin.cpp
//
// Replaces the M2 model skin/submesh batch sort comparator (sub_824B70, 234 bytes).
// In profile wow_optimize_2026-09-22_15-28-41.log, sub_824B70 was sampled 3,864
// times (1.40% of executing time) at 0x00824C1F.
//
// Invoked as the ordering comparator in quicksort / intro-sort (sub_82E840,
// sub_82DC10, sub_82BC20, sub_82EB10) when sorting model submesh batches.
//
// The client's sub_824B70 unconditionally spills registers and dereferences
// multiple table pointers redundantly, repeatedly re-evaluating model pointers
// even when sorting elements of the exact same model.
//
// This replacement fast-paths the common case where both items belong to the
// same model (m2_1 == m2_2), eliminating redundant pointer chases, and evaluates
// tie-breakers directly without unnecessary stack spills or /GS cookies.
//
// Verification: Dual-runs against the client function for the first 10,000
// comparisons and 1 out of every 128 calls thereafter. Retires immediately on
// the first mismatch.
// ============================================================================

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <atomic>

#include "m2_batch_cmp_skin.h"
#include "MinHook.h"
#include "version.h"
#include "config.h"
#include "ab_test.h"
#include "sampling_profiler.h"

extern "C" void Log(const char* fmt, ...);

namespace M2BatchCmpSkin {

namespace {

constexpr uintptr_t kTarget = 0x00824B70;

// push ebp / mov ebp, esp / mov ecx, [ebp+arg_0] / mov eax, [ecx] / mov edx, [eax+2D0h] / test edx, edx
const unsigned char kPrologue[16] = {
    0x55, 0x8B, 0xEC, 0x8B, 0x4D, 0x08, 0x8B, 0x01,
    0x8B, 0x90, 0xD0, 0x02, 0x00, 0x00, 0x85, 0xD2
};

typedef bool (__stdcall *CmpSkinFn)(const uint32_t* a1, const uint32_t* a2);
CmpSkinFn g_orig = nullptr;

bool g_installed = false;
bool g_dead = false;
bool g_abSubject = false;

// Statistics
std::atomic<uint64_t> g_calls{0};
std::atomic<uint64_t> g_armedCalls{0};
std::atomic<uint64_t> g_verifiedCalls{0};
std::atomic<uint64_t> g_controlCalls{0};
std::atomic<uint32_t> g_mismatches{0};

__declspec(safebuffers) static inline bool FastCmp(const uint32_t* a1, const uint32_t* a2) {
    const uint32_t m2_1 = a1[0];
    const uint32_t idx_1 = a1[1];
    const uint32_t m2_2 = a2[0];
    const uint32_t idx_2 = a2[1];

    if (m2_1 == m2_2) {
        const uint32_t aux = *(const uint32_t*)(m2_1 + 0x2D0);
        uint16_t key1, key2;
        if (aux) {
            const uint32_t table = *(const uint32_t*)aux;
            const uint32_t base = *(const uint32_t*)(aux + 8);
            const uint16_t subIdx1 = *(const uint16_t*)(table + idx_1 * 24 + 4);
            const uint16_t subIdx2 = *(const uint16_t*)(table + idx_2 * 24 + 4);
            key1 = *(const uint16_t*)(base + (size_t)subIdx1 * 48 + 0x10);
            key2 = *(const uint16_t*)(base + (size_t)subIdx2 * 48 + 0x10);
        } else {
            const uint32_t model = *(const uint32_t*)(m2_1 + 0x2C);
            const uint32_t f170 = *(const uint32_t*)(model + 0x170);
            const uint32_t table = *(const uint32_t*)(f170 + 0x28);
            const uint32_t base = *(const uint32_t*)(model + 0x18C);
            const uint16_t subIdx1 = *(const uint16_t*)(table + idx_1 * 24 + 4);
            const uint16_t subIdx2 = *(const uint16_t*)(table + idx_2 * 24 + 4);
            key1 = *(const uint16_t*)(base + (size_t)subIdx1 * 48 + 0x10);
            key2 = *(const uint16_t*)(base + (size_t)subIdx2 * 48 + 0x10);
        }
        if (key1 != key2) {
            return key1 < key2;
        }
        return idx_1 < idx_2;
    }

    uint16_t key1;
    const uint32_t aux1 = *(const uint32_t*)(m2_1 + 0x2D0);
    if (aux1) {
        const uint32_t table1 = *(const uint32_t*)aux1;
        const uint16_t subIdx1 = *(const uint16_t*)(table1 + idx_1 * 24 + 4);
        const uint32_t base1 = *(const uint32_t*)(aux1 + 8);
        key1 = *(const uint16_t*)(base1 + (size_t)subIdx1 * 48 + 0x10);
    } else {
        const uint32_t model1 = *(const uint32_t*)(m2_1 + 0x2C);
        const uint32_t f170_1 = *(const uint32_t*)(model1 + 0x170);
        const uint32_t table1 = *(const uint32_t*)(f170_1 + 0x28);
        const uint16_t subIdx1 = *(const uint16_t*)(table1 + idx_1 * 24 + 4);
        const uint32_t base1 = *(const uint32_t*)(model1 + 0x18C);
        key1 = *(const uint16_t*)(base1 + (size_t)subIdx1 * 48 + 0x10);
    }

    uint16_t key2;
    const uint32_t aux2 = *(const uint32_t*)(m2_2 + 0x2D0);
    if (aux2) {
        const uint32_t table2 = *(const uint32_t*)aux2;
        const uint16_t subIdx2 = *(const uint16_t*)(table2 + idx_2 * 24 + 4);
        const uint32_t base2 = *(const uint32_t*)(aux2 + 8);
        key2 = *(const uint16_t*)(base2 + (size_t)subIdx2 * 48 + 0x10);
    } else {
        const uint32_t model2 = *(const uint32_t*)(m2_2 + 0x2C);
        const uint32_t f170_2 = *(const uint32_t*)(model2 + 0x170);
        const uint32_t table2 = *(const uint32_t*)(f170_2 + 0x28);
        const uint16_t subIdx2 = *(const uint16_t*)(table2 + idx_2 * 24 + 4);
        const uint32_t base2 = *(const uint32_t*)(model2 + 0x18C);
        key2 = *(const uint16_t*)(base2 + (size_t)subIdx2 * 48 + 0x10);
    }

    if (key1 != key2) {
        return key1 < key2;
    }

    if (aux1 != aux2) {
        return aux1 < aux2;
    }

    const uint32_t model1 = *(const uint32_t*)(m2_1 + 0x2C);
    const uint32_t model2 = *(const uint32_t*)(m2_2 + 0x2C);
    if (model1 != model2) {
        return model1 < model2;
    }

    return idx_1 < idx_2;
}

__declspec(safebuffers) static bool __stdcall Hook_sub_824B70(const uint32_t* a1, const uint32_t* a2) {
    if (g_dead) return g_orig(a1, a2);

    const uint64_t callCount = ++g_calls;
    if (g_abSubject && AbTest::StandAside()) {
        ++g_controlCalls;
        return g_orig(a1, a2);
    }

    const bool verify = (callCount <= 10000) || ((callCount & 127) == 0);
    if (verify) {
        ++g_verifiedCalls;
        const bool expected = g_orig(a1, a2);
        const bool actual = FastCmp(a1, a2);
        if (actual != expected) {
            ++g_mismatches;
            g_dead = true;
            Log("[M2BatchCmpSkin] Disagreement at a1=%p a2=%p: fast=%d client=%d. Hook retired.",
                a1, a2, (int)actual, (int)expected);
            return expected;
        }
        return expected;
    }

    ++g_armedCalls;
    return FastCmp(a1, a2);
}

}  // namespace

bool Init() {
    if (!Config::g_settings.OptM2BatchCmpSkin) return true;

    if (memcmp((const void*)kTarget, kPrologue, sizeof(kPrologue)) != 0) {
        Log("[M2BatchCmpSkin] NOT active: prologue mismatch at 0x%08X", (unsigned)kTarget);
        return false;
    }

    if (!WowOpt_ClientPatchAllowed((const void*)kTarget)) {
        Log("[M2BatchCmpSkin] NOT active: client patches not allowed");
        return false;
    }

    if (WineSafe_CreateHook((void*)kTarget, (void*)&Hook_sub_824B70, (void**)&g_orig) != MH_OK) {
        Log("[M2BatchCmpSkin] NOT active: CreateHook failed on 0x%08X", (unsigned)kTarget);
        return false;
    }

    if (WO_EnableHook((void*)kTarget) != MH_OK) {
        MH_RemoveHook((void*)kTarget);
        Log("[M2BatchCmpSkin] NOT active: EnableHook failed on 0x%08X", (unsigned)kTarget);
        return false;
    }

    g_installed = true;
    g_abSubject = AbTest::IsSubject("M2BatchCmpSkin", &g_abSubject);
    SamplingProfiler::RegisterSelfSymbol("M2BatchCmpSkin_Hook", (const void*)&Hook_sub_824B70);
    Log("[M2BatchCmpSkin] Hook installed on sub_824B70 (234 bytes)");
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
             "[M2BatchCmpSkin] calls=%llu (armed=%llu, verified=%llu, control=%llu) mismatches=%u%s",
             g_calls.load(std::memory_order_relaxed),
             g_armedCalls.load(std::memory_order_relaxed),
             g_verifiedCalls.load(std::memory_order_relaxed),
             g_controlCalls.load(std::memory_order_relaxed),
             g_mismatches.load(std::memory_order_relaxed),
             g_dead ? " [RETIRED]" : "");
    Log("%s", buf);
}

}  // namespace M2BatchCmpSkin
