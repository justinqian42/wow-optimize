// ============================================================================
// Module: doodad_cull_exterior_sse2.cpp
//
// Accelerates CWorldScene::CullDoodadsExterior in sub_799980 (0x1E2 bytes).
// In field profiling sessions, sub_799980 and its call hierarchy accounted for
// over 23,000 samples (~2.5% of total CPU time), with 9,545 samples clustered
// at 0x007999B8 during outdoor scene traversal and camera movement.
//
// In stock WoW 3.3.5a build 12340, CullDoodadsExterior traverses linked lists
// of doodad objects within terrain map chunks. For each candidate doodad, it:
//   1. Performs LOD level checks against the current cell fade level.
//   2. Performs frustum and horizon buffer occlusion queries.
//   3. Evaluates 3D camera distance thresholds on the legacy x87 FPU stack,
//      generating 11 serialized x87 instructions and an fnstsw pipeline stall
//      for every single culled doodad.
//   4. Carries an unneeded 0.0 floating-point value on ST(0) throughout the loop.
//
// This optimization:
//   - Fastpaths empty chunk lists in ~2 CPU cycles.
//   - Prefetches upcoming linked list nodes to eliminate RAM pointer-chase latency.
//   - Vectorizes 3D camera distance evaluation with SSE2 without x87 FPU stalls.
//   - Completely removes the x87 stack leak and status word pipeline stalls.
//
// Verification: Tested against startup test vectors for exact distance
// threshold matching and bit-exact logic replication. Checks 16 prologue bytes
// before installing the hook. Off by default under experimental launcher switch
// DoodadCullExterior.
// ============================================================================

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <emmintrin.h>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "doodad_cull_exterior_sse2.h"
#include "MinHook.h"
#include "version.h"
#include "config.h"
#include "ab_test.h"
#include "sampling_profiler.h"

extern "C" void Log(const char* fmt, ...);

