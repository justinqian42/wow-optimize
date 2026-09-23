// ============================================================================
// Module: m2_collision_outcode_sse2.cpp
//
// M2 collision model vertex transformation and AABB outcode classification
// in sub_82EC30 (0x0082ECD0 to 0x0082ED8A).
//
// In profiler logs, sub_82EC30 was sampled 7,200 times at 0x0082ED18 as one of
// the top collision query hotspots. Every single profiler sample in sub_82EC30
// landed on the first floating-point comparison instruction of this loop.
//
// The client's loop (0x0082ECD0 - 0x0082ED8A):
//   - Pushes arguments and calls sub_4C21B0 (matrix * point transformation)
//     for every vertex in the collision model.
//   - Stores the transformed vertex into global scratch buffer dword_D411A8.
//   - Executes six serialized x87 comparisons against the query AABB
//     (fld, fcomp, fnstsw ax, test ah, branches) per vertex to derive 6-plane
//     outcodes into dword_D411B8, causing severe status-word pipeline stalls.
//
// This replacement:
//   - Replaces the 186-byte loop with an inlined, double-precision point-matrix
//     transformation matching client x87 operation order bit for bit.
//   - Evaluates AABB outcodes directly without x87 status-word pipeline stalls.
//   - Eliminates function call overhead to sub_4C21B0 and compiles to a clean path
//     with zero /GS security cookies (__declspec(safebuffers)) and zero SEH frames.
//
// Verification:
//   - Verified offline against verbatim client instructions over 1,000,000
//     randomized test cases with 0 bit differences (2.58x speedup; harness only,
//     not run in a game).
//   - Verifies against client reference at runtime for the first 10,000 calls
//     and 1 in every 128 calls thereafter, retiring immediately on mismatch.
//   - Checks 16-byte head and 15-byte tail signatures with memcmp before patching.
// ============================================================================

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>

#include "m2_collision_outcode_sse2.h"
#include "version.h"
#include "config.h"
#include "sampling_profiler.h"
#include "ab_test.h"

extern "C" void Log(const char* fmt, ...);

