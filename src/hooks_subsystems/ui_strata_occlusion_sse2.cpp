// ============================================================================
// Module: ui_strata_occlusion_sse2.cpp
//
// Fast AABB bounds check and occlusion culling for CFrameStrata::CheckOcclusion
// (sub_494E60) and CFrameStrata::FrameOccluded (sub_494D20).
//
// In stock WoW 3.3.5a build 12340:
// Whenever any frame changes level or moves, CFrameStrata::CheckOcclusion
// executes an O(N^2) scan across all frames. Inside the inner loop of
// FrameOccluded, it repeatedly calls GetRect (0x00489230) on every single
// iteration, plus executes ancestor parent pointer chasing (m_parent at +0x94),
// and executes slow serialized x87 rect intersections (CRect::operator& at 0x0048ED60).
//
// With heavy UI configurations (WeakAuras, Details, ElvUI, Gladius), this drains
// significant CPU frame time during UI updates.
//
// Optimization:
//   Pass 1: Flatten all active frames in the strata into a cache-coherent contiguous
//           array of compact structs. Read layout valid flags (offset +0x60) and
//           bounding rect (offset +0x64) directly without virtual calls.
//   Pass 2: Vectorize 2D AABB intersection using SSE2 _mm_cmplt_ps and movemask
//           without x87 status word pipeline stalls (fnstsw ax).
//
// Correctness & Verification:
//   Dual-run verification compares the decision against the client's verbatim
//   FrameOccluded (sub_494D20) for the first 5,000 calls. Any disagreement logs
//   full diagnostics and immediately retires the detour for the session.
// ============================================================================

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <emmintrin.h>
#include <cstdint>
#include <cstring>
#include <vector>

#include "ui_strata_occlusion_sse2.h"
#include "MinHook.h"
#include "version.h"
#include "config.h"
#include "server_sync_compat.h"

extern "C" void Log(const char* fmt, ...);

