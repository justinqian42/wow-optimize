// ============================================================================
// Module: collision_face_clip_sse2.cpp
//
// Accelerates collision mesh face query and polygon clipping in sub_75C5A0
// (0x14F bytes). In collision profiling, sub_75C5A0 takes 2.19% of executing
// main-thread time during scene collision queries (invoked by camera collision
// sub_75CD70 and character swept movement sub_75F9D0).
//
// For every face in the collision mesh (52 bytes per face), the client computes
// an x87 dot product of the face normal against the ray direction with fld,
// fmul, fadd, fcomp, and an fnstsw ax pipeline stall for backface culling.
// At entry, it also zeroes 180 bytes (15 Vec3 vertices) on the stack using 45
// x87 fldz/fst stores, even though only active vertices (poly.count) are read.
//
// This replacement:
//   1. Returns immediately if !mesh or numFaces == 0.
//   2. Eliminates redundant 180-byte stack zeroing on entry.
//   3. Evaluates backface culling dot products in IEEE double precision in
//      exact client 53-bit x87 operation order, eliminating x87 FPU state
//      switches and status-word pipeline stalls.
//   4. Copies candidate front-facing vertices and delegates polygon clipping
//      to sub_75B710 and triangle intersection to sub_75C0B0 via zero-overhead
//      thunks.
//
// Verification: Verified offline against verbatim client instructions over
// 1,000,000 cases with 0 bit differences (harness only, not run in a game).
// Runtime verification tests the first 10,000 calls and 1 in every 128
// thereafter, retiring immediately on the first mismatch.
// ============================================================================

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "collision_face_clip_sse2.h"
#include "MinHook.h"
#include "version.h"
#include "config.h"
#include "ab_test.h"
#include "sampling_profiler.h"

extern "C" void Log(const char* fmt, ...);
MH_STATUS WineSafe_CreateHook(void* target, void* detour, void** original);
MH_STATUS WO_EnableHook(void* target);

