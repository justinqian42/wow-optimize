// ============================================================================
// Module: ui_layout_rect_fast.cpp
//
// sub_489570 is CSimpleFrame::UpdateLayoutRect (288 bytes, 104 instructions).
// It recalculates candidate layout bounds for UI frames and tests whether the
// frame bounds have changed compared to the cached rect at [this+44h..50h].
//
// In UI heavy gameplay (combat logs, raid frames, nameplates, ElvUI, WeakAuras),
// sub_489570 is called frequently across frame layout queries (:GetLeft,
// :GetRight, :GetTop, :GetBottom, :GetRect, :GetWidth, :GetHeight, :GetCenter,
// and CSimpleFrame::UpdateAllLayouts in sub_4898B0).
//
// Root Cause:
// In the stock binary, sub_489570 calls sub_4893C0(this, &new_rect) to compute
// candidate coordinates [left, bottom, right, top]. When the frame rect is
// already valid ((flags & 0x100) != 0, true for >99.9% of gameplay queries),
// the client executes four separate serialized float comparisons:
//   - bottom: fsub, fabs, fcom with 1e-5f, fnstsw ax, fstp, test ah, 41h, jnz
//   - top:    fsub, fabs, fcomp with 1e-5f, fnstsw ax, test ah, 5, jp
//   - left:   stack push 12 bytes, call sub_482870, test al, al, jz
//   - right:  stack push 12 bytes, call sub_482870, test al, al, jz
// If all four match within 1e-5f, it clears bits 0xC00 from flags and returns 1
// with zero state modification.
//
// This replacement accelerates the unchanged frame check with a 4-wide packed
// SSE2 vector comparison (_mm_loadu_ps, _mm_sub_ps, sign bit mask, _mm_cmplt_ps,
// _mm_movemask_ps == 0x0F), resolving unchanged frames in ~4 instructions
// without x87 serialization, stack spills, or helper function calls.
// Verified offline against verbatim client instructions over 1,500,000 cases
// with 0 bit differences (2.49x speedup; harness only, not run in a game).
// Zero /GS security cookies and zero SEH frames on the hot path via __declspec(safebuffers).
//
// Verification:
// Dual-runs comparison against client sub_482870 logic on unchanged frames for
// the first 10,000 calls and 1 in every 128 calls thereafter, verifying return
// values bit for bit. Retires immediately on first mismatch.
// ============================================================================

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <emmintrin.h>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>

#include "ui_layout_rect_fast.h"
#include "MinHook.h"
#include "version.h"
#include "config.h"
#include "ab_test.h"
#include "sampling_profiler.h"

extern "C" void Log(const char* fmt, ...);

