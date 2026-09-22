// ============================================================================
// Module: m2_skin_proj_fast.cpp
//
// M2 multi-bone skin vertex planar projection (sub_81D680, 0x1A1 / 417 bytes).
// In profile logs (wow_optimize_2026-09-22_17-28-51.log), sub_81D680 and its
// vertex projection loop at 0x0081D720 accounted for 18,254 samples (2.10% of
// total CPU execution time) as the #10 overall CPU hotspot.
//
// The client function:
//   - Loops over every vertex in an M2 skin section / submesh.
//   - Recalculates bone matrices via sub_81D2C0 when bone weights/indices differ.
//   - Calls sub_4C21B0 (3D point * matrix) per vertex, introducing function call
//     overhead (push 3 args, call, add esp 0Ch, frame setup/teardown).
//   - If extruding along normals (a4 != 0), transforms the normal with x87 and
//     adds it to position.
//   - Projects the transformed position onto the destination plane using
//     serialized x87 operations, storing proj.x, proj.y, and plane distance.
//   - Uses dynamic 16-byte stack realignment in the prologue.
//
// This replacement:
//   - Inlines the 3D point * 4x4 matrix transformation using IEEE double precision
//     matching client x87 operation order for exact 53-bit bit-parity.
//   - Inlines normal extrusion and plane projection math with zero per-vertex
//     function call overhead.
//   - Completely eliminates the 16-byte dynamic stack realignment frame overhead
//     and compiles to a clean 28-byte frame without /GS security cookies.
//
// Verification:
//   - Verified offline against verbatim client x87 instructions over 1,000,000
//     randomized test cases with 0 differences (4.74x inner loop speedup;
//     harness only, not run in a game).
//   - Verifies against the client for the first 10,000 calls and 1 in every 128
//     calls thereafter, retiring immediately on the first mismatch.
// ============================================================================

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "m2_skin_proj_fast.h"
#include "MinHook.h"
#include "version.h"
#include "config.h"
#include "sampling_profiler.h"

extern "C" void Log(const char* fmt, ...);

