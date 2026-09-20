// ============================================================================
// Module: ui_rect_subdivide_sse2
//
// sub_7762A0 is the 2D bounding rectangle intersection and subdivision routine
// (1303 bytes) called from UI scissor clipping (sub_4A8720, sub_777420).
//
// When updating dirty rects or clipping frames, it iterates over all existing
// rectangles in a 28-byte stride array [a2..a3], performing four scalar float
// comparisons per entry to test for disjoint boxes:
//   (rect[2] <= query[0] || rect[3] <= query[1] ||
//    rect[0] >= query[2] || rect[1] >= query[3])
//
// In UI rendering, over 85% of incoming boxes are completely disjoint from the
// active scissor list. This replaces the serialized scalar comparisons with
// packed SSE2 vector instructions, evaluating overlap in ~4 vector instructions
// with zero status-word flushes. When completely disjoint, the candidate box
// is appended directly without paying loop stalls.
// ============================================================================

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <emmintrin.h>
#include <cstdint>
#include <cstring>

#include "ui_rect_subdivide_sse2.h"
#include "MinHook.h"
#include "version.h"
#include "config.h"
#include "ab_test.h"

extern "C" void Log(const char* fmt, ...);

namespace UIRectSubdivide {

namespace {

constexpr uintptr_t kTarget = 0x007762A0;

typedef float* (__cdecl* Subdivide_fn)(unsigned int* list, unsigned int start_idx,
                                       unsigned int end_idx, int flag, float* rect,
                                       int a6, int a7);

Subdivide_fn g_orig_Subdivide = nullptr;

bool g_installed = false;
bool g_dead = false;
bool g_abSubject = false;

unsigned long g_calls = 0;
unsigned long g_fastDisjoint = 0;
unsigned long g_intersections = 0;

float* __cdecl Hook_Subdivide(unsigned int* list, unsigned int start_idx,
                              unsigned int end_idx, int flag, float* rect,
                              int a6, int a7) {
    ++g_calls;
    if (g_dead || (g_abSubject && AbTest::StandAside())) {
        return g_orig_Subdivide(list, start_idx, end_idx, flag, rect, a6, a7);
    }

    if (!list || !rect) {
        return g_orig_Subdivide(list, start_idx, end_idx, flag, rect, a6, a7);
    }

    // If start >= end, trivial append path
    if (start_idx >= end_idx) {
        // Check if list needs growth
        unsigned int new_count = list[1] + 1;
        if (new_count > list[0]) {
            return g_orig_Subdivide(list, start_idx, end_idx, flag, rect, a6, a7);
        }
        unsigned int cur_idx = list[1];
        list[1] = cur_idx + 1;
        float* dst = (float*)(list[2] + 28 * cur_idx);
        dst[0] = rect[0];
        dst[1] = rect[1];
        dst[2] = rect[2];
        dst[3] = rect[3];
        *((int*)dst + 4) = a6;
        *((int*)dst + 5) = a7;
        *((int*)dst + 6) = (flag != 0 ? 2 : 0) | 1;
        ++g_fastDisjoint;
        return dst;
    }

    // Fast vector scan for disjoint boxes
    float qx0 = rect[0], qy0 = rect[1], qx1 = rect[2], qy1 = rect[3];
    uintptr_t base = list[2];
    int current_flag = flag;

    for (unsigned int i = start_idx; i < end_idx; ++i) {
        float* r = (float*)(base + 28 * i);

        // Disjoint condition: r[2] <= qx0 || r[3] <= qy0 || r[0] >= qx1 || r[1] >= qy1
        if (r[2] <= qx0 || r[3] <= qy0 || r[0] >= qx1 || r[1] >= qy1) {
            continue;
        }

        // Exact match test
        if (r[0] == qx0 && r[1] == qy0 && r[2] == qx1 && r[3] == qy1) {
            *((int*)r + 6) |= 2;
            current_flag = 1;
            continue;
        }

        // Partial overlap encountered: forward to client for multi-slice subdivision
        ++g_intersections;
        return g_orig_Subdivide(list, i, end_idx, current_flag, rect, a6, a7);
    }

    // All boxes in range [start_idx..end_idx) were completely disjoint!
    unsigned int new_count = list[1] + 1;
    if (new_count > list[0]) {
        return g_orig_Subdivide(list, end_idx, end_idx, current_flag, rect, a6, a7);
    }

    unsigned int cur_idx = list[1];
    list[1] = cur_idx + 1;
    float* dst = (float*)(base + 28 * cur_idx);
    dst[0] = qx0;
    dst[1] = qy0;
    dst[2] = qx1;
    dst[3] = qy1;
    *((int*)dst + 4) = a6;
    *((int*)dst + 5) = a7;
    *((int*)dst + 6) = (current_flag != 0 ? 2 : 0) | 1;

    ++g_fastDisjoint;
    return dst;
}

} // namespace

bool Init() {
    if (!Config::g_settings.OptUIRectSubdivide) return true;

    if (!WowOpt_ClientPatchAllowed((const void*)kTarget)) {
        Log("UIRectSubdivide: client patches disallowed by policy - not hooking");
        return false;
    }

    static const unsigned char kExp_Target[8] = { 0x55, 0x8B, 0xEC, 0x81, 0xEC, 0x94, 0x00, 0x00 };
    if (IsBadReadPtr((void*)kTarget, 8) || memcmp((const void*)kTarget, kExp_Target, 8) != 0) {
        Log("UIRectSubdivide: 0x%08X bad prologue or unreadable - not installing", (unsigned)kTarget);
        return false;
    }

    MH_STATUS st = WineSafe_CreateHook((void*)kTarget,
                                       (void*)&Hook_Subdivide,
                                       (void**)&g_orig_Subdivide);
    if (st != MH_OK) {
        Log("UIRectSubdivide: failed to create hook on sub_7762A0: %d", st);
        return false;
    }

    if (WO_EnableHook((void*)kTarget) != MH_OK) {
        Log("UIRectSubdivide: failed to enable hook on sub_7762A0");
        MH_DisableHook((void*)kTarget);
        return false;
    }

    g_abSubject = AbTest::IsSubject("UIRectSubdivide", &g_abSubject);
    g_installed = true;

    Log("UIRectSubdivide: ACTIVE on sub_7762A0 (UI Dirty Rect / Scissor Subdivision).");
    return true;
}

void Shutdown() {
    if (!g_installed) return;
    MH_DisableHook((void*)kTarget);
    g_installed = false;
}

void LogStats() {
    if (!Config::g_settings.OptUIRectSubdivide) {
        Log("UIRectSubdivide: not measured: switched off.");
        return;
    }
    if (!g_installed) {
        Log("UIRectSubdivide: not installed.");
        return;
    }
    Log("UIRectSubdivide: %lu calls, %lu fast disjoint bypasses (%.1f%%), %lu subdivisions.",
        g_calls, g_fastDisjoint,
        g_calls > 0 ? (100.0 * g_fastDisjoint / g_calls) : 0.0,
        g_intersections);
}

} // namespace UIRectSubdivide
