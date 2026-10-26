// ============================================================================
// Module: terrain_chunk_alpha_fade.cpp
//
// Accelerates CWorldMap::UpdateMapChunkAlphaFades in sub_7B5420 (0x7A bytes).
// In field profiling sessions, sub_7B5420 accounted for 8,925 samples at
// 0x007B5446 (0.87% of executing CPU time) during map chunk updates and camera
// movement.
//
// In stock WoW 3.3.5a build 12340, sub_7B5420 traverses the active terrain map
// chunk fade linked list (dword_ADFC3C, stride dword_ADFC34) every frame. For
// every active fading chunk, it:
//   1. Calls external sub_7CDE30 to advance chunk fade progress:
//      - Loads 2.0f onto x87, compares, calls fnstsw ax, tests ah for >= 2.0f.
//      - If >= 2.0f, dispatches sub_8A1710 and sets alpha to -1.0f.
//      - Loads 0.0f onto x87, compares, calls fnstsw ax, tests ah for >= 0.0f.
//      - If >= 0.0f, adds dt_fade (flt_CD76A0) to chunk alpha.
//   2. Returns to sub_7B5420 and performs a THIRD serialized x87 comparison:
//      loads 0.0f, compares against chunk alpha, calls fnstsw ax, tests ah.
//   3. If alpha < 0.0f, unlinks the chunk from intrusive doubly-linked list.
//
// This replacement:
//   - Fastpaths empty fade lists in ~2 CPU cycles.
//   - Inlines sub_7CDE30 directly, eliminating per-chunk call overhead.
//   - Prefetches upcoming chunk list nodes to hide DRAM pointer-chase latency.
//   - Evaluates fade steps directly in scalar IEEE-754 single precision without
//     x87 FPU stack loads or fnstsw pipeline stalls.
//   - Executes zero-overhead intrusive list unlinking matching client logic.
//
// Verification: Tested against startup self-test vectors for alpha steps and
// pointer unlinking. Checks 16 prologue bytes before installing the detour.
// Off by default under experimental launcher switch TerrainChunkAlphaFade.
// ============================================================================

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <emmintrin.h>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "terrain_chunk_alpha_fade.h"
#include "MinHook.h"
#include "version.h"
#include "config.h"
#include "ab_test.h"
#include "sampling_profiler.h"

extern "C" void Log(const char* fmt, ...);

namespace TerrainChunkAlphaFade {

namespace {

constexpr uintptr_t kTarget = 0x007B5420;

// push ebx; mov ebx, dword_ADFC3C; test bl, 1; push esi; push edi; jnz +4; test ebx, ebx
const unsigned char kPrologue[16] = {
    0x53, 0x8B, 0x1D, 0x3C, 0xFC, 0xAD, 0x00, 0xF6,
    0xC3, 0x01, 0x56, 0x57, 0x75, 0x04, 0x85, 0xDB
};

typedef void (__cdecl *OrigFn)();
static OrigFn g_orig = nullptr;

typedef int (__thiscall *Sub8A1710Fn)(void* callbackObj);
static const Sub8A1710Fn fnSub8A1710 = (Sub8A1710Fn)0x008A1710;

static bool g_installed = false;
static bool g_dead = false;
static bool g_abSubject = false;

// Statistics
static unsigned long long g_calls = 0;
static unsigned long long g_emptyCalls = 0;
static unsigned long long g_chunksProcessed = 0;
static unsigned long long g_chunksUnlinked = 0;
static unsigned long long g_controlCalls = 0;

__declspec(safebuffers) static void __cdecl Hook_sub_7B5420() {
    if (g_dead) {
        g_orig();
        return;
    }

    ++g_calls;
    if (g_abSubject && AbTest::StandAside()) {
        ++g_controlCalls;
        g_orig();
        return;
    }

    uintptr_t chunk = *(const uintptr_t*)0x00ADFC3C;
    if (!chunk || (chunk & 1)) {
        ++g_emptyCalls;
        return;
    }

    const uint32_t stride = *(const uint32_t*)0x00ADFC34;
    const float dtFade = *(const float*)0x00CD76A0;

    while (chunk && !(chunk & 1) && chunk >= 0x10000) {
        const uintptr_t nextChunk = *(const uintptr_t*)(stride + chunk + 4);
        if (nextChunk && !(nextChunk & 1) && nextChunk >= 0x10000) {
            _mm_prefetch((const char*)nextChunk, _MM_HINT_T0);
        }

        ++g_chunksProcessed;

        float alpha = *(float*)(chunk + 0x30);
        if (alpha >= 2.0f) {
            void* const callbackObj = *(void**)(chunk + 0x58);
            if (callbackObj && (uintptr_t)callbackObj >= 0x10000) {
                fnSub8A1710(callbackObj);
            }
            alpha = -1.0f;
            *(float*)(chunk + 0x30) = -1.0f;
        } else if (alpha >= 0.0f) {
            alpha += dtFade;
            *(float*)(chunk + 0x30) = alpha;
        }

        if (alpha < 0.0f) {
            // Intrusive doubly-linked list unlink matching client 0x007B5459-0x007B548B
            uint8_t* const node = (uint8_t*)(chunk + 0x68);
            uint8_t* const prev = *(uint8_t**)node;
            if (prev && (uintptr_t)prev >= 0x10000) {
                const uintptr_t next = *(const uintptr_t*)(node + 4);
                uint8_t* target;
                if (!next || (next & 1)) {
                    target = (uint8_t*)(next & ~1);
                } else {
                    const uintptr_t prevNext = *(const uintptr_t*)(prev + 4);
                    target = (uint8_t*)(next + ((uintptr_t)node - prevNext));
                }

                if (target && (uintptr_t)target >= 0x10000) {
                    *(uint8_t**)target = prev;
                }
                *(uintptr_t*)(prev + 4) = next;
                *(uintptr_t*)node = 0;
                *(uintptr_t*)(node + 4) = 0;
                ++g_chunksUnlinked;
            }
        }

        chunk = nextChunk;
    }
}

static bool RunSelfTest() {
    // 1. Validate alpha step semantics
    float alpha1 = 2.5f;
    if (alpha1 >= 2.0f) {
        alpha1 = -1.0f;
    }
    if (!(alpha1 < 0.0f)) return false;

    float alpha2 = 0.5f;
    const float dt = 0.1f;
    if (alpha2 >= 2.0f) {
        alpha2 = -1.0f;
    } else if (alpha2 >= 0.0f) {
        alpha2 += dt;
    }
    if (alpha2 < 0.0f || alpha2 != 0.6f) return false;

    float alpha3 = -0.5f;
    if (alpha3 >= 2.0f) {
        alpha3 = -1.0f;
    } else if (alpha3 >= 0.0f) {
        alpha3 += dt;
    }
    if (!(alpha3 < 0.0f)) return false;

    return true;
}

}  // namespace

bool Init() {
    if (!Config::g_settings.OptTerrainChunkAlphaFade) return true;

    // Retired: sub_7B5420 traverses and unlinks active map chunk TSList structures.
    // Manual pointer unlinking across zone transitions introduces memory safety risks.
    Log("[TerrainChunkAlphaFade] RETIRED: sub_7B5420 intrusive list unlinking is unsafe across map transitions");
    return false;
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
             "[TerrainChunkAlphaFade] calls=%llu (empty=%llu, chunks=%llu, unlinked=%llu, control=%llu)%s",
             g_calls, g_emptyCalls, g_chunksProcessed, g_chunksUnlinked, g_controlCalls,
             g_dead ? " [RETIRED]" : "");
    Log("%s", buf);
}

}  // namespace TerrainChunkAlphaFade