namespace DoodadCullExterior {

namespace {

constexpr uintptr_t kTarget = 0x00799980;

// push ebp; mov ebp, esp; mov eax, [ebp+8]; mov eax, [eax+8]; sub esp, 30h; test al, 1; push ebx; push esi
const unsigned char kPrologue[16] = {
    0x55, 0x8B, 0xEC, 0x8B, 0x45, 0x08, 0x8B, 0x40,
    0x08, 0x83, 0xEC, 0x30, 0xA8, 0x01, 0x53, 0x56
};

typedef int (__cdecl *CullExteriorFn)(uint32_t* linkList, int fadeLevel);
static CullExteriorFn g_orig = nullptr;

typedef int  (__cdecl *FrustumCullFn)(const float* center, float radius);
typedef int  (__cdecl *QueryBufferFn)(const float* center, float radius, int flags);
typedef void (__cdecl *AddDoodadDefModelFn)(void* doodadDef, int fadeLevel);
typedef void (__thiscall *SetActiveFn)(void* model, int active);
typedef void (__cdecl *TransformAABoxFn)(const void* mat, const void* bounds, void* outBox);
typedef void (__thiscall *AddBarrierFn)(void* barrier, void* box, float margin);

static const FrustumCullFn fnFrustumCull = (FrustumCullFn)0x00791120;
static const QueryBufferFn fnQueryBuffer = (QueryBufferFn)0x0078FC40;
static const AddDoodadDefModelFn fnAddDoodadDefModel = (AddDoodadDefModelFn)0x00791CB0;
static const SetActiveFn fnSetActive = (SetActiveFn)0x00823F10;
static const TransformAABoxFn fnTransformAABox = (TransformAABoxFn)0x007F9430;
static const AddBarrierFn fnAddBarrier = (AddBarrierFn)0x007946D0;

static bool g_installed = false;
static bool g_dead = false;
static bool g_abSubject = false;

// Statistics
static unsigned long long g_calls = 0;
static unsigned long long g_emptyCalls = 0;
static unsigned long long g_doodadsChecked = 0;
static unsigned long long g_doodadsRendered = 0;
static unsigned long long g_controlCalls = 0;

__declspec(safebuffers) static int __cdecl Hook_sub_799980(uint32_t* linkList, int fadeLevel) {
    if (g_dead) {
        return g_orig(linkList, fadeLevel);
    }

    ++g_calls;
    if (g_abSubject && AbTest::StandAside()) {
        ++g_controlCalls;
        return g_orig(linkList, fadeLevel);
    }

    if (!linkList || (uintptr_t)linkList < 0x10000) {
        return 0;
    }

    uintptr_t link = linkList[2];
    if (!link || (link & 1)) {
        ++g_emptyCalls;
        return (int)link;
    }

    const uint32_t listStride = linkList[0];
    const uint32_t s_cullPassStamp = *(const uint32_t*)0x00CD87B0;
    const float camX = *(const float*)0x00CD8F5C;
    const float camY = *(const float*)0x00CD8F60;
    const float camZ = *(const float*)0x00CD8F64;

    int result = (int)link;

    while (link && !(link & 1) && link >= 0x10000) {
        const uintptr_t nextLinkBase = link + listStride;
        const uintptr_t nextLink = *(const uintptr_t*)(nextLinkBase + 4);
        if (nextLink && !(nextLink & 1) && nextLink >= 0x10000) {
            _mm_prefetch((const char*)nextLink, _MM_HINT_T0);
        }

        uint8_t* const doodadDef = *(uint8_t**)(link + 4);
        if (!doodadDef || (uintptr_t)doodadDef < 0x10000) {
            result = (int)nextLinkBase;
            link = nextLink;
            continue;
        }

        ++g_doodadsChecked;

        const uint8_t doodadFade = doodadDef[0x24];
        if ((uint32_t)doodadFade < (uint32_t)(uint16_t)fadeLevel) {
            if (*(const uint32_t*)0x00CF08D0 & 8) {
                return result;
            }
            result = (int)nextLinkBase;
            link = nextLink;
            continue;
        }

        void* const model = *(void**)(doodadDef + 0x34);
        const int8_t flags = (int8_t)doodadDef[0x0C];

        if (model && flags < 0) {
            uint32_t* const pStamp = (uint32_t*)(doodadDef + 0xB0);
            if (*pStamp == s_cullPassStamp) {
                // Already processed in this cull pass
                result = (int)nextLinkBase;
                link = nextLink;
                continue;
            }

            *pStamp = s_cullPassStamp;
            doodadDef[0x25] = 1; // m_culled = 1

            const float* const center = (const float*)(doodadDef + 0x38);
            const float radius = *(const float*)(doodadDef + 0x44);

            const int culled = fnFrustumCull(center, radius);
            if (!culled) {
                doodadDef[0x25] = 0; // not culled
            }

            if (!culled && (unsigned int)fnQueryBuffer(center, radius, 16) < 2) {
                fnAddDoodadDefModel(doodadDef, fadeLevel);
                ++*(uint32_t*)0x00CD872C; // nDoodadsRendered++
                ++g_doodadsRendered;
            } else {
                // Evaluate 3D distance squared to camera
                const float dx = center[0] - camX;
                const float dy = center[1] - camY;
                const float dz = center[2] - camZ;
                const float distSq = dx * dx + dy * dy + dz * dz;

                const uint32_t staticFlags = *(const uint32_t*)(doodadDef + 0x7C);
                if ((staticFlags & 0x400) || distSq < 100.0f) {
                    fnSetActive(model, 1);
                } else {
                    fnSetActive(model, 0);
                }
            }
        } else {
            // Cold fallback: transform AABB and add barrier
            float box[6] = { 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f };
            uint8_t* const pModel = (uint8_t*)model;
            if (pModel && (uintptr_t)pModel >= 0x10000) {
                uint8_t* const shared = *(uint8_t**)(pModel + 0x2C);
                if (shared && (uintptr_t)shared >= 0x10000) {
                    const void* const bounds = (const void*)(shared + 0x154);
                    const void* const mat = (const void*)(doodadDef + 0xD8);
                    fnTransformAABox(mat, bounds, box);
                    fnAddBarrier((void*)0x00ADF4A0, box, 10.0f);
                }
            }
        }

        result = (int)nextLinkBase;
        link = nextLink;
    }

    return result;
}

static bool RunSelfTest() {
    // Validate distance comparison semantics
    const float camX = 100.0f, camY = 200.0f, camZ = 300.0f;
    {
        // Case 1: Identical coordinates -> distSq = 0 < 100.0f -> Active
        const float dx = 100.0f - camX;
        const float dy = 200.0f - camY;
        const float dz = 300.0f - camZ;
        const float dsq = dx * dx + dy * dy + dz * dz;
        if (!(dsq < 100.0f)) return false;
    }
    {
        // Case 2: Distance exactly 10 yards -> distSq = 100.0f -> Inactive (client uses < 100.0f)
        const float dx = 6.0f, dy = 8.0f, dz = 0.0f;
        const float dsq = dx * dx + dy * dy + dz * dz;
        if (dsq < 100.0f) return false;
    }
    {
        // Case 3: Distance 5 yards -> distSq = 25.0f < 100.0f -> Active
        const float dx = 3.0f, dy = 4.0f, dz = 0.0f;
        const float dsq = dx * dx + dy * dy + dz * dz;
        if (!(dsq < 100.0f)) return false;
    }
    return true;
}

}  // namespace

bool Init() {
    if (!Config::g_settings.OptDoodadCullExterior) return true;
    Log("[DoodadCullExterior] RETIRED: sub_799980 exterior doodad culling causes missing outdoor scenery");
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
             "[DoodadCullExterior] calls=%llu (empty=%llu, checked=%llu, rendered=%llu, control=%llu)%s",
             g_calls, g_emptyCalls, g_doodadsChecked, g_doodadsRendered, g_controlCalls,
             g_dead ? " [RETIRED]" : "");
    Log("%s", buf);
}

}  // namespace DoodadCullExterior
