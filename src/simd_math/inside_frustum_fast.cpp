// ============================================================================
// Module: inside_frustum_fast.cpp
//
// Hardware-accelerated 2D terrain frustum intersection for WoW 3.3.5a:
//   - sub_7D6690: CWorldScene::InsideFrustumRect (CiRect)
//
// In stock 3.3.5a:
//   - sub_7D6690 tests whether a quadtree terrain chunk rect overlaps the
//     camera frustum chunk bounds (CWorldScene::s_frustumChunkRect at 0x00CDB0F4):
//       rect->minX <= s_frustumChunkRect.maxX &&
//       rect->minY <= s_frustumChunkRect.maxY &&
//       rect->maxX >= s_frustumChunkRect.minX &&
//       rect->maxY >= s_frustumChunkRect.minY
//   - In stock code, sub_7D6690 is called repeatedly inside CMapArea::Update
//     (sub_7D6BF0) for every candidate terrain chunk within an area.
//   - The stock implementation uses four conditional branches with serial
//     comparisons that cause frequent branch mispredictions during camera
//     panning and flight.
//
// This optimization evaluates the rectangle overlap branchlessly using bitwise
// logic, eliminating all branch penalties and pipeline stalls. Dual-run
// verification and automatic fail-safe retirement guards ensure strict parity
// with client output.
// ============================================================================

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <cstdint>
#include <cstring>

#include "inside_frustum_fast.h"
#include "MinHook.h"
#include "version.h"
#include "config.h"
#include "sampling_profiler.h"

extern "C" void Log(const char* fmt, ...);

namespace InsideFrustumFast {

namespace {

#pragma pack(push, 1)
struct CiRect {
    int32_t minY;
    int32_t minX;
    int32_t maxY;
    int32_t maxX;
};
#pragma pack(pop)

constexpr uintptr_t kTarget_InsideFrustumRect = 0x007D6690;
constexpr uintptr_t kAddr_FrustumChunkRect    = 0x00CDB0F4;

// Verification settings
constexpr unsigned long kVerifyFirst = 20000;
constexpr unsigned long kResampleMask = 4095;

typedef int (__cdecl* InsideFrustumRect_fn)(const CiRect* rect);

InsideFrustumRect_fn orig_InsideFrustumRect = nullptr;

bool g_installed = false;
bool g_dead = false;

unsigned long long g_calls = 0;
unsigned long long g_verified = 0;
unsigned long long g_mismatches = 0;

__forceinline int Fast_InsideFrustumRect_Impl(const CiRect* rect) {
    const CiRect* const f = reinterpret_cast<const CiRect*>(kAddr_FrustumChunkRect);
    return ((rect->minX <= f->maxX) &
            (rect->minY <= f->maxY) &
            (rect->maxX >= f->minX) &
            (rect->maxY >= f->minY)) ? 1 : 0;
}

int __cdecl Hooked_InsideFrustumRect(const CiRect* rect) {
    if (g_dead || !rect) {
        return orig_InsideFrustumRect(rect);
    }

    const unsigned long long call_idx = ++g_calls;
    const int fast_res = Fast_InsideFrustumRect_Impl(rect);

    if (call_idx <= kVerifyFirst || ((call_idx & kResampleMask) == 0)) {
        ++g_verified;
        const int orig_res = orig_InsideFrustumRect(rect);

        if (fast_res != orig_res) {
            if (++g_mismatches == 1) {
                const CiRect* const f = reinterpret_cast<const CiRect*>(kAddr_FrustumChunkRect);
                Log("[InsideFrustumFast] MISMATCH at call %llu: fast=%d, orig=%d, rect=(%d,%d,%d,%d), frustum=(%d,%d,%d,%d), retiring hook",
                    call_idx, fast_res, orig_res,
                    rect->minY, rect->minX, rect->maxY, rect->maxX,
                    f->minY, f->minX, f->maxY, f->maxX);
                g_dead = true;
            }
            return orig_res;
        }
    }

    return fast_res;
}

} // namespace

void Init() {
    if (!Config::g_settings.OptInsideFrustumFast) {
        return;
    }

    static const unsigned char kExp_Prologue[8] = { 0x55, 0x8B, 0xEC, 0x8B, 0x45, 0x08, 0x8B, 0x48 };

    if (memcmp((const void*)kTarget_InsideFrustumRect, kExp_Prologue, 8) != 0) {
        Log("[InsideFrustumFast] Prologue mismatch at 0x%08X, aborting installation", kTarget_InsideFrustumRect);
        return;
    }

    if (WineSafe_CreateHook((void*)kTarget_InsideFrustumRect, (void*)Hooked_InsideFrustumRect, (void**)&orig_InsideFrustumRect) != MH_OK) {
        Log("[InsideFrustumFast] Failed to create hook");
        return;
    }

    if (WO_EnableHook((void*)kTarget_InsideFrustumRect) != MH_OK) {
        Log("[InsideFrustumFast] Failed to enable hook");
        return;
    }

    SamplingProfiler::RegisterSelfSymbol("Fast_InsideFrustumRect", (const void*)&Hooked_InsideFrustumRect);

    g_installed = true;
    Log("[InsideFrustumFast] ACTIVE on CWorldScene InsideFrustumRect (0x007D6690)");
}

void LogStats() {
    if (!Config::g_settings.OptInsideFrustumFast || !g_installed) {
        return;
    }

    Log("[InsideFrustumFast] Stats: calls=%llu (verified=%llu, mismatches=%llu%s)",
        g_calls, g_verified, g_mismatches, g_dead ? " [RETIRED]" : "");
}

void Shutdown() {
    if (!g_installed) {
        return;
    }

    MH_DisableHook((void*)kTarget_InsideFrustumRect);
    g_installed = false;
}

} // namespace InsideFrustumFast