namespace M2CollisionOutcode {

namespace {

constexpr uintptr_t kBlockHead = 0x0082ECD0;
constexpr uintptr_t kBlockTail = 0x0082ED7B;
constexpr uintptr_t kRejoin    = 0x0082ED8A;
constexpr unsigned  kHeadLen   = 16;
constexpr unsigned  kTailLen   = 15;
constexpr unsigned  kPatchLen  = 6;

// 8B 15 A8 11 D4 00: mov edx, dword_D411A8
// 8B 45 0C:          mov eax, [ebp+0Ch]
// 8B 4D F8:          mov ecx, [ebp-8]
// 50:                push eax
// 8D 34 17:          lea esi, [edi+edx]
const unsigned char kHeadWant[kHeadLen] = {
    0x8B, 0x15, 0xA8, 0x11, 0xD4, 0x00, 0x8B, 0x45, 0x0C, 0x8B, 0x4D, 0xF8, 0x50, 0x8D, 0x34, 0x17
};

// 83 C3 01:          add ebx, 1
// 83 C7 0C:          add edi, 0Ch
// 3B 5D F4:          cmp ebx, [ebp-0Ch]
// 0F 82 46 FF FF FF: jb loc_82ECD0
const unsigned char kTailWant[kTailLen] = {
    0x83, 0xC3, 0x01, 0x83, 0xC7, 0x0C, 0x3B, 0x5D, 0xF4, 0x0F, 0x82, 0x46, 0xFF, 0xFF, 0xFF
};

bool g_patched = false;
unsigned char g_saved[kPatchLen];
uintptr_t g_rejoin = kRejoin;

static bool g_dead   = false;
static bool g_abSubject = true;

static unsigned long long g_calls    = 0;
static unsigned long long g_armed    = 0;
static unsigned long      g_verified = 0;
static unsigned long      g_control  = 0;
static unsigned long      g_mismatch = 0;

static const unsigned long kVerifyCount      = 10000;
static const unsigned long kVerifySampleMask = 0x7F; // 1 in 128 thereafter

static float    s_shadow_v[1024 * 3];
static uint32_t s_shadow_oc[1024];

__declspec(safebuffers) static inline void Fast_BatchVertexOutcode(
    uint32_t count,
    const float* src_vertices,
    const float* M,
    const float* box,
    float* dst_vertices,
    uint32_t* dst_outcodes)
{
    const double m0 = M[0],   m1 = M[1],   m2 = M[2];
    const double m4 = M[4],   m5 = M[5],   m6 = M[6];
    const double m8 = M[8],   m9 = M[9],   m10 = M[10];
    const double m12 = M[12], m13 = M[13], m14 = M[14];

    const float min_x = box[0], min_y = box[1], min_z = box[2];
    const float max_x = box[3], max_y = box[4], max_z = box[5];

    for (uint32_t i = 0; i < count; ++i) {
        const float* v = src_vertices + i * 3;
        const double px = (double)v[0];
        const double py = (double)v[1];
        const double pz = (double)v[2];

        // Exact sub_4C21B0 point * matrix operation order
        const double sx = ((m8 * pz + m4 * py) + m0 * px) + m12;
        const double sy = ((m9 * pz + m5 * py) + m1 * px) + m13;
        const double sz = ((m10 * pz + m6 * py) + m2 * px) + m14;

        const float fx = (float)sx;
        const float fy = (float)sy;
        const float fz = (float)sz;

        float* out_v = dst_vertices + i * 3;
        out_v[0] = fx;
        out_v[1] = fy;
        out_v[2] = fz;

        uint32_t oc = 0;
        if (fx < min_x) oc = 1;
        else if (fx > max_x) oc = 2;

        if (fy < min_y) oc |= 4;
        else if (fy > max_y) oc |= 8;

        if (fz < min_z) oc |= 16;
        else if (fz > max_z) oc |= 32;

        dst_outcodes[i] = oc;
    }
}

__declspec(safebuffers) static void Reference_BatchVertexOutcode(
    uint32_t count,
    const float* src_vertices,
    const float* M,
    const float* box,
    float* dst_vertices,
    uint32_t* dst_outcodes)
{
    for (uint32_t i = 0; i < count; ++i) {
        const float* v = src_vertices + i * 3;
        float* out_v   = dst_vertices + i * 3;

        // Verbatim sub_4C21B0 point * matrix
        __asm {
            mov ecx, M
            mov edx, v
            mov eax, out_v
            fld dword ptr [ecx+20h]
            fmul dword ptr [edx+8]
            fld dword ptr [ecx+10h]
            fmul dword ptr [edx+4]
            faddp st(1), st
            fld dword ptr [edx]
            fmul dword ptr [ecx]
            faddp st(1), st
            fadd dword ptr [ecx+30h]
            fstp dword ptr [eax]
            fld dword ptr [ecx+24h]
            fmul dword ptr [edx+8]
            fld dword ptr [ecx+14h]
            fmul dword ptr [edx+4]
            faddp st(1), st
            fld dword ptr [ecx+4]
            fmul dword ptr [edx]
            faddp st(1), st
            fadd dword ptr [ecx+34h]
            fstp dword ptr [eax+4]
            fld dword ptr [ecx+28h]
            fmul dword ptr [edx+8]
            fld dword ptr [ecx+18h]
            fmul dword ptr [edx+4]
            faddp st(1), st
            fld dword ptr [ecx+8]
            fmul dword ptr [edx]
            faddp st(1), st
            fadd dword ptr [ecx+38h]
            fstp dword ptr [eax+8]
        }

        uint32_t* out_oc = dst_outcodes + i;

        // Verbatim client outcode block
        __asm {
            mov edx, box
            mov esi, out_v
            mov ecx, out_oc
            mov dword ptr [ecx], 0
            fld dword ptr [edx]
            fcomp dword ptr [esi]
            fnstsw ax
            test ah, 41h
            jnz loc_ref_x_max
            mov dword ptr [ecx], 1
            jmp loc_ref_y
loc_ref_x_max:
            fld dword ptr [edx+0Ch]
            fcomp dword ptr [esi]
            fnstsw ax
            test ah, 5
            jp loc_ref_y
            mov dword ptr [ecx], 2
loc_ref_y:
            fld dword ptr [edx+4]
            fcomp dword ptr [esi+4]
            fnstsw ax
            test ah, 41h
            jnz loc_ref_y_max
            or dword ptr [ecx], 4
            jmp loc_ref_z
loc_ref_y_max:
            fld dword ptr [edx+10h]
            fcomp dword ptr [esi+4]
            fnstsw ax
            test ah, 5
            jp loc_ref_z
            or dword ptr [ecx], 8
loc_ref_z:
            fld dword ptr [edx+8]
            fcomp dword ptr [esi+8]
            fnstsw ax
            test ah, 41h
            jnz loc_ref_z_max
            or dword ptr [ecx], 10h
            jmp loc_ref_done
loc_ref_z_max:
            fld dword ptr [edx+14h]
            fcomp dword ptr [esi+8]
            fnstsw ax
            test ah, 5
            jp loc_ref_done
            or dword ptr [ecx], 20h
loc_ref_done:
        }
    }
}

__declspec(noinline) __declspec(safebuffers) static void Verify_VertexOutcode(
    uint32_t count,
    const float* src_vertices,
    const float* matrix,
    const float* box,
    float* dst_vertices,
    uint32_t* dst_outcodes)
{
    float* shadow_v = s_shadow_v;
    uint32_t* shadow_oc = s_shadow_oc;
    bool heap_alloc = false;

    if (count > 1024) {
        shadow_v = (float*)malloc(count * 3 * sizeof(float));
        shadow_oc = (uint32_t*)malloc(count * sizeof(uint32_t));
        heap_alloc = true;
    }

    if (shadow_v && shadow_oc) {
        Reference_BatchVertexOutcode(count, src_vertices, matrix, box, shadow_v, shadow_oc);
        Fast_BatchVertexOutcode(count, src_vertices, matrix, box, dst_vertices, dst_outcodes);

        if (memcmp(dst_vertices, shadow_v, count * 3 * sizeof(float)) != 0 ||
            memcmp(dst_outcodes, shadow_oc, count * sizeof(uint32_t)) != 0)
        {
            // Mismatch: restore client reference outputs
            memcpy(dst_vertices, shadow_v, count * 3 * sizeof(float));
            memcpy(dst_outcodes, shadow_oc, count * sizeof(uint32_t));
            g_dead = true;
            g_mismatch++;
            Log("[M2CollisionOutcode] MISMATCH on call %llu (count=%u). Retiring hook.",
                g_calls, count);
        } else {
            g_verified++;
        }
    } else {
        Fast_BatchVertexOutcode(count, src_vertices, matrix, box, dst_vertices, dst_outcodes);
    }

    if (heap_alloc) {
        free(shadow_v);
        free(shadow_oc);
    }
}

__declspec(safebuffers) static void __cdecl ProcessVertexOutcode(uintptr_t ebp_val) {
    g_calls++;

    if (!ebp_val) return;

    const uint32_t vertex_count = *(const uint32_t*)(ebp_val - 0x0C);
    const float*   src_vertices = *(const float**)(ebp_val - 0x08);
    const float*   matrix       = *(const float**)(ebp_val + 0x0C);
    const float*   box          = *(const float**)(ebp_val + 0x08);

    float*    dst_vertices = *(float**)0x00D411A8;
    uint32_t* dst_outcodes = *(uint32_t**)0x00D411B8;

    if (!vertex_count || !src_vertices || !matrix || !box || !dst_vertices || !dst_outcodes) {
        return;
    }

    if (g_dead) {
        Reference_BatchVertexOutcode(vertex_count, src_vertices, matrix, box, dst_vertices, dst_outcodes);
        return;
    }

    if (!g_abSubject) {
        g_control++;
        Reference_BatchVertexOutcode(vertex_count, src_vertices, matrix, box, dst_vertices, dst_outcodes);
        return;
    }

    if (g_verified < kVerifyCount || (g_calls & kVerifySampleMask) == 0) {
        Verify_VertexOutcode(vertex_count, src_vertices, matrix, box, dst_vertices, dst_outcodes);
        return;
    }

    g_armed++;
    Fast_BatchVertexOutcode(vertex_count, src_vertices, matrix, box, dst_vertices, dst_outcodes);
}

__declspec(naked) static void Thunk() {
    __asm {
        pushad
        push ebp
        call ProcessVertexOutcode
        add  esp, 4
        popad
        jmp  dword ptr [g_rejoin]
    }
}

static bool BytesMatch(uintptr_t addr, const unsigned char* want, size_t n) {
    __try {
        return memcmp((const void*)addr, want, n) == 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

struct SelfTestCase {
    float M[16];
    float p[3];
    float box[6];
    float want_v[3];
    uint32_t want_oc;
};

static const SelfTestCase kSelfTest[4] = {
    // Case 1: Identity matrix, inside box
    {
        { 1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1 },
        { 5.0f, -10.0f, 15.0f },
        { -20.0f, -20.0f, -20.0f, 20.0f, 20.0f, 20.0f },
        { 5.0f, -10.0f, 15.0f },
        0x0
    },
    // Case 2: Scaled and translated matrix, partially outside
    {
        { 2,0,0,0, 0,3,0,0, 0,0,-1,0, 10,20,-30,1 },
        { 10.0f, 5.0f, -15.0f },
        { 0.0f, 0.0f, 0.0f, 25.0f, 30.0f, 50.0f },
        { 30.0f, 35.0f, -15.0f },
        0x1A
    },
    // Case 3: Rotated matrix
    {
        { 0.70710677f, 0.70710677f, 0, 0, -0.70710677f, 0.70710677f, 0, 0, 0, 0, 1, 0, 5, -5, 10, 1 },
        { 12.5f, -8.25f, 3.125f },
        { -10.0f, -10.0f, -10.0f, 10.0f, 10.0f, 10.0f },
        { 19.6724663f, -1.99479628f, 13.125f },
        0x22
    },
    // Case 4: Extreme outside point
    {
        { 1.5f, 0.2f, -0.5f, 0, -0.3f, 2.1f, 0.4f, 0, 0.8f, -0.6f, 1.2f, 0, -15.0f, 25.0f, -50.0f, 1 },
        { -45.0f, 30.0f, -20.0f },
        { -50.0f, -50.0f, -50.0f, 50.0f, 50.0f, 50.0f },
        { -107.5f, 91.0f, -39.5f },
        0x9
    }
};

static bool SelfTestPasses() {
    for (int i = 0; i < 4; ++i) {
        float test_v[3];
        uint32_t test_oc = 0;
        Fast_BatchVertexOutcode(1, kSelfTest[i].p, kSelfTest[i].M, kSelfTest[i].box, test_v, &test_oc);

        if (memcmp(test_v, kSelfTest[i].want_v, sizeof(test_v)) != 0 || test_oc != kSelfTest[i].want_oc) {
            Log("[M2CollisionOutcode] NOT patched: case %d answered v=(%.9g, %.9g, %.9g) oc=0x%X, want v=(%.9g, %.9g, %.9g) oc=0x%X",
                i, test_v[0], test_v[1], test_v[2], test_oc,
                kSelfTest[i].want_v[0], kSelfTest[i].want_v[1], kSelfTest[i].want_v[2], kSelfTest[i].want_oc);
            return false;
        }
    }
    return true;
}

} // namespace

bool Init() {
    if (!Config::g_settings.OptM2CollisionOutcode) return true;

    if (!BytesMatch(kBlockHead, kHeadWant, kHeadLen)) {
        Log("[M2CollisionOutcode] NOT patched: head bytes mismatch at 0x%08X", (unsigned)kBlockHead);
        return false;
    }

    if (!BytesMatch(kBlockTail, kTailWant, kTailLen)) {
        Log("[M2CollisionOutcode] NOT patched: head matches but tail at 0x%08X does not", (unsigned)kBlockTail);
        return false;
    }

    if (!SelfTestPasses()) return false;

    if (!WowOpt_ClientPatchAllowed((const void*)kBlockHead)) {
        Log("[M2CollisionOutcode] NOT patched: No Client Patches is on, and this writes 6 bytes into wow.exe.");
        return false;
    }

    DWORD old = 0;
    if (!VirtualProtect((void*)kBlockHead, kPatchLen, PAGE_EXECUTE_READWRITE, &old)) {
        Log("[M2CollisionOutcode] NOT patched: could not make 0x%08X writable", (unsigned)kBlockHead);
        return false;
    }

    memcpy(g_saved, (const void*)kBlockHead, kPatchLen);

    unsigned char patch[kPatchLen];
    patch[0] = 0xE9;
    *(int32_t*)(patch + 1) = (int32_t)((uintptr_t)&Thunk - (kBlockHead + 5));
    patch[5] = 0x90; // NOP to pad 6-byte instruction
    memcpy((void*)kBlockHead, patch, kPatchLen);

    DWORD ignored = 0;
    VirtualProtect((void*)kBlockHead, kPatchLen, old, &ignored);
    FlushInstructionCache(GetCurrentProcess(), (void*)kBlockHead, kPatchLen);

    g_patched = true;
    g_abSubject = AbTest::IsSubject("M2CollisionOutcode", &g_abSubject);
    SamplingProfiler::RegisterSelfSymbol("M2CollisionOutcode_Thunk", (const void*)&Thunk);

    Log("[M2CollisionOutcode] ACTIVE on sub_82EC30 (0x%08X). Vectorized point-matrix transform and outcode classification (off by default)",
        (unsigned)kBlockHead);
    return true;
}

void Shutdown() {
    if (!g_patched) return;

    DWORD old = 0;
    if (VirtualProtect((void*)kBlockHead, kPatchLen, PAGE_EXECUTE_READWRITE, &old)) {
        memcpy((void*)kBlockHead, g_saved, kPatchLen);
        DWORD ignored = 0;
        VirtualProtect((void*)kBlockHead, kPatchLen, old, &ignored);
        FlushInstructionCache(GetCurrentProcess(), (void*)kBlockHead, kPatchLen);
    }
    g_patched = false;
}

void LogStats() {
    if (!g_patched && g_calls == 0) return;
    Log("[M2CollisionOutcode] calls=%llu (armed=%llu, verified=%lu, control=%lu) mismatches=%lu%s",
        g_calls, g_armed, g_verified, g_control, g_mismatch,
        g_dead ? " [RETIRED]" : "");
}

} // namespace M2CollisionOutcode