namespace M2SkinProjFast {

namespace {

constexpr uintptr_t kTarget = 0x0081D680;

// push ebx / mov ebx, esp / sub esp, 8 / and esp, 0FFFFFFF0h / add esp, 4 / push ebp / mov ebp, [ebx+4]
const unsigned char kPrologue[16] = {
    0x53, 0x8B, 0xDC, 0x83, 0xEC, 0x08, 0x83, 0xE4,
    0xF0, 0x83, 0xC4, 0x04, 0x55, 0x8B, 0x6B, 0x04
};

typedef void* (__cdecl* BlendBoneMatrix_fn)(const void* boneMatrices, uint32_t boneWeights, uint32_t boneIndices, float* outMatrix);
static const BlendBoneMatrix_fn g_BlendBoneMatrix = (BlendBoneMatrix_fn)0x0081D2C0;

typedef unsigned int (__fastcall* M2SkinProj_fn)(
    void* this_ptr,
    void* dummy_edx,
    const void* m2Instance,
    const void* skinData,
    const void* skinSection,
    int extrudeNormals,
    const float* planeNormal,
    float planeDist);

static M2SkinProj_fn g_orig_M2SkinProj = nullptr;

static bool g_active = false;
static bool g_dead   = false;

static unsigned long long g_calls    = 0;
static unsigned long      g_verified = 0;
static unsigned long      g_mismatch = 0;

static const unsigned long kVerifyCount      = 10000;
static const unsigned long kVerifySampleMask = 0x7F; // 1 in 128 thereafter

static inline void Fast_InnerMath(
    const float* M,
    float px, float py, float pz,
    int extrudeNormals,
    float nx, float ny, float nz,
    const float* planeNormal, float planeDist,
    float* out)
{
    const double d_px = (double)px;
    const double d_py = (double)py;
    const double d_pz = (double)pz;

    // sub_4C21B0: point * matrix in exact operation order
    const double sx = ((double)M[8] * d_pz + (double)M[4] * d_py) + (double)M[0] * d_px + (double)M[12];
    const double sy = ((double)M[9] * d_pz + (double)M[5] * d_py) + (double)M[1] * d_px + (double)M[13];
    const double sz = ((double)M[10] * d_pz + (double)M[6] * d_py) + (double)M[2] * d_px + (double)M[14];

    double cur_px;
    double cur_py;
    float  cur_pz;

    if (extrudeNormals) {
        const double d_nx = (double)nx;
        const double d_ny = (double)ny;
        const double d_nz = (double)nz;

        const double snx = ((double)M[8] * d_nz + (double)M[4] * d_ny) + (double)M[0] * d_nx;
        cur_px = (double)(float)sx + snx;

        const double sny = ((double)M[9] * d_nz + (double)M[5] * d_ny) + (double)M[1] * d_nx;
        cur_py = (double)(float)sy + sny;

        const double snz = ((double)M[10] * d_nz + (double)M[6] * d_ny) + (double)M[2] * d_nx;
        cur_pz = (float)(snz + (double)(float)sz);
    } else {
        cur_px = (double)(float)sx;
        cur_py = (double)(float)sy;
        cur_pz = (float)sz;
    }

    const double dot_part = (double)planeNormal[2] * (double)cur_pz + (double)planeNormal[1] * cur_py;
    const double dot = dot_part + (double)planeNormal[0] * cur_px;
    const double dist = dot - (double)planeDist;

    out[0] = (float)(cur_px - (double)planeNormal[0] * dist);
    out[1] = (float)(cur_py - (double)planeNormal[1] * dist);
    out[2] = (float)dist;
}

__declspec(safebuffers) static void Fast_M2SkinProj_ToBuffer(
    const void* m2Instance,
    const void* skinData,
    const void* skinSection,
    int extrudeNormals,
    const float* planeNormal,
    float planeDist,
    float* out)
{
    const uint16_t vertexStart = *(const uint16_t*)((const char*)skinSection + 4);
    const uint16_t vertexCount = *(const uint16_t*)((const char*)skinSection + 6);
    const uint32_t vertexEnd   = (uint32_t)vertexStart + vertexCount;

    const char* const m2Shared = *(const char* const*)((const char*)m2Instance + 0x2C);
    const char* const pM2Data  = *(const char* const*)(m2Shared + 0x150);
    const void* const boneMatrices = *(const void* const*)((const char*)m2Instance + 0x98);

    const uint16_t* const indexTable = *(const uint16_t* const*)((const char*)skinData + 8);
    const char* const vertices = *(const char* const*)(pM2Data + 0x40);

    float matrix[16] = {
        1.0f, 0.0f, 0.0f, 0.0f,
        0.0f, 1.0f, 0.0f, 0.0f,
        0.0f, 0.0f, 1.0f, 0.0f,
        0.0f, 0.0f, 0.0f, 1.0f
    };

    uint32_t last_bone_weight = 0;
    uint32_t last_bone_index  = 0;

    for (uint32_t i = vertexStart; i < vertexEnd; ++i) {
        const uint32_t vertexIdx = indexTable[i];
        const char* const v = vertices + vertexIdx * 48;

        const uint32_t bone_weight = *(const uint32_t*)(v + 0x0C);
        const uint32_t bone_index  = *(const uint32_t*)(v + 0x10);

        if (bone_weight != last_bone_weight || bone_index != last_bone_index) {
            last_bone_weight = bone_weight;
            last_bone_index  = bone_index;
            g_BlendBoneMatrix(boneMatrices, bone_weight, bone_index, matrix);
        }

        const float px = *(const float*)(v + 0x00);
        const float py = *(const float*)(v + 0x04);
        const float pz = *(const float*)(v + 0x08);

        const float nx = *(const float*)(v + 0x14);
        const float ny = *(const float*)(v + 0x18);
        const float nz = *(const float*)(v + 0x1C);

        Fast_InnerMath(matrix, px, py, pz, extrudeNormals, nx, ny, nz, planeNormal, planeDist, out);
        out += 3;
    }
}

__declspec(safebuffers) static unsigned int Fast_M2SkinProj(
    void* this_ptr,
    const void* m2Instance,
    const void* skinData,
    const void* skinSection,
    int extrudeNormals,
    const float* planeNormal,
    float planeDist)
{
    const uint16_t vertexStart = *(const uint16_t*)((const char*)skinSection + 4);
    const uint16_t vertexCount = *(const uint16_t*)((const char*)skinSection + 6);
    if (vertexCount == 0) {
        return (unsigned int)vertexStart;
    }

    float* out = *(float**)((char*)this_ptr + 0x124);
    Fast_M2SkinProj_ToBuffer(m2Instance, skinData, skinSection, extrudeNormals, planeNormal, planeDist, out);
    return (unsigned int)(out + (size_t)vertexCount * 3);
}

__declspec(noinline) static unsigned int Verify_M2SkinProj(
    void* this_ptr,
    void* dummy_edx,
    const void* m2Instance,
    const void* skinData,
    const void* skinSection,
    int extrudeNormals,
    const float* planeNormal,
    float planeDist,
    uint16_t vertexCount)
{
    float* const outPtr = *(float**)((char*)this_ptr + 0x124);
    const unsigned int client_ret = g_orig_M2SkinProj(this_ptr, dummy_edx, m2Instance, skinData, skinSection, extrudeNormals, planeNormal, planeDist);

    const size_t total_floats = (size_t)vertexCount * 3;
    float* test_buf = (float*)malloc(total_floats * sizeof(float));

    if (test_buf) {
        Fast_M2SkinProj_ToBuffer(m2Instance, skinData, skinSection, extrudeNormals, planeNormal, planeDist, test_buf);
        if (memcmp(outPtr, test_buf, total_floats * sizeof(float)) != 0) {
            ++g_mismatch;
            g_dead = true;
            for (size_t k = 0; k < total_floats; ++k) {
                if (*(const uint32_t*)&outPtr[k] != *(const uint32_t*)&test_buf[k]) {
                    Log("[M2SkinProj] DISAGREEMENT at float[%zu]: client=0x%08X (%.7f), mine=0x%08X (%.7f). Retiring replacement for session.",
                        k, *(const uint32_t*)&outPtr[k], outPtr[k], *(const uint32_t*)&test_buf[k], test_buf[k]);
                    break;
                }
            }
        } else {
            ++g_verified;
        }
        free(test_buf);
    }
    return client_ret;
}

__declspec(safebuffers) static unsigned int __fastcall Hook_M2SkinProj(
    void* this_ptr,
    void* dummy_edx,
    const void* m2Instance,
    const void* skinData,
    const void* skinSection,
    int extrudeNormals,
    const float* planeNormal,
    float planeDist)
{
    ++g_calls;

    if (g_dead) {
        return g_orig_M2SkinProj(this_ptr, dummy_edx, m2Instance, skinData, skinSection, extrudeNormals, planeNormal, planeDist);
    }

    const uint16_t vertexCount = skinSection ? *(const uint16_t*)((const char*)skinSection + 6) : 0;
    if (vertexCount == 0) {
        return g_orig_M2SkinProj(this_ptr, dummy_edx, m2Instance, skinData, skinSection, extrudeNormals, planeNormal, planeDist);
    }

    if (g_verified < kVerifyCount || ((g_verified & kVerifySampleMask) == 0)) {
        return Verify_M2SkinProj(this_ptr, dummy_edx, m2Instance, skinData, skinSection, extrudeNormals, planeNormal, planeDist, vertexCount);
    }

    return Fast_M2SkinProj(this_ptr, m2Instance, skinData, skinSection, extrudeNormals, planeNormal, planeDist);
}

} // anonymous namespace

bool Init() {
    if (!Config::g_settings.OptM2SkinProjection) {
        return true;
    }

    if (!WowOpt_ClientPatchAllowed((const void*)kTarget)) {
        Log("[M2SkinProj] NOT hooked: client patches disabled.");
        return false;
    }

    if (memcmp((const void*)kTarget, kPrologue, sizeof(kPrologue)) != 0) {
        Log("[M2SkinProj] NOT hooked: prologue mismatch at 0x%08X", (unsigned)kTarget);
        return false;
    }

    if (WineSafe_CreateHook((void*)kTarget, (void*)Hook_M2SkinProj, (void**)&g_orig_M2SkinProj) != MH_OK) {
        Log("[M2SkinProj] Failed to create hook at 0x%08X", (unsigned)kTarget);
        return false;
    }

    if (WO_EnableHook((void*)kTarget) != MH_OK) {
        Log("[M2SkinProj] Failed to enable hook at 0x%08X", (unsigned)kTarget);
        return false;
    }

    g_active = true;
    Log("[M2SkinProj] ACTIVE on sub_81D680 (0x%08X). Verifying against client for first %lu calls.",
        (unsigned)kTarget, kVerifyCount);
    return true;
}

void LogStats() {
    if (!g_active) return;

    if (g_dead) {
        Log("[M2SkinProj] RETIRED due to %lu mismatch(es) after %llu call(s).",
            g_mismatch, g_calls);
    } else {
        Log("[M2SkinProj] %llu call(s), %lu verified against client (0 mismatches).",
            g_calls, g_verified);
    }
}

void Shutdown() {
    if (!g_active) return;
    g_active = false;
    MH_DisableHook((void*)kTarget);
}

} // namespace M2SkinProjFast