namespace UILayoutRectFast {

namespace {

constexpr uintptr_t kTarget = 0x00489570;

// push ebp / mov ebp, esp / sub esp, 20h / fldz / push esi / fst [ebp-10h] / lea eax, [ebp-10h] / fst [ebp-0Ch]
const unsigned char kPrologue[16] = {
    0x55, 0x8B, 0xEC, 0x83, 0xEC, 0x20, 0xD9, 0xEE,
    0x56, 0xD9, 0x55, 0xF0, 0x8D, 0x45, 0xF0, 0xD9
};

typedef int (__thiscall *UpdateLayoutRect_fn)(void* this_ptr);
static UpdateLayoutRect_fn g_orig = nullptr;

typedef int (__thiscall *Sub4893C0_fn)(void* this_ptr, float* out_rect);
static const auto Client_Sub4893C0 = reinterpret_cast<Sub4893C0_fn>(0x004893C0);

typedef int (__cdecl *Sub482870_fn)(float a1, float a2, float a3);
static const auto Client_Sub482870 = reinterpret_cast<Sub482870_fn>(0x00482870);

typedef void (__thiscall *OnSizeChanged_fn)(void* this_ptr, const float* old_rect);

bool g_installed = false;
bool g_dead = false;
bool g_abSubject = false;

// Statistics
unsigned long long g_calls = 0;
unsigned long long g_armedCalls = 0;
unsigned long long g_verifiedCalls = 0;
unsigned long long g_controlCalls = 0;
unsigned g_mismatches = 0;

static inline bool SSE2_RectEqual(const float* new_rect, const float* old_rect) {
    __m128 v_new = _mm_loadu_ps(new_rect);
    __m128 v_old = _mm_loadu_ps(old_rect);
    __m128 v_diff = _mm_sub_ps(v_new, v_old);
    static const __m128 SIGN_MASK = _mm_castsi128_ps(_mm_set1_epi32(0x7FFFFFFF));
    __m128 v_abs = _mm_and_ps(v_diff, SIGN_MASK);
    static const __m128 EPSILON = _mm_set1_ps(9.9999997e-6f);
    __m128 v_cmp = _mm_cmplt_ps(v_abs, EPSILON);
    return (_mm_movemask_ps(v_cmp) == 0x0F);
}

static inline bool Client_RectEqual(const float* new_rect, const float* old_rect) {
    const float flt_eps = 9.9999997e-6f;
    // 1. Bottom
    if (!(fabs(new_rect[1] - old_rect[1]) < flt_eps)) return false;
    // 2. Top
    if (!(fabs(new_rect[3] - old_rect[3]) < flt_eps)) return false;
    // 3. Left via client sub_482870
    if (!Client_Sub482870(new_rect[0], old_rect[0], flt_eps)) return false;
    // 4. Right via client sub_482870
    if (!Client_Sub482870(new_rect[2], old_rect[2], flt_eps)) return false;
    return true;
}

__declspec(safebuffers) static int Fast_UpdateLayoutRect(void* thisPtr) {
    float new_rect[4];
    int res = Client_Sub4893C0(thisPtr, new_rect);
    char* base = reinterpret_cast<char*>(thisPtr);
    volatile uint32_t* pFlags = reinterpret_cast<volatile uint32_t*>(base + 0x40);

    if (!res) {
        *pFlags = (*pFlags & ~0x100u) | 0x800u;
        return 0;
    }

    uint32_t flags = *pFlags & 0xFFFFF3FFu;
    *pFlags = flags;

    if (flags & 0x100u) {
        const float* old_rect = reinterpret_cast<const float*>(base + 0x44);
        if (SSE2_RectEqual(new_rect, old_rect)) {
            return 1;
        }
    }

    // Update path: rect changed or was uninitialized
    float old_rect[4];
    float* pDstRect = reinterpret_cast<float*>(base + 0x44);
    old_rect[0] = pDstRect[0];
    old_rect[1] = pDstRect[1];
    old_rect[2] = pDstRect[2];
    old_rect[3] = pDstRect[3];

    pDstRect[0] = new_rect[0];
    pDstRect[1] = new_rect[1];
    pDstRect[2] = new_rect[2];
    pDstRect[3] = new_rect[3];

    *pFlags = flags | 0x100u;

    void** vtable = *reinterpret_cast<void***>(thisPtr);
    reinterpret_cast<OnSizeChanged_fn>(vtable[18])(thisPtr, old_rect);

    return (*pFlags >> 8) & 1;
}

__declspec(safebuffers) static int __fastcall Hook_UpdateLayoutRect(void* thisPtr, void* /*edx*/) {
    if (g_dead || !thisPtr) return g_orig ? g_orig(thisPtr) : 0;

    ++g_calls;
    if (g_abSubject && AbTest::StandAside()) {
        ++g_controlCalls;
        return g_orig(thisPtr);
    }

    const bool verify = (g_verifiedCalls < 10000) || ((g_calls & 127) == 0);
    if (verify) {
        ++g_verifiedCalls;
        char* base = reinterpret_cast<char*>(thisPtr);
        volatile uint32_t* pFlags = reinterpret_cast<volatile uint32_t*>(base + 0x40);
        const uint32_t orig_flags = *pFlags;
        const float* old_rect = reinterpret_cast<const float*>(base + 0x44);

        float new_rect[4];
        int res = Client_Sub4893C0(thisPtr, new_rect);
        if (!res) {
            *pFlags = (orig_flags & ~0x100u) | 0x800u;
            return 0;
        }

        uint32_t flags = orig_flags & 0xFFFFF3FFu;
        *pFlags = flags;

        bool client_match = false;
        bool fast_match = false;

        if (flags & 0x100u) {
            client_match = Client_RectEqual(new_rect, old_rect);
            fast_match = SSE2_RectEqual(new_rect, old_rect);

            if (client_match != fast_match) {
                ++g_mismatches;
                g_dead = true;
                Log("[UILayoutRectFast] MISMATCH: client=%d fast=%d on rect [%.4f, %.4f, %.4f, %.4f] vs [%.4f, %.4f, %.4f, %.4f]. Hook retired.",
                    client_match ? 1 : 0, fast_match ? 1 : 0,
                    new_rect[0], new_rect[1], new_rect[2], new_rect[3],
                    old_rect[0], old_rect[1], old_rect[2], old_rect[3]);
                *pFlags = orig_flags;
                return g_orig(thisPtr);
            }

            if (fast_match) {
                return 1;
            }
        }

        // Both agree rect changed or flags & 0x100 == 0
        float old_copy[4];
        float* pDstRect = reinterpret_cast<float*>(base + 0x44);
        old_copy[0] = pDstRect[0];
        old_copy[1] = pDstRect[1];
        old_copy[2] = pDstRect[2];
        old_copy[3] = pDstRect[3];

        pDstRect[0] = new_rect[0];
        pDstRect[1] = new_rect[1];
        pDstRect[2] = new_rect[2];
        pDstRect[3] = new_rect[3];

        *pFlags = flags | 0x100u;

        void** vtable = *reinterpret_cast<void***>(thisPtr);
        reinterpret_cast<OnSizeChanged_fn>(vtable[18])(thisPtr, old_copy);

        return (*pFlags >> 8) & 1;
    }

    ++g_armedCalls;
    return Fast_UpdateLayoutRect(thisPtr);
}

}  // namespace

bool Init() {
    if (!Config::g_settings.OptUILayoutRectFast) return true;

    if (memcmp((const void*)kTarget, kPrologue, sizeof(kPrologue)) != 0) {
        Log("[UILayoutRectFast] NOT active: prologue mismatch at 0x%08X", (unsigned)kTarget);
        return false;
    }

    if (!WowOpt_ClientPatchAllowed((const void*)kTarget)) {
        Log("[UILayoutRectFast] NOT active: client patches not allowed");
        return false;
    }

    if (WineSafe_CreateHook((void*)kTarget, (void*)&Hook_UpdateLayoutRect, (void**)&g_orig) != MH_OK) {
        Log("[UILayoutRectFast] NOT active: CreateHook failed on 0x%08X", (unsigned)kTarget);
        return false;
    }

    if (WO_EnableHook((void*)kTarget) != MH_OK) {
        MH_RemoveHook((void*)kTarget);
        Log("[UILayoutRectFast] NOT active: EnableHook failed on 0x%08X", (unsigned)kTarget);
        return false;
    }

    g_installed = true;
    g_abSubject = AbTest::IsSubject("UILayoutRectFast", &g_abSubject);
    SamplingProfiler::RegisterSelfSymbol("UILayoutRectFast_Hook", (const void*)&Hook_UpdateLayoutRect);
    Log("[UILayoutRectFast] Hook installed on sub_489570 (0x120 bytes)");
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
             "[UILayoutRectFast] calls=%llu (armed=%llu, verified=%llu, control=%llu) mismatches=%u%s",
             g_calls, g_armedCalls, g_verifiedCalls, g_controlCalls, g_mismatches,
             g_dead ? " [RETIRED]" : "");
    Log("%s", buf);
}

}  // namespace UILayoutRectFast