namespace CollisionFaceClip {

namespace {

constexpr uintptr_t kTarget = 0x0075C5A0;
constexpr uintptr_t kSub75B710 = 0x0075B710;
constexpr uintptr_t kSub75C0B0 = 0x0075C0B0;
constexpr uintptr_t kThreshAddr = 0x00A34EEC; // -0.0000099999997f

// push ebp / mov ebp, esp / sub esp, 108h / fldz / xor edx, edx / mov [ebp-10h], edx
const unsigned char kPrologue[16] = {
    0x55, 0x8B, 0xEC, 0x81, 0xEC, 0x08, 0x01, 0x00,
    0x00, 0xD9, 0xEE, 0x33, 0xD2, 0x89, 0x55, 0xF0
};

#pragma pack(push, 1)
struct StackPoly {
    float verts[15][3];       // 0x00 - 0xB3: 15 * 12 = 180 bytes
    int32_t planeIndices[15]; // 0xB4 - 0xEF: 15 * 4 = 60 bytes
    int32_t count;            // 0xF0 - 0xF3: 4 bytes
};
static_assert(sizeof(StackPoly) == 244, "StackPoly size mismatch");

struct Face {
    float normal[3];
    float planeD;
    float v0[3];
    float v1[3];
    float v2[3];
};
static_assert(sizeof(Face) == 52, "Face size mismatch");

struct MeshHeader {
    uint32_t pad00;
    uint32_t numFaces;
    const Face* faces;
};
#pragma pack(pop)

typedef int (__cdecl *Sub75C5A0_fn)(
    const void* mesh,
    const float* rayDir,
    const float* planes,
    unsigned int numPlanes,
    int a5,
    int a6,
    float* bestDist,
    unsigned int* bestFaceIdx);

Sub75C5A0_fn g_orig = nullptr;

bool g_installed = false;
bool g_dead = false;
bool g_abSubject = false;

// Statistics
unsigned long long g_calls = 0;
unsigned long long g_armedCalls = 0;
unsigned long long g_verifiedCalls = 0;
unsigned long long g_controlCalls = 0;
unsigned g_mismatches = 0;

__declspec(safebuffers) static inline void Call_sub_75B710(const float* plane, void* poly, uint32_t planeIdx) {
    __asm {
        push planeIdx
        mov  edx, plane
        mov  esi, poly
        mov  eax, kSub75B710
        call eax
        add  esp, 4
    }
}

__declspec(safebuffers) static inline int Call_sub_75C0B0(void* poly, int a5, int a6, float* dist, const float* rayDir) {
    int result;
    __asm {
        push dist
        push a6
        push a5
        push poly
        mov  edi, rayDir
        mov  eax, kSub75C0B0
        call eax
        add  esp, 16
        mov  result, eax
    }
    return result;
}

__declspec(safebuffers) static inline int FastMeshQuery(
    const void* meshPtr,
    const float* rayDir,
    const float* planes,
    unsigned int numPlanes,
    int a5,
    int a6,
    float* bestDist,
    unsigned int* bestFaceIdx)
{
    const MeshHeader* const mesh = (const MeshHeader*)meshPtr;
    if (!mesh || !mesh->numFaces) return 0;

    const uint32_t numFaces = mesh->numFaces;
    const Face* const faces = mesh->faces;
    if (!faces) return 0;

    const double kThresh = (double)*(const float*)kThreshAddr;

    int hit = 0;
    float curBestDist = *bestDist;
    unsigned int curBestFace = *bestFaceIdx;
    StackPoly poly;

    for (uint32_t i = 0; i < numFaces; ++i) {
        const Face* const face = &faces[i];

        // Operation order matches client 53-bit x87 sequence bit-for-bit:
        // st(0) = normal[2]*rayDir[2]
        // st(0) = normal[1]*rayDir[1] + (normal[2]*rayDir[2])
        // st(0) = normal[0]*rayDir[0] + ((normal[1]*rayDir[1]) + (normal[2]*rayDir[2]))
        const double term_z = (double)face->normal[2] * (double)rayDir[2];
        const double term_y = (double)face->normal[1] * (double)rayDir[1];
        const double sum_yz = term_y + term_z;
        const double term_x = (double)face->normal[0] * (double)rayDir[0];
        const double dot = term_x + sum_yz;

        if (dot > kThresh) {
            continue; // backface culled
        }

        memcpy(poly.verts, face->v0, 36);
        poly.count = 3;
        poly.planeIndices[0] = -1;
        poly.planeIndices[1] = -1;
        poly.planeIndices[2] = -1;

        if (numPlanes > 0) {
            bool culled = false;
            for (unsigned int p = 0; p < numPlanes; ++p) {
                Call_sub_75B710(planes + (size_t)p * 4, &poly, p);
                if (poly.count == 0) {
                    culled = true;
                    break;
                }
            }
            if (culled) continue;
        }

        float dist = 3.4028235e38f;
        if (Call_sub_75C0B0(&poly, a5, a6, &dist, rayDir)) {
            if (dist <= curBestDist) {
                curBestDist = dist;
                curBestFace = i;
                hit = 1;
            }
        }
    }

    if (hit) {
        *bestDist = curBestDist;
        *bestFaceIdx = curBestFace;
    }
    return hit;
}

__declspec(safebuffers) static int __cdecl Hook_sub_75C5A0(
    const void* mesh,
    const float* rayDir,
    const float* planes,
    unsigned int numPlanes,
    int a5,
    int a6,
    float* bestDist,
    unsigned int* bestFaceIdx)
{
    g_calls++;

    if (g_dead) {
        return g_orig(mesh, rayDir, planes, numPlanes, a5, a6, bestDist, bestFaceIdx);
    }

    if (g_abSubject && AbTest::StandAside()) {
        g_controlCalls++;
        return g_orig(mesh, rayDir, planes, numPlanes, a5, a6, bestDist, bestFaceIdx);
    }

    const bool verify = (g_calls <= 10000) || ((g_calls & 0x7F) == 0);
    if (!verify) {
        g_armedCalls++;
        return FastMeshQuery(mesh, rayDir, planes, numPlanes, a5, a6, bestDist, bestFaceIdx);
    }

    g_verifiedCalls++;

    float clientDist = *bestDist;
    unsigned int clientFace = *bestFaceIdx;
    const int clientRes = g_orig(mesh, rayDir, planes, numPlanes, a5, a6, &clientDist, &clientFace);

    float fastDist = *bestDist;
    unsigned int fastFace = *bestFaceIdx;
    const int fastRes = FastMeshQuery(mesh, rayDir, planes, numPlanes, a5, a6, &fastDist, &fastFace);

    if (fastRes != clientRes || memcmp(&fastDist, &clientDist, sizeof(float)) != 0 || fastFace != clientFace) {
        g_mismatches++;
        g_dead = true;
        Log("[CollisionFaceClip] Disagreement: fast=(res=%d, dist=%f, face=%u) client=(res=%d, dist=%f, face=%u). Hook retired.",
            fastRes, fastDist, fastFace, clientRes, clientDist, clientFace);
    }

    *bestDist = clientDist;
    *bestFaceIdx = clientFace;
    return clientRes;
}

}  // namespace

bool Init() {
    if (!Config::g_settings.OptCollisionFaceClip) return true;

    if (memcmp((const void*)kTarget, kPrologue, sizeof(kPrologue)) != 0) {
        Log("[CollisionFaceClip] NOT active: prologue mismatch at 0x%08X", (unsigned)kTarget);
        return false;
    }

    if (!WowOpt_ClientPatchAllowed((const void*)kTarget)) {
        Log("[CollisionFaceClip] NOT active: client patches not allowed");
        return false;
    }

    if (WineSafe_CreateHook((void*)kTarget, (void*)&Hook_sub_75C5A0, (void**)&g_orig) != MH_OK) {
        Log("[CollisionFaceClip] NOT active: CreateHook failed on 0x%08X", (unsigned)kTarget);
        return false;
    }

    if (WO_EnableHook((void*)kTarget) != MH_OK) {
        MH_RemoveHook((void*)kTarget);
        Log("[CollisionFaceClip] NOT active: EnableHook failed on 0x%08X", (unsigned)kTarget);
        return false;
    }

    g_installed = true;
    g_abSubject = AbTest::IsSubject("CollisionFaceClip", &g_abSubject);
    SamplingProfiler::RegisterSelfSymbol("CollisionFaceClip", (const void*)&Hook_sub_75C5A0);
    Log("[CollisionFaceClip] Hook installed on sub_75C5A0 (0x14F bytes)");
    return true;
}

void Shutdown() {
    if (!g_installed) return;
    MH_DisableHook((void*)kTarget);
    MH_RemoveHook((void*)kTarget);
    g_installed = false;
}

void LogStats() {
    if (!Config::g_settings.OptCollisionFaceClip || !g_installed) return;
    Log("[CollisionFaceClip] calls=%llu (armed=%llu, verified=%llu, control=%llu) mismatches=%u%s",
        g_calls, g_armedCalls, g_verifiedCalls, g_controlCalls, g_mismatches,
        g_dead ? " [RETIRED]" : "");
}

}  // namespace CollisionFaceClip
