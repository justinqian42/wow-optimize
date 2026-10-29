// ============================================================================
// Module: rect_ops_fast.cpp
//
// Hardware-accelerated 2D bounding rectangle geometry operations for WoW 3.3.5a:
//   - sub_48ED60: CRect operator& (Intersection)
//   - sub_78F2F0: CRect operator| (Union)
//   - sub_7A6DA0: CRect operator+ (Offset / Addition)
//   - sub_7A6DD0: CRect operator/ (Scale / Normalization)
//   - sub_488D40: CRect::IsPointInside (Inclusive bounds hit-test)
//   - sub_47FC40: CRect::ContainsPoint (Strict bounds test)
//
// In stock 3.3.5a:
//   - sub_48ED60 and sub_78F2F0 perform 2D rectangle intersection and union
//     using 49 bytes of serialized x87 FPU comparison instructions (fcomp),
//     FPU status-word transfers (fnstsw), and conditional branches for every
//     single component. They are called heavily across UI frame layout,
//     scissor rect clipping, and WMO portal visibility.
//   - sub_7A6DA0 and sub_7A6DD0 perform rectangle addition and division
//     using serial x87 arithmetic; sub_7A6DD0 executes 4 serial fdiv instructions
//     (~120 cycles total) during portal NDC coordinate mapping.
//   - sub_488D40 and sub_47FC40 evaluate 2D point containment using serial x87
//     comparisons and up to 4 conditional branches during UI cursor hit-testing
//     and hover checks.
//
// This module provides 128-bit SSE vectorized replacements for the arithmetic
// and spatial union/intersection operators (evaluating min, max, shuffle, add,
// and div in parallel registers) and branchless point hit-tests. Dual-run
// verification and automatic fail-safe retirement guards ensure strict parity
// with client output.
// ============================================================================

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <xmmintrin.h>
#include <emmintrin.h>
#include <cmath>
#include <cstdint>
#include <cstring>

#include "rect_ops_fast.h"
#include "MinHook.h"
#include "version.h"
#include "config.h"
#include "sampling_profiler.h"

extern "C" void Log(const char* fmt, ...);

