// ============================================================================
// Module: m2_batch_cmp_transparent.cpp
//
// Replaces the transparent M2 batch sort comparator (sub_81EF30, 0x1A7 bytes).
// In profile wow_optimize_2026-09-21_20-10-34.log, sub_81EF30 took 2.01% of
// executing time, sampled at 0x0081EF5B inside its first float comparison.
//
// The client's sub_81EF30 uses x87 fld/fcomp/fnstsw ax sequences to compare
// depths, sorting elements back-to-front or front-to-back. Each fnstsw ax
// transfers the x87 status word to AX, stalling the CPU pipeline and incurring
// FPU/integer domain transfer latency on every comparison in heapsort.
//
// This replacement performs direct scalar comparisons on untouched IEEE floats
// and integer record fields. It does no floating-point arithmetic (zero adds,
// subs, muls, or divs), so its results are exact by construction.
//
// One deliberate difference from the client: where it reads through the
// pointer at +0x28 with no check, this checks for null and skips that
// comparison. The client faults there; this does not.
//
// Verification: Compares results bit for bit against the client function for
// the first 10,000 comparisons and 1 out of every 128 calls thereafter.
// Retires immediately on the first mismatch.
// ============================================================================

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "m2_batch_cmp_transparent.h"
#include "MinHook.h"
#include "config.h"
#include "ab_test.h"

extern "C" void Log(const char* fmt, ...);
bool WowOpt_ClientPatchAllowed(const void* addr);
MH_STATUS WineSafe_CreateHook(void* target, void* detour, void** original);
MH_STATUS WO_EnableHook(void* target);

