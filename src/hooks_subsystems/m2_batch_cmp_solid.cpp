// ============================================================================
// Module: m2_batch_cmp_solid.cpp
//
// Replaces the solid/opaque M2 batch sort comparator (sub_81EAD0, 0x233 bytes).
// In profile wow_optimize_2026-09-22_23-08-49.log and 17-28-51.log, sub_81EAD0
// accounted for 1.48% - 3.02% of executing time, clustered at 0x0081EAE6.
//
// The client's sub_81EAD0 unconditionally dereferences two 3-hop pointer chains
// (rec -> +4 -> +2Ch -> +150h) in its function prologue before evaluating any
// fields. In heapsort (sub_83DCF0), the vast majority of opaque batches (type 0)
// differ on early scalar fields (material/shader ID, submesh indices, etc.) and
// return immediately without ever needing the pointers at +150h.
//
// This replacement performs all early scalar comparisons first. The expensive
// 3-hop pointer chain is only traversed if all early fields compare equal or
// if the batch type is non-zero.
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

#include "m2_batch_cmp_solid.h"
#include "MinHook.h"
#include "version.h"
#include "config.h"
#include "ab_test.h"
#include "sampling_profiler.h"

extern "C" void Log(const char* fmt, ...);

namespace M2BatchCmpSolid {

namespace {

constexpr uintptr_t kTarget = 0x0081EAD0;

// push ebp / mov ebp, esp / sub esp, 14h / push ebx / mov ebx, [ebp+0Ch]
const unsigned char kPrologue[8] = {
    0x55, 0x8B, 0xEC, 0x83, 0xEC, 0x14, 0x53, 0x8B
};

typedef int (__cdecl *CmpFn)(const void* a1, const void* a2);
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

__declspec(safebuffers) static inline int FastCmp(const void* a1_ptr, const void* a2_ptr) {
    const uint8_t* const r1 = (const uint8_t*)a1_ptr;
    const uint8_t* const r2 = (const uint8_t*)a2_ptr;

    const int32_t type1 = *(const int32_t*)r1;
    if (type1 == 0) {
        const uintptr_t batch1 = *(const uintptr_t*)(r1 + 0x28);
        const uintptr_t batch2 = *(const uintptr_t*)(r2 + 0x28);

        const uint16_t v7 = *(const uint16_t*)(batch1 + 0x0C);
        const uint16_t v8 = *(const uint16_t*)(batch2 + 0x0C);
        if (v7 < v8) return -1;
        if (v7 > v8) return 1;

        const uintptr_t p30_1 = *(const uintptr_t*)(r1 + 0x30);
        const uintptr_t p30_2 = *(const uintptr_t*)(r2 + 0x30);
        if (p30_1 != 0 && p30_2 != 0) {
            const uint32_t idx34_1 = *(const uint32_t*)(r1 + 0x34);
            const uint32_t idx34_2 = *(const uint32_t*)(r2 + 0x34);
            const uint32_t v12 = *(const uint32_t*)(p30_1 + 4 * idx34_1 + 0x2C);
            const uint32_t v14 = *(const uint32_t*)(p30_2 + 4 * idx34_2 + 0x2C);
            if (v12 < v14) return -1;
            if (v12 > v14) return 1;

            const uint32_t idx38_1 = *(const uint32_t*)(r1 + 0x38);
            const uint32_t idx38_2 = *(const uint32_t*)(r2 + 0x38);
            const uint32_t v13 = *(const uint32_t*)(p30_1 + 4 * idx38_1 + 0x194);
            const uint32_t v15 = *(const uint32_t*)(p30_2 + 4 * idx38_2 + 0x194);
            if (v13 < v15) return -1;
            if (v13 > v15) return 1;
        }

        const uintptr_t m1 = *(const uintptr_t*)(r1 + 0x04);
        const uintptr_t m2 = *(const uintptr_t*)(r2 + 0x04);
        const uint32_t mdata1 = *(const uint32_t*)(m1 + 0x2C);
        const uint32_t mdata2 = *(const uint32_t*)(m2 + 0x2C);
        if (mdata1 < mdata2) return -1;
        if (mdata1 > mdata2) return 1;

        const int32_t f1 = *(const int32_t*)(r1 + 0x08) & 4;
        const int32_t f2 = *(const int32_t*)(r2 + 0x08) & 4;
        if (f1 < f2) return -1;
        if (f1 > f2) return 1;

        if (m1 < m2) return -1;
        if (m1 > m2) return 1;

        const uintptr_t sub1 = *(const uintptr_t*)(r1 + 0x2C);
        const uintptr_t sub2 = *(const uintptr_t*)(r2 + 0x2C);
        const uint16_t w1 = *(const uint16_t*)(sub1 + 0x0E);
        const uint16_t w2 = *(const uint16_t*)(sub2 + 0x0E);
        if (w1 < w2) return -1;
        if (w1 > w2) return 1;
    }

    // loc_81EBCE: common path
    const uintptr_t m1 = *(const uintptr_t*)(r1 + 0x04);
    const uintptr_t m2 = *(const uintptr_t*)(r2 + 0x04);
    const uintptr_t pModel1 = *(const uintptr_t*)(*(const uintptr_t*)(m1 + 0x2C) + 0x150);
    const uintptr_t pModel2 = *(const uintptr_t*)(*(const uintptr_t*)(m2 + 0x2C) + 0x150);

    const uintptr_t batch1 = *(const uintptr_t*)(r1 + 0x28);
    const uintptr_t batch2 = *(const uintptr_t*)(r2 + 0x28);

    const uint32_t idxA = *(const uint16_t*)(batch1 + 0x0A);
    const uint32_t idxB = *(const uint16_t*)(batch2 + 0x0A);

    const uintptr_t tbl74_1 = *(const uintptr_t*)(pModel1 + 0x74);
    const uintptr_t tbl74_2 = *(const uintptr_t*)(pModel2 + 0x74);
    const uint8_t* const entry1 = (const uint8_t*)(tbl74_1 + idxA * 4);
    const uint8_t* const entry2 = (const uint8_t*)(tbl74_2 + idxB * 4);

    const uint16_t ew1 = *(const uint16_t*)(entry1 + 2);
    const uint16_t ew2 = *(const uint16_t*)(entry2 + 2);
    if (ew1 < ew2) return -1;
    if (ew1 > ew2) return 1;

    const uint32_t b1 = *(const uint8_t*)entry1 & 0x1F;
    const uint32_t b2 = *(const uint8_t*)entry2 & 0x1F;
    if (b1 < b2) return -1;
    if (b1 > b2) return 1;

    const uint16_t cnt1 = *(const uint16_t*)(batch1 + 0x0E);
    const uint16_t cnt2 = *(const uint16_t*)(batch2 + 0x0E);
    const uint32_t count = cnt1 < cnt2 ? cnt1 : cnt2;
    if (count > 0) {
        const uintptr_t list84_1 = *(const uintptr_t*)(pModel1 + 0x84);
        const uintptr_t list84_2 = *(const uintptr_t*)(pModel2 + 0x84);
        const uint32_t off1 = *(const uint16_t*)(batch1 + 0x10);
        const uint32_t off2 = *(const uint16_t*)(batch2 + 0x10);
        const uint16_t* const arr1 = (const uint16_t*)(list84_1 + off1 * 2);
        const uint16_t* const arr2 = (const uint16_t*)(list84_2 + off2 * 2);
        const uint32_t lim1 = *(const uint32_t*)(pModel1 + 0x50);
        const uint32_t lim2 = *(const uint32_t*)(pModel2 + 0x50);
        const uintptr_t texTable1 = *(const uintptr_t*)(m1 + 0xA4);
        const uintptr_t texTable2 = *(const uintptr_t*)(m2 + 0xA4);

        for (uint32_t i = 0; i < count; ++i) {
            const uint32_t t1 = arr1[i];
            const uint32_t val1 = (t1 < lim1) ? *(const uint32_t*)(texTable1 + t1 * 4) : 0;
            const uint32_t t2 = arr2[i];
            const uint32_t val2 = (t2 < lim2) ? *(const uint32_t*)(texTable2 + t2 * 4) : 0;
            const int32_t diff = (int32_t)(val1 - val2) >> 2;
            if (diff < 0) return -1;
            if (diff > 0) return 1;
        }
    }

    if (cnt1 < cnt2) return -1;
    if (cnt1 > cnt2) return 1;

    if (batch1 < batch2) return -1;
    if (batch1 > batch2) return 1;
    return 0;
}

__declspec(safebuffers) static int __cdecl Hook_sub_81EAD0(const void* a1, const void* a2) {
    if (g_dead) return g_orig(a1, a2);

    ++g_calls;
    if (g_abSubject && AbTest::StandAside()) {
        ++g_controlCalls;
        return g_orig(a1, a2);
    }

    const bool verify = (g_verifiedCalls < 10000) || ((g_calls & 127) == 0);
    if (verify) {
        ++g_verifiedCalls;
        const int expected = g_orig(a1, a2);
        const int actual = FastCmp(a1, a2);
        if (actual != expected) {
            ++g_mismatches;
            g_dead = true;
            Log("[M2BatchCmpSolid] Disagreement at a1=%p a2=%p: fast=%d client=%d. Hook retired.",
                a1, a2, actual, expected);
            return expected;
        }
        return expected;
    }

    ++g_armedCalls;
    return FastCmp(a1, a2);
}

}  // namespace

bool Init() {
    if (!Config::g_settings.OptM2BatchCmpSolid) return true;

    if (memcmp((const void*)kTarget, kPrologue, sizeof(kPrologue)) != 0) {
        Log("[M2BatchCmpSolid] NOT active: prologue mismatch at 0x%08X", (unsigned)kTarget);
        return false;
    }

    if (!WowOpt_ClientPatchAllowed((const void*)kTarget)) {
        Log("[M2BatchCmpSolid] NOT active: client patches not allowed");
        return false;
    }

    if (WineSafe_CreateHook((void*)kTarget, (void*)&Hook_sub_81EAD0, (void**)&g_orig) != MH_OK) {
        Log("[M2BatchCmpSolid] NOT active: CreateHook failed on 0x%08X", (unsigned)kTarget);
        return false;
    }

    if (WO_EnableHook((void*)kTarget) != MH_OK) {
        MH_RemoveHook((void*)kTarget);
        Log("[M2BatchCmpSolid] NOT active: EnableHook failed on 0x%08X", (unsigned)kTarget);
        return false;
    }

    g_installed = true;
    g_abSubject = AbTest::IsSubject("M2BatchCmpSolid", &g_abSubject);
    SamplingProfiler::RegisterSelfSymbol("M2BatchCmpSolid_Hook", (const void*)&Hook_sub_81EAD0);
    Log("[M2BatchCmpSolid] Hook installed on sub_81EAD0 (0x233 bytes)");
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
             "[M2BatchCmpSolid] calls=%llu (armed=%llu, verified=%llu, control=%llu) mismatches=%u%s",
             g_calls, g_armedCalls, g_verifiedCalls, g_controlCalls, g_mismatches,
             g_dead ? " [RETIRED]" : "");
    Log("%s", buf);
}

}  // namespace M2BatchCmpSolid