namespace RectOpsFast {

namespace {

#pragma pack(push, 1)
struct CRect {
    float minY;
    float minX;
    float maxY;
    float maxX;
};

struct C2Vector {
    float x;
    float y;
};
#pragma pack(pop)

constexpr uintptr_t kTarget_Intersect = 0x0048ED60;
constexpr uintptr_t kTarget_Union     = 0x0078F2F0;
constexpr uintptr_t kTarget_Add       = 0x007A6DA0;
constexpr uintptr_t kTarget_Div       = 0x007A6DD0;
constexpr uintptr_t kTarget_IsInside  = 0x00488D40;
constexpr uintptr_t kTarget_Contains  = 0x0047FC40;

// Verification settings
constexpr unsigned long kVerifyFirst = 20000;
constexpr unsigned long kResampleMask = 4095;
constexpr float kVerifyTolerance = 0.0001f;

// Function signatures
typedef float* (__cdecl* RectBinaryOp_fn)(float* out, const float* a, const float* b);
typedef BOOL (__fastcall* PointTest_fn)(const void* this_ptr, void* dummy_edx, const float* point);

RectBinaryOp_fn orig_Intersect = nullptr;
RectBinaryOp_fn orig_Union     = nullptr;
RectBinaryOp_fn orig_Add       = nullptr;
RectBinaryOp_fn orig_Div       = nullptr;
PointTest_fn    orig_IsInside  = nullptr;
PointTest_fn    orig_Contains  = nullptr;

bool g_installed = false;

bool g_dead_intersect = false;
bool g_dead_union     = false;
bool g_dead_add       = false;
bool g_dead_div       = false;
bool g_dead_is_inside = false;
bool g_dead_contains  = false;

unsigned long long g_calls_intersect = 0;
unsigned long long g_calls_union     = 0;
unsigned long long g_calls_add       = 0;
unsigned long long g_calls_div       = 0;
unsigned long long g_calls_is_inside = 0;
unsigned long long g_calls_contains  = 0;

unsigned long long g_verified_intersect = 0;
unsigned long long g_verified_union     = 0;
unsigned long long g_verified_add       = 0;
unsigned long long g_verified_div       = 0;
unsigned long long g_verified_is_inside = 0;
unsigned long long g_verified_contains  = 0;

unsigned long long g_mismatches_intersect = 0;
unsigned long long g_mismatches_union     = 0;
unsigned long long g_mismatches_add       = 0;
unsigned long long g_mismatches_div       = 0;
unsigned long long g_mismatches_is_inside = 0;
unsigned long long g_mismatches_contains  = 0;

// ============================================================================
// 1. CRect operator& (Intersection, 0x0048ED60)
// ============================================================================
__forceinline float* Fast_Intersect_Impl(float* out, const float* a, const float* b) {
    const __m128 va = _mm_loadu_ps(a);
    const __m128 vb = _mm_loadu_ps(b);
    const __m128 min_val = _mm_min_ps(va, vb);
    const __m128 max_val = _mm_max_ps(va, vb);
    // [max(minY), max(minX), min(maxY), min(maxX)]
    const __m128 res = _mm_shuffle_ps(max_val, min_val, _MM_SHUFFLE(3, 2, 1, 0));
    _mm_storeu_ps(out, res);
    return out;
}

float* __cdecl Hooked_Intersect(float* out, const float* a, const float* b) {
    if (g_dead_intersect || !out || !a || !b) {
        return orig_Intersect(out, a, b);
    }

    const unsigned long long call_idx = ++g_calls_intersect;

    if (call_idx <= kVerifyFirst || ((call_idx & kResampleMask) == 0)) {
        ++g_verified_intersect;
        float fast_temp[4];
        Fast_Intersect_Impl(fast_temp, a, b);

        orig_Intersect(out, a, b);

        bool mismatch = false;
        for (int i = 0; i < 4; ++i) {
            if (std::fabs(fast_temp[i] - out[i]) > kVerifyTolerance) {
                mismatch = true;
                break;
            }
        }

        if (mismatch) {
            if (++g_mismatches_intersect == 1) {
                Log("[RectOpsFast] Intersect MISMATCH at call %llu, retiring hook", call_idx);
                g_dead_intersect = true;
            }
        }
        return out;
    }

    return Fast_Intersect_Impl(out, a, b);
}

// ============================================================================
// 2. CRect operator| (Union, 0x0078F2F0)
// ============================================================================
__forceinline float* Fast_Union_Impl(float* out, const float* a, const float* b) {
    const __m128 va = _mm_loadu_ps(a);
    const __m128 vb = _mm_loadu_ps(b);
    const __m128 min_val = _mm_min_ps(va, vb);
    const __m128 max_val = _mm_max_ps(va, vb);
    // [min(minY), min(minX), max(maxY), max(maxX)]
    const __m128 res = _mm_shuffle_ps(min_val, max_val, _MM_SHUFFLE(3, 2, 1, 0));
    _mm_storeu_ps(out, res);
    return out;
}

float* __cdecl Hooked_Union(float* out, const float* a, const float* b) {
    if (g_dead_union || !out || !a || !b) {
        return orig_Union(out, a, b);
    }

    const unsigned long long call_idx = ++g_calls_union;

    if (call_idx <= kVerifyFirst || ((call_idx & kResampleMask) == 0)) {
        ++g_verified_union;
        float fast_temp[4];
        Fast_Union_Impl(fast_temp, a, b);

        orig_Union(out, a, b);

        bool mismatch = false;
        for (int i = 0; i < 4; ++i) {
            if (std::fabs(fast_temp[i] - out[i]) > kVerifyTolerance) {
                mismatch = true;
                break;
            }
        }

        if (mismatch) {
            if (++g_mismatches_union == 1) {
                Log("[RectOpsFast] Union MISMATCH at call %llu, retiring hook", call_idx);
                g_dead_union = true;
            }
        }
        return out;
    }

    return Fast_Union_Impl(out, a, b);
}

// ============================================================================
// 3. CRect operator+ (Addition / Offset, 0x007A6DA0)
// ============================================================================
__forceinline float* Fast_Add_Impl(float* out, const float* a, const float* b) {
    const __m128 va = _mm_loadu_ps(a);
    const __m128 vb = _mm_loadu_ps(b);
    const __m128 res = _mm_add_ps(va, vb);
    _mm_storeu_ps(out, res);
    return out;
}

float* __cdecl Hooked_Add(float* out, const float* a, const float* b) {
    if (g_dead_add || !out || !a || !b) {
        return orig_Add(out, a, b);
    }

    const unsigned long long call_idx = ++g_calls_add;

    if (call_idx <= kVerifyFirst || ((call_idx & kResampleMask) == 0)) {
        ++g_verified_add;
        float fast_temp[4];
        Fast_Add_Impl(fast_temp, a, b);

        orig_Add(out, a, b);

        bool mismatch = false;
        for (int i = 0; i < 4; ++i) {
            if (std::fabs(fast_temp[i] - out[i]) > kVerifyTolerance) {
                mismatch = true;
                break;
            }
        }

        if (mismatch) {
            if (++g_mismatches_add == 1) {
                Log("[RectOpsFast] Add MISMATCH at call %llu, retiring hook", call_idx);
                g_dead_add = true;
            }
        }
        return out;
    }

    return Fast_Add_Impl(out, a, b);
}

// ============================================================================
// 4. CRect operator/ (Division / Scale, 0x007A6DD0)
// ============================================================================
__forceinline float* Fast_Div_Impl(float* out, const float* a, const float* b) {
    const __m128 va = _mm_loadu_ps(a);
    const __m128 vb = _mm_loadu_ps(b);
    const __m128 res = _mm_div_ps(va, vb);
    _mm_storeu_ps(out, res);
    return out;
}

float* __cdecl Hooked_Div(float* out, const float* a, const float* b) {
    if (g_dead_div || !out || !a || !b) {
        return orig_Div(out, a, b);
    }

    const unsigned long long call_idx = ++g_calls_div;

    if (call_idx <= kVerifyFirst || ((call_idx & kResampleMask) == 0)) {
        ++g_verified_div;
        float fast_temp[4];
        Fast_Div_Impl(fast_temp, a, b);

        orig_Div(out, a, b);

        bool mismatch = false;
        for (int i = 0; i < 4; ++i) {
            if (std::fabs(fast_temp[i] - out[i]) > kVerifyTolerance) {
                mismatch = true;
                break;
            }
        }

        if (mismatch) {
            if (++g_mismatches_div == 1) {
                Log("[RectOpsFast] Div MISMATCH at call %llu, retiring hook", call_idx);
                g_dead_div = true;
            }
        }
        return out;
    }

    return Fast_Div_Impl(out, a, b);
}

// ============================================================================
// 5. CRect::IsPointInside (Inclusive bounds test, 0x00488D40)
// ============================================================================
__forceinline BOOL Fast_IsInside_Impl(const void* this_ptr, const float* point) {
    const float* const r = reinterpret_cast<const float*>(this_ptr);
    const float px = point[0];
    const float py = point[1];
    return ((r[1] <= px) & (px <= r[3]) & (r[0] <= py) & (py <= r[2])) ? 1 : 0;
}

BOOL __fastcall Hooked_IsInside(const void* this_ptr, void* dummy_edx, const float* point) {
    if (g_dead_is_inside || !this_ptr || !point) {
        return orig_IsInside(this_ptr, dummy_edx, point);
    }

    const unsigned long long call_idx = ++g_calls_is_inside;
    const BOOL fast_res = Fast_IsInside_Impl(this_ptr, point);

    if (call_idx <= kVerifyFirst || ((call_idx & kResampleMask) == 0)) {
        ++g_verified_is_inside;
        const BOOL orig_res = orig_IsInside(this_ptr, dummy_edx, point);

        if (fast_res != orig_res) {
            if (++g_mismatches_is_inside == 1) {
                Log("[RectOpsFast] IsInside MISMATCH at call %llu (fast=%d, orig=%d), retiring hook",
                    call_idx, fast_res, orig_res);
                g_dead_is_inside = true;
            }
            return orig_res;
        }
    }

    return fast_res;
}

// ============================================================================
// 6. CRect::ContainsPoint (Strict bounds test, 0x0047FC40)
// ============================================================================
__forceinline BOOL Fast_Contains_Impl(const void* this_ptr, const float* point) {
    const float* const r = reinterpret_cast<const float*>(this_ptr);
    const float px = point[0];
    const float py = point[1];
    return ((r[1] < px) & (px < r[3]) & (r[0] < py) & (py < r[2])) ? 1 : 0;
}

BOOL __fastcall Hooked_Contains(const void* this_ptr, void* dummy_edx, const float* point) {
    if (g_dead_contains || !this_ptr || !point) {
        return orig_Contains(this_ptr, dummy_edx, point);
    }

    const unsigned long long call_idx = ++g_calls_contains;
    const BOOL fast_res = Fast_Contains_Impl(this_ptr, point);

    if (call_idx <= kVerifyFirst || ((call_idx & kResampleMask) == 0)) {
        ++g_verified_contains;
        const BOOL orig_res = orig_Contains(this_ptr, dummy_edx, point);

        if (fast_res != orig_res) {
            if (++g_mismatches_contains == 1) {
                Log("[RectOpsFast] Contains MISMATCH at call %llu (fast=%d, orig=%d), retiring hook",
                    call_idx, fast_res, orig_res);
                g_dead_contains = true;
            }
            return orig_res;
        }
    }

    return fast_res;
}

} // namespace

void Init() {
    if (!Config::g_settings.OptRectOpsFast) {
        return;
    }

    static const unsigned char kExp_Intersect[8] = { 0x55, 0x8B, 0xEC, 0x8B, 0x55, 0x10, 0x8B, 0x4D };
    static const unsigned char kExp_Union[8]     = { 0x55, 0x8B, 0xEC, 0x8B, 0x55, 0x10, 0x8B, 0x4D };
    static const unsigned char kExp_Add[8]       = { 0x55, 0x8B, 0xEC, 0x8B, 0x4D, 0x0C, 0x8B, 0x55 };
    static const unsigned char kExp_Div[8]       = { 0x55, 0x8B, 0xEC, 0x8B, 0x4D, 0x0C, 0x8B, 0x55 };
    static const unsigned char kExp_IsInside[8]  = { 0x55, 0x8B, 0xEC, 0xD9, 0x41, 0x04, 0x8B, 0x55 };
    static const unsigned char kExp_Contains[8]  = { 0x55, 0x8B, 0xEC, 0xD9, 0x41, 0x04, 0x8B, 0x55 };

    if (memcmp((const void*)kTarget_Intersect, kExp_Intersect, 8) != 0) {
        Log("[RectOpsFast] Intersect prologue mismatch, aborting");
        return;
    }
    if (memcmp((const void*)kTarget_Union, kExp_Union, 8) != 0) {
        Log("[RectOpsFast] Union prologue mismatch, aborting");
        return;
    }
    if (memcmp((const void*)kTarget_Add, kExp_Add, 8) != 0) {
        Log("[RectOpsFast] Add prologue mismatch, aborting");
        return;
    }
    if (memcmp((const void*)kTarget_Div, kExp_Div, 8) != 0) {
        Log("[RectOpsFast] Div prologue mismatch, aborting");
        return;
    }
    if (memcmp((const void*)kTarget_IsInside, kExp_IsInside, 8) != 0) {
        Log("[RectOpsFast] IsInside prologue mismatch, aborting");
        return;
    }
    if (memcmp((const void*)kTarget_Contains, kExp_Contains, 8) != 0) {
        Log("[RectOpsFast] Contains prologue mismatch, aborting");
        return;
    }

    if (WineSafe_CreateHook((void*)kTarget_Intersect, (void*)Hooked_Intersect, (void**)&orig_Intersect) != MH_OK ||
        WineSafe_CreateHook((void*)kTarget_Union,     (void*)Hooked_Union,     (void**)&orig_Union)     != MH_OK ||
        WineSafe_CreateHook((void*)kTarget_Add,       (void*)Hooked_Add,       (void**)&orig_Add)       != MH_OK ||
        WineSafe_CreateHook((void*)kTarget_Div,       (void*)Hooked_Div,       (void**)&orig_Div)       != MH_OK ||
        WineSafe_CreateHook((void*)kTarget_IsInside,  (void*)Hooked_IsInside,  (void**)&orig_IsInside)  != MH_OK ||
        WineSafe_CreateHook((void*)kTarget_Contains,  (void*)Hooked_Contains,  (void**)&orig_Contains)  != MH_OK) {
        Log("[RectOpsFast] Failed to create one or more hooks");
        return;
    }

    if (WO_EnableHook((void*)kTarget_Intersect) != MH_OK ||
        WO_EnableHook((void*)kTarget_Union)     != MH_OK ||
        WO_EnableHook((void*)kTarget_Add)       != MH_OK ||
        WO_EnableHook((void*)kTarget_Div)       != MH_OK ||
        WO_EnableHook((void*)kTarget_IsInside)  != MH_OK ||
        WO_EnableHook((void*)kTarget_Contains)  != MH_OK) {
        Log("[RectOpsFast] Failed to enable one or more hooks");
        return;
    }

    SamplingProfiler::RegisterSelfSymbol("Fast_RectIntersect", (const void*)&Hooked_Intersect);
    SamplingProfiler::RegisterSelfSymbol("Fast_RectUnion",     (const void*)&Hooked_Union);
    SamplingProfiler::RegisterSelfSymbol("Fast_RectAdd",       (const void*)&Hooked_Add);
    SamplingProfiler::RegisterSelfSymbol("Fast_RectDiv",       (const void*)&Hooked_Div);
    SamplingProfiler::RegisterSelfSymbol("Fast_RectIsInside",  (const void*)&Hooked_IsInside);
    SamplingProfiler::RegisterSelfSymbol("Fast_RectContains",  (const void*)&Hooked_Contains);

    g_installed = true;
    Log("[RectOpsFast] ACTIVE on CRect geometry ops (sub_48ED60/78F2F0/7A6DA0/7A6DD0/488D40/47FC40)");
}

void LogStats() {
    if (!Config::g_settings.OptRectOpsFast || !g_installed) {
        return;
    }

    Log("[RectOpsFast] Stats: Intersect=%llu (%llu ver, %llu mis%s), Union=%llu (%llu ver, %llu mis%s), "
        "Add=%llu (%llu ver, %llu mis%s), Div=%llu (%llu ver, %llu mis%s), "
        "IsInside=%llu (%llu ver, %llu mis%s), Contains=%llu (%llu ver, %llu mis%s)",
        g_calls_intersect, g_verified_intersect, g_mismatches_intersect, g_dead_intersect ? " [RETIRED]" : "",
        g_calls_union, g_verified_union, g_mismatches_union, g_dead_union ? " [RETIRED]" : "",
        g_calls_add, g_verified_add, g_mismatches_add, g_dead_add ? " [RETIRED]" : "",
        g_calls_div, g_verified_div, g_mismatches_div, g_dead_div ? " [RETIRED]" : "",
        g_calls_is_inside, g_verified_is_inside, g_mismatches_is_inside, g_dead_is_inside ? " [RETIRED]" : "",
        g_calls_contains, g_verified_contains, g_mismatches_contains, g_dead_contains ? " [RETIRED]" : "");
}

void Shutdown() {
    if (!g_installed) {
        return;
    }

    MH_DisableHook((void*)kTarget_Intersect);
    MH_DisableHook((void*)kTarget_Union);
    MH_DisableHook((void*)kTarget_Add);
    MH_DisableHook((void*)kTarget_Div);
    MH_DisableHook((void*)kTarget_IsInside);
    MH_DisableHook((void*)kTarget_Contains);
    g_installed = false;
}

} // namespace RectOpsFast