namespace M2BatchCmpTransparent {

namespace {

constexpr uintptr_t kTarget = 0x0081EF30;
constexpr uintptr_t kCmpOpaque = 0x0081EEA0;

// push ebp / mov ebp, esp / mov eax, [ebp+10h] / mov edx, [eax+3Ch]
const unsigned char kPrologue[8] = {
    0x55, 0x8B, 0xEC, 0x8B, 0x45, 0x10, 0x8B, 0x50
};

typedef int (__cdecl *CmpFn)(int a1, int a2, int a3);
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

__declspec(safebuffers) static inline int FastCmp(int a1, int a2, int a3) {
    const uint8_t* const base = *(const uint8_t* const*)((uintptr_t)a3 + 0x3C);
    const uint8_t* const rec1 = base + (size_t)a1 * 68;
    const uint8_t* const rec2 = base + (size_t)a2 * 68;

    const float f1_a = *(const float*)(rec1 + 16);
    const float f1_b = *(const float*)(rec2 + 16);
    if (f1_b < f1_a) return -1;
    if (f1_b > f1_a) return 1;

    const uint32_t b1 = *(const uint32_t*)(rec1 + 8) & 1;
    const uint32_t b2 = *(const uint32_t*)(rec2 + 8) & 1;
    if (b1 > b2) return -1;
    if (b1 < b2) return 1;

    const int32_t v24_1 = *(const int32_t*)(rec1 + 0x24);
    const int32_t v24_2 = *(const int32_t*)(rec2 + 0x24);
    if (v24_1 < v24_2) return -1;
    if (v24_1 > v24_2) return 1;

    const float f2_a = *(const float*)(rec1 + 20);
    const float f2_b = *(const float*)(rec2 + 20);
    if (f2_b < f2_a) return -1;
    if (f2_b > f2_a) return 1;

    const uint32_t flag = *(const uint32_t*)0x00AF5898 & 0x4000;
    if (flag) {
        const uint32_t t1 = *(const uint32_t*)rec1;
        const uint32_t t2 = *(const uint32_t*)rec2;
        const uint32_t m1 = *(const uint32_t*)(rec1 + 4);
        const uint32_t m2 = *(const uint32_t*)(rec2 + 4);
        if (t1 != t2 || m1 != m2) {
            const uintptr_t p30_1 = *(const uintptr_t*)(rec1 + 0x30);
            const uintptr_t p30_2 = *(const uintptr_t*)(rec2 + 0x30);
            if (p30_1 != 0 && p30_2 != 0) {
                const uint32_t idx34_1 = *(const uint32_t*)(rec1 + 0x34);
                const uint32_t v14 = *(const uint32_t*)(p30_1 + 4 * idx34_1 + 0x2C);
                const uint32_t idx34_2 = *(const uint32_t*)(rec2 + 0x34);
                const uint32_t v28 = *(const uint32_t*)(p30_2 + 4 * idx34_2 + 0x2C);
                if (v14 < v28) return -1;
                if (v14 > v28) return 1;

                const uint32_t idx38_1 = *(const uint32_t*)(rec1 + 0x38);
                const uint32_t v16 = *(const uint32_t*)(p30_1 + 4 * idx38_1 + 0x194);
                const uint32_t idx38_2 = *(const uint32_t*)(rec2 + 0x38);
                const uint32_t v17 = *(const uint32_t*)(p30_2 + 4 * idx38_2 + 0x194);
                if (v16 < v17) return -1;
                if (v16 > v17) return 1;
            }
        }
    }

    const uint32_t m_a = *(const uint32_t*)(rec1 + 4);
    const uint32_t m_b = *(const uint32_t*)(rec2 + 4);
    if (m_a < m_b) return -1;
    if (m_a > m_b) return 1;

    const int32_t type_a = *(const int32_t*)rec1;
    const int32_t type_b = *(const int32_t*)rec2;
    if (type_a < type_b) return -1;
    if (type_a > type_b) return 1;

    if (type_a <= 2) {
        const uintptr_t p28_a = *(const uintptr_t*)(rec1 + 0x28);
        const uintptr_t p28_b = *(const uintptr_t*)(rec2 + 0x28);
        if (p28_a && p28_b) {
            const uint16_t w_a = *(const uint16_t*)(p28_a + 0x0C);
            const uint16_t w_b = *(const uint16_t*)(p28_b + 0x0C);
            if (w_a < w_b) return -1;
            if (w_a > w_b) return 1;
        }
    }

    if (!flag) return ((CmpFn)kCmpOpaque)(a1, a2, a3);

    const uintptr_t p30_a = *(const uintptr_t*)(rec1 + 0x30);
    const uintptr_t p30_b = *(const uintptr_t*)(rec2 + 0x30);
    if (!p30_a || !p30_b) return ((CmpFn)kCmpOpaque)(a1, a2, a3);

    const uint32_t idx34_a = *(const uint32_t*)(rec1 + 0x34);
    const uint32_t v23 = *(const uint32_t*)(p30_a + 4 * idx34_a + 0x2C);
    const uint32_t idx34_b = *(const uint32_t*)(rec2 + 0x34);
    const uint32_t v25 = *(const uint32_t*)(p30_b + 4 * idx34_b + 0x2C);
    if (v23 < v25) return -1;
    if (v23 > v25) return 1;

    const uint32_t idx38_a = *(const uint32_t*)(rec1 + 0x38);
    const uint32_t v26 = *(const uint32_t*)(p30_a + 4 * idx38_a + 0x194);
    const uint32_t idx38_b = *(const uint32_t*)(rec2 + 0x38);
    const uint32_t v27 = *(const uint32_t*)(p30_b + 4 * idx38_b + 0x194);
    if (v26 < v27) return -1;
    if (v26 > v27) return 1;

    return ((CmpFn)kCmpOpaque)(a1, a2, a3);
}

__declspec(safebuffers) static int __cdecl Hook_sub_81EF30(int a1, int a2, int a3) {
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
            Log("[M2BatchCmpTransparent] Disagreement at a1=%d a2=%d: fast=%d client=%d. Hook retired.",
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
    if (!Config::g_settings.OptM2BatchCmpTransparent) return true;

    if (memcmp((const void*)kTarget, kPrologue, sizeof(kPrologue)) != 0) {
        Log("[M2BatchCmpTransparent] NOT active: prologue mismatch at 0x%08X", (unsigned)kTarget);
        return false;
    }

    if (!WowOpt_ClientPatchAllowed((const void*)kTarget)) {
        Log("[M2BatchCmpTransparent] NOT active: client patches not allowed");
        return false;
    }

    if (WineSafe_CreateHook((void*)kTarget, (void*)&Hook_sub_81EF30, (void**)&g_orig) != MH_OK) {
        Log("[M2BatchCmpTransparent] NOT active: CreateHook failed on 0x%08X", (unsigned)kTarget);
        return false;
    }

    if (WO_EnableHook((void*)kTarget) != MH_OK) {
        MH_RemoveHook((void*)kTarget);
        Log("[M2BatchCmpTransparent] NOT active: EnableHook failed on 0x%08X", (unsigned)kTarget);
        return false;
    }

    g_installed = true;
    g_abSubject = AbTest::IsSubject("M2BatchCmpTransparent", &g_abSubject);
    Log("[M2BatchCmpTransparent] Hook installed on sub_81EF30 (0x1A7 bytes)");
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
             "[M2BatchCmpTransparent] calls=%llu (armed=%llu, verified=%llu, control=%llu) mismatches=%u%s",
             g_calls, g_armedCalls, g_verifiedCalls, g_controlCalls, g_mismatches,
             g_dead ? " [RETIRED]" : "");
    Log("%s", buf);
}

}  // namespace M2BatchCmpTransparent