namespace UIStrataOcclusionFast {

namespace {

constexpr uintptr_t kTargetCheckOcclusion = 0x00494E60;
constexpr uintptr_t kTargetFrameOccluded  = 0x00494D20;

// Verbatim 11 prologue bytes of sub_494E60:
// 56 57 8B F1 33 FF 39 7E 08 76 64
const unsigned char kExpectedPrologue[11] = {
    0x56, 0x57, 0x8B, 0xF1, 0x33, 0xFF, 0x39, 0x7E, 0x08, 0x76, 0x64
};

typedef void (__fastcall* CheckOcclusion_fn)(void* self, void* dummy_edx);
typedef int32_t (__fastcall* FrameOccluded_fn)(void* strata, void* dummy_edx, void* frame);

static CheckOcclusion_fn g_origCheckOcclusion = nullptr;
static FrameOccluded_fn  g_origFrameOccluded  = reinterpret_cast<FrameOccluded_fn>(kTargetFrameOccluded);

bool g_installed = false;
bool g_dead = false;

uint64_t g_calls = 0;
uint64_t g_framesEvaluated = 0;
uint64_t g_framesOccluded = 0;
uint64_t g_rectFastpaths = 0;
uint64_t g_verified = 0;

constexpr uint32_t kVerifyFirst = 5000;
constexpr uint32_t kResampleMask = 127;

static __forceinline bool IsValidUserPtr(const void* p) {
    return (uintptr_t)p >= 0x10000 && (uintptr_t)p <= 0xFFFF0000;
}

struct alignas(16) FrameOcclusionEntry {
    void*    frame;         // CSimpleFrame*
    void*    parent;        // CSimpleFrame* m_parent (at frame + 0x94)
    uint32_t level;         // m_level (at frame + 0xD4)
    uint32_t frameFlags;    // m_frameFlags (at frame + 0xB4)
    __m128   sse_vec;       // (minX, -maxX, minY, -maxY)
    bool     hasValidRect;
};

// Static thread-local scratch buffer for common single strata sizes (< 2048 frames)
constexpr size_t kScratchCapacity = 2048;
static FrameOcclusionEntry s_scratch[kScratchCapacity];

__declspec(safebuffers) static void FastCheckOcclusion(void* self) {
    if (!IsValidUserPtr(self)) {
        return;
    }

    const uint32_t topLevel = *(const uint32_t*)((const char*)self + 8);
    if (topLevel == 0 || topLevel > 256) {
        return;
    }

    void* const* levelsData = *(void* const* const*)((const char*)self + 0x14);
    if (!IsValidUserPtr(levelsData)) {
        return;
    }

    // Pass 1: Gather active frames across all levels in this strata
    FrameOcclusionEntry* entries = s_scratch;
    std::vector<FrameOcclusionEntry> dynamicEntries;
    size_t totalFrames = 0;

    for (uint32_t lvl = 0; lvl < topLevel; ++lvl) {
        const void* node = levelsData[lvl];
        if (!IsValidUserPtr(node)) {
            continue;
        }

        // node + 0x14 is m_frames.m_terminator.m_next (Head)
        uintptr_t headVal = *(const uintptr_t*)((const char*)node + 0x14);
        if ((headVal & 1) != 0 || headVal == 0) {
            continue;
        }

        void* frame = (void*)headVal;
        while (IsValidUserPtr(frame)) {
            if (totalFrames >= kScratchCapacity && dynamicEntries.empty()) {
                // Grow dynamically if UI has an extreme number of frames in one strata
                dynamicEntries.assign(s_scratch, s_scratch + totalFrames);
                dynamicEntries.reserve(totalFrames * 2);
            }

            FrameOcclusionEntry* curEntry = nullptr;
            if (dynamicEntries.empty()) {
                curEntry = &entries[totalFrames];
            } else {
                dynamicEntries.emplace_back();
                curEntry = &dynamicEntries.back();
            }
            ++totalFrames;

            curEntry->frame = frame;
            curEntry->parent = *(void**)((const char*)frame + 0x94);
            curEntry->level = lvl;
            const uint32_t flags = *(const uint32_t*)((const char*)frame + 0xB4);
            curEntry->frameFlags = flags;

            // Direct layout bounds extraction from CLayoutFrame (frame + 0x20):
            // m_layoutFlags is at frame + 0x60. Bit 0x100 indicates rect is valid.
            const uint32_t layoutFlags = *(const uint32_t*)((const char*)frame + 0x60);
            if ((layoutFlags & 0x100) != 0) {
                // m_rect is at frame + 0x64 (4 IEEE floats: minY, minX, maxY, maxX)
                const float* r = (const float*)((const char*)frame + 0x64);
                const float minY = r[0];
                const float minX = r[1];
                const float maxY = r[2];
                const float maxX = r[3];

                if (maxY > minY && maxX > minX) {
                    curEntry->hasValidRect = true;
                    // Store as (minX, -maxX, minY, -maxY)
                    curEntry->sse_vec = _mm_set_ps(-maxY, minY, -maxX, minX);
                    ++g_rectFastpaths;
                } else {
                    curEntry->hasValidRect = false;
                    curEntry->sse_vec = _mm_setzero_ps();
                }
            } else {
                curEntry->hasValidRect = false;
                curEntry->sse_vec = _mm_setzero_ps();
            }

            // Next frame:
            // Frame strata link is at frame + 0x294. m_next is at offset +4 (+0x298).
            uintptr_t nextVal = *(const uintptr_t*)((const char*)frame + 0x298);
            if ((nextVal & 1) != 0 || nextVal == 0) {
                break;
            }
            frame = (void*)nextVal;
        }
    }

    if (totalFrames == 0) {
        return;
    }

    if (!dynamicEntries.empty()) {
        entries = dynamicEntries.data();
    }

    // Pass 2: Vectorized Occlusion Evaluation
    const uint64_t callIdx = g_calls++;
    const bool verify = (callIdx < kVerifyFirst) || ((callIdx & kResampleMask) == 0);

    for (size_t i = 0; i < totalFrames; ++i) {
        // Only visible frames have occlusion evaluated (m_frameFlags & 1)
        if ((entries[i].frameFlags & 1) == 0) {
            continue;
        }

        ++g_framesEvaluated;
        bool occluded = false;

        if (entries[i].hasValidRect) {
            void* const curFrame = entries[i].frame;
            const uint32_t curLevel = entries[i].level;

            // frame_vec = (maxX, -minX, maxY, -minY)
            // Extract components from entries[i].sse_vec which holds (minX, -maxX, minY, -maxY)
            alignas(16) float fVec[4];
            _mm_store_ps(fVec, entries[i].sse_vec);
            const float fMinX = fVec[0];
            const float fMaxX = -fVec[1];
            const float fMinY = fVec[2];
            const float fMaxY = -fVec[3];

            const __m128 frame_vec = _mm_set_ps(-fMinY, fMaxY, -fMinX, fMaxX);

            for (size_t j = 0; j < totalFrames; ++j) {
                if (entries[j].level < curLevel) continue;
                if (entries[j].frame == curFrame) continue;
                if (!entries[j].hasValidRect) continue;

                // SSE2 2D AABB intersection:
                // Test: other.minX < frame.maxX && other.maxX > frame.minX &&
                //       other.minY < frame.maxY && other.maxY > frame.minY
                const __m128 cmp = _mm_cmplt_ps(entries[j].sse_vec, frame_vec);
                if (_mm_movemask_ps(cmp) == 0xF) {
                    // Rectangles overlap. Check if entries[j] is a descendant of curFrame:
                    void* p = entries[j].parent;
                    while (p != nullptr && p != curFrame) {
                        p = *(void**)((const char*)p + 0x94);
                    }

                    if (p != nullptr) {
                        // entries[j] is a child/descendant of curFrame, so it cannot occlude curFrame
                        continue;
                    }

                    occluded = true;
                    break;
                }
            }
        }

        if (verify && !g_dead) {
            int32_t clientRes = (g_origFrameOccluded(self, nullptr, entries[i].frame) != 0) ? 1 : 0;
            int32_t fastRes = occluded ? 1 : 0;
            if (fastRes != clientRes) {
                g_dead = true;
                Log("[UIStrataOcclusion] RETIRED on call %llu: client=%d fast=%d frame=0x%p",
                    callIdx, clientRes, fastRes, entries[i].frame);
                // Fall back to client decision for this frame
                occluded = (clientRes != 0);
            } else {
                ++g_verified;
            }
        }

        // Apply occlusion flag 0x10 to frame->m_frameFlags (+0xB4)
        uint32_t* pFlags = (uint32_t*)((char*)entries[i].frame + 0xB4);
        if (occluded) {
            *pFlags |= 0x10;
            ++g_framesOccluded;
        } else {
            *pFlags &= ~0x10;
        }
    }
}

static void __fastcall Hook_CheckOcclusion(void* self, void* dummy_edx) {
    if (g_dead || !IsValidUserPtr(self)) {
        g_origCheckOcclusion(self, dummy_edx);
        return;
    }

    FastCheckOcclusion(self);
}

} // anonymous namespace

bool Init() {
    if (!Config::g_settings.OptUIStrataOcclusionFast) {
        return true;
    }

    void* target = reinterpret_cast<void*>(kTargetCheckOcclusion);
    if (!WowOpt_ClientPatchAllowed(target)) {
        Log("[UIStrataOcclusion] Client patch not allowed at 0x%08X", (unsigned)kTargetCheckOcclusion);
        return false;
    }

    if (memcmp(target, kExpectedPrologue, sizeof(kExpectedPrologue)) != 0) {
        Log("[UIStrataOcclusion] BAD PROLOGUE at 0x%08X", (unsigned)kTargetCheckOcclusion);
        return false;
    }

    if (MH_CreateHook(target, reinterpret_cast<void*>(Hook_CheckOcclusion),
                      reinterpret_cast<void**>(&g_origCheckOcclusion)) != MH_OK) {
        Log("[UIStrataOcclusion] MH_CreateHook FAILED at 0x%08X", (unsigned)kTargetCheckOcclusion);
        return false;
    }

    if (MH_EnableHook(target) != MH_OK) {
        Log("[UIStrataOcclusion] MH_EnableHook FAILED at 0x%08X", (unsigned)kTargetCheckOcclusion);
        return false;
    }

    g_installed = true;
    ServerSyncCompat::RegisterHook(kTargetCheckOcclusion, reinterpret_cast<void*>(Hook_CheckOcclusion), 5);
    Log("[UIStrataOcclusion] ACTIVE -- SSE2 vectorized AABB strata occlusion at 0x%08X", (unsigned)kTargetCheckOcclusion);
    return true;
}

void LogStats() {
    if (!Config::g_settings.OptUIStrataOcclusionFast) return;
    if (!g_installed) {
        Log("[UIStrataOcclusion] not installed - nothing measured");
        return;
    }
    if (g_calls == 0) {
        Log("[UIStrataOcclusion] installed but never called");
        return;
    }

    double occludedPct = (g_framesEvaluated > 0) ? (100.0 * (double)g_framesOccluded / (double)g_framesEvaluated) : 0.0;

    Log("[UIStrataOcclusion] %llu calls%s: %llu frames evaluated (%llu occluded, %.1f%%), %llu rect fastpaths, %llu verified against client.",
        g_calls,
        g_dead ? " - RETIRED on disagreement" : "",
        g_framesEvaluated, g_framesOccluded, occludedPct, g_rectFastpaths, g_verified);
}

void Shutdown() {
    if (g_installed) {
        MH_DisableHook(reinterpret_cast<void*>(kTargetCheckOcclusion));
        g_installed = false;
    }
}

} // namespace UIStrataOcclusionFast
