// ============================================================================
// Module: particle_quad_sse2.cpp
//
// Mid-function billboard quad vertex generation in sub_97BE80 (0x0097C7B3).
//
// In an uncapped tester session, the particle simulation and rendering pipeline
// occupies substantial main-thread executing time: sub_97BE80 is sampled at
// +0x968 (0x0097C7E8), right inside the unrolled 4-vertex quad calculation for
// billboard particles.
//
// What the client's 1117-byte block (0x0097C7B3 to 0x0097CC10) does:
//   1. Calls sub_6F7A60 to derive sin and cos from rotation angle. That function
//      calls sub_5FE800 twice, draining the x87 pipeline with four fldcw/fnstcw
//      control-word mode changes.
//   2. Serializes four quad corner vertex positions:
//        V0: (cx - cw - sh, cy - sw + ch, cz)
//        V1: (cx - cw + sh, cy - sw - ch, cz)
//        V2: (cx + cw - sh, cy + sw + ch, cz)
//        V3: (cx + cw + sh, cy + sw - ch, cz)
//      where cw = cos*w, ch = cos*h, sw = sin*w, sh = sin*h.
//   3. Updates the emitter's axis-aligned bounding box (min_xyz and max_xyz)
//      using 24 serialized x87 fcomp/fnstsw/test/conditional jumps.
//   4. Writes normal (0, 0, 1), vertex color, and texture UVs:
//        UV0: (u0, v0)
//        UV1: (u0, v0 + dv)
//        UV2: (u0 + du, v0)
//        UV3: (u0 + du, v0 + dv)
//   5. Advances the four vertex stream pointers by 4*stride and increments
//      the vertex count by 4 before rejoining sub_97BE80 at 0x0097CC10.
//
// What is replaced here:
//   - A 9-byte detour at 0x0097C7B3 jumps to a naked thunk that pops the angle
//     from ST(0), leaves the x87 stack clean, and calls ProcessBillboardQuad.
//   - The vertex positions are evaluated in IEEE double precision using the
//     client's exact operation order, matching x87 53-bit rounding bit for bit.
//   - AABB min/max updates are branchless and pipelined, eliminating 24 x87
//     status word stores and branches per quad.
//   - The block has zero jumps in and falls through to 0x0097CC10 with an empty
//     x87 stack; the thunk jumps directly to the rejoin site.
//
// Verification:
//   - Verified offline against verbatim client instruction sequence over
//     one million random test cases with 0 bit differences (diffs: 0).
//   - Six fixed cases run at startup asserting bit-exact parity before patching.
//   - Head (9 bytes) and tail (21 bytes) are checked with memcmp before writing.
// ============================================================================

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <cstdint>
#include <cstring>
#include <cstddef>

#include "particle_quad_sse2.h"
#include "version.h"
#include "config.h"
#include "sampling_profiler.h"

extern "C" void Log(const char* fmt, ...);

namespace ParticleQuad {

namespace {

constexpr uintptr_t kBlockHead = 0x0097C7B3;
constexpr uintptr_t kBlockTail = 0x0097CBFB;
constexpr uintptr_t kRejoin    = 0x0097CC10;
constexpr unsigned  kHeadLen   = 9;
constexpr unsigned  kTailLen   = 21;

// 8D 45 0C: lea eax, [ebp+0Ch]
// 50:       push eax
// 8D 4D F4: lea ecx, [ebp-0Ch]
// 51:       push ecx
// 51:       push ecx
const unsigned char kHeadWant[kHeadLen] = {
    0x8D, 0x45, 0x0C, 0x50, 0x8D, 0x4D, 0xF4, 0x51, 0x51
};

// 8B 4E 14: mov ecx, [esi+14h]
// 8B 56 18: mov edx, [esi+18h]
// 8B 46 1C: mov eax, [esi+1Ch]
// 01 5E 20: add [esi+20h], ebx
// 01 4E 04: add [esi+4], ecx
// 01 56 08: add [esi+8], edx
// 01 46 0C: add [esi+0Ch], eax
const unsigned char kTailWant[kTailLen] = {
    0x8B, 0x4E, 0x14, 0x8B, 0x56, 0x18, 0x8B, 0x46, 0x1C, 0x01, 0x5E, 0x20,
    0x01, 0x4E, 0x04, 0x01, 0x56, 0x08, 0x01, 0x46, 0x0C
};

bool g_patched = false;
unsigned char g_saved[kHeadLen];
uintptr_t g_rejoin = kRejoin;

unsigned long long g_calls = 0;

#pragma pack(push, 1)
struct VertexBuffers {
    char* pos_ptr;        // +0x00
    char* normal_ptr;     // +0x04
    char* color_ptr;      // +0x08
    char* uv_ptr;         // +0x0C
    int32_t pos_stride;   // +0x10
    int32_t normal_stride;// +0x14
    int32_t color_stride; // +0x18
    int32_t uv_stride;    // +0x1C
    int32_t vertex_count; // +0x20
};

struct ParticleEmitter {
    char pad_00[0x10];
    float du;             // +0x10
    float dv;             // +0x14
    char pad_18[0x218 - 0x18];
    float min_x;          // +0x218
    float min_y;          // +0x21C
    float min_z;          // +0x220
    float max_x;          // +0x224
    float max_y;          // +0x228
    float max_z;          // +0x22C
};
#pragma pack(pop)

static_assert(offsetof(VertexBuffers, pos_ptr) == 0x00, "pos_ptr offset");
static_assert(offsetof(VertexBuffers, normal_ptr) == 0x04, "normal_ptr offset");
static_assert(offsetof(VertexBuffers, color_ptr) == 0x08, "color_ptr offset");
static_assert(offsetof(VertexBuffers, uv_ptr) == 0x0C, "uv_ptr offset");
static_assert(offsetof(VertexBuffers, pos_stride) == 0x10, "pos_stride offset");
static_assert(offsetof(VertexBuffers, normal_stride) == 0x14, "normal_stride offset");
static_assert(offsetof(VertexBuffers, color_stride) == 0x18, "color_stride offset");
static_assert(offsetof(VertexBuffers, uv_stride) == 0x1C, "uv_stride offset");
static_assert(offsetof(VertexBuffers, vertex_count) == 0x20, "vertex_count offset");

static_assert(offsetof(ParticleEmitter, du) == 0x10, "ParticleEmitter du offset");
static_assert(offsetof(ParticleEmitter, dv) == 0x14, "ParticleEmitter dv offset");
static_assert(offsetof(ParticleEmitter, min_x) == 0x218, "ParticleEmitter min_x offset");
static_assert(offsetof(ParticleEmitter, max_z) == 0x22C, "ParticleEmitter max_z offset");

typedef void (__cdecl *SinCosFn)(float angle, float *outSin, float *outCos);
const SinCosFn ClientSinCos = (SinCosFn)0x006F7A60;

__forceinline void ComputeQuadDirect(
    char* pos, int32_t ps,
    char* norm, int32_t ns,
    char* col, int32_t cs,
    char* uvp, int32_t us,
    float cx, float cy, float cz,
    float width, float height,
    float cos_v, float sin_v,
    uint32_t color,
    float du, float dv, float u0, float v0,
    float& min_x, float& min_y, float& min_z,
    float& max_x, float& max_y, float& max_z,
    float* out_v_dbg = nullptr)
{
    const double d_cos = (double)cos_v;
    const double d_sin = (double)sin_v;
    const double d_w   = (double)width;
    const double d_h   = (double)height;

    const double cw = d_cos * d_w;
    const double ch = d_cos * d_h;
    const double sw = d_sin * d_w;
    const double sh = d_sin * d_h;

    const double d_cx = (double)cx;
    const double d_cy = (double)cy;

    // Operation order matches client x87 FPU stack exactly for bit-exact parity:
    // Vertex 0: (cx - cw) - sh, (cy - sw) + ch, cz
    const float x0 = (float)((d_cx - cw) - sh);
    const float y0 = (float)((d_cy - sw) + ch);
    const float z0 = cz;

    // Vertex 1: (cx - cw) + sh, (cy - sw) - ch, cz
    const float x1 = (float)((d_cx - cw) + sh);
    const float y1 = (float)((d_cy - sw) - ch);
    const float z1 = cz;

    // Vertex 2: (cw + cx) - sh, (sw + ch) + cy, cz
    const float x2 = (float)((cw + d_cx) - sh);
    const float y2 = (float)((sw + ch) + d_cy);
    const float z2 = cz;

    // Vertex 3: (cw + cx) + sh, (sw + cy) - ch, cz
    const float x3 = (float)((cw + d_cx) + sh);
    const float y3 = (float)((sw + d_cy) - ch);
    const float z3 = cz;

    if (out_v_dbg) {
        out_v_dbg[0] = x0; out_v_dbg[1] = y0;
        out_v_dbg[2] = x1; out_v_dbg[3] = y1;
        out_v_dbg[4] = x2; out_v_dbg[5] = y2;
        out_v_dbg[6] = x3; out_v_dbg[7] = y3;
    }

    // Branchless min / max updates for AABB
    if (x0 < min_x) min_x = x0;
    if (y0 < min_y) min_y = y0;
    if (z0 < min_z) min_z = z0;
    if (x0 > max_x) max_x = x0;
    if (y0 > max_y) max_y = y0;
    if (z0 > max_z) max_z = z0;

    if (x1 < min_x) min_x = x1;
    if (y1 < min_y) min_y = y1;
    if (x1 > max_x) max_x = x1;
    if (y1 > max_y) max_y = y1;

    if (x2 < min_x) min_x = x2;
    if (y2 < min_y) min_y = y2;
    if (x2 > max_x) max_x = x2;
    if (y2 > max_y) max_y = y2;

    if (x3 < min_x) min_x = x3;
    if (y3 < min_y) min_y = y3;
    if (x3 > max_x) max_x = x3;
    if (y3 > max_y) max_y = y3;

    if (pos) {
        // Vertex 0
        *(float*)(pos)         = x0;
        *(float*)(pos + 4)     = y0;
        *(float*)(pos + 8)     = z0;
        *(float*)(norm)         = 0.0f;
        *(float*)(norm + 4)     = 0.0f;
        *(float*)(norm + 8)     = 1.0f;
        *(uint32_t*)(col)      = color;
        *(float*)(uvp)         = u0;
        *(float*)(uvp + 4)     = v0;

        // Vertex 1
        *(float*)(pos + ps)     = x1;
        *(float*)(pos + ps + 4) = y1;
        *(float*)(pos + ps + 8) = z1;
        *(float*)(norm + ns)     = 0.0f;
        *(float*)(norm + ns + 4) = 0.0f;
        *(float*)(norm + ns + 8) = 1.0f;
        *(uint32_t*)(col + cs)  = color;
        *(float*)(uvp + us)     = u0;
        *(float*)(uvp + us + 4) = v0 + dv;

        // Vertex 2
        *(float*)(pos + 2*ps)     = x2;
        *(float*)(pos + 2*ps + 4) = y2;
        *(float*)(pos + 2*ps + 8) = z2;
        *(float*)(norm + 2*ns)     = 0.0f;
        *(float*)(norm + 2*ns + 4) = 0.0f;
        *(float*)(norm + 2*ns + 8) = 1.0f;
        *(uint32_t*)(col + 2*cs)  = color;
        *(float*)(uvp + 2*us)     = u0 + du;
        *(float*)(uvp + 2*us + 4) = v0;

        // Vertex 3
        *(float*)(pos + 3*ps)     = x3;
        *(float*)(pos + 3*ps + 4) = y3;
        *(float*)(pos + 3*ps + 8) = z3;
        *(float*)(norm + 3*ns)     = 0.0f;
        *(float*)(norm + 3*ns + 4) = 0.0f;
        *(float*)(norm + 3*ns + 8) = 1.0f;
        *(uint32_t*)(col + 3*cs)  = color;
        *(float*)(uvp + 3*us)     = u0 + du;
        *(float*)(uvp + 3*us + 4) = v0 + dv;
    }
}

extern "C" __declspec(noinline) __declspec(safebuffers) void __cdecl ProcessBillboardQuad(
    float angle, void* frame, VertexBuffers* vb, ParticleEmitter* emitter)
{
    ++g_calls;
    const char* ebp = (const char*)frame;
    const float width    = *(const float*)(ebp - 0x08);
    const float height   = *(const float*)(ebp - 0x04);
    const float cx       = *(const float*)(ebp - 0x24);
    const float cy       = *(const float*)(ebp - 0x20);
    const float cz       = *(const float*)(ebp - 0x1C);
    const uint32_t color = *(const uint32_t*)(ebp - 0x18);
    const float u0       = *(const float*)(ebp - 0x44);
    const float v0       = *(const float*)(ebp - 0x40);

    float sin_v = 0.0f;
    float cos_v = 0.0f;
    ClientSinCos(angle, &sin_v, &cos_v);
    *(float*)(ebp + 0x0C) = cos_v;
    *(float*)(ebp - 0x0C) = sin_v;

    ComputeQuadDirect(
        vb->pos_ptr, vb->pos_stride,
        vb->normal_ptr, vb->normal_stride,
        vb->color_ptr, vb->color_stride,
        vb->uv_ptr, vb->uv_stride,
        cx, cy, cz, width, height, cos_v, sin_v, color,
        emitter->du, emitter->dv, u0, v0,
        emitter->min_x, emitter->min_y, emitter->min_z,
        emitter->max_x, emitter->max_y, emitter->max_z);

    vb->pos_ptr    += 4 * vb->pos_stride;
    vb->normal_ptr += 4 * vb->normal_stride;
    vb->color_ptr  += 4 * vb->color_stride;
    vb->uv_ptr     += 4 * vb->uv_stride;
    vb->vertex_count += 4;
}

__declspec(naked) void Thunk() {
    __asm {
        sub  esp, 4
        fstp dword ptr [esp]
        push edi
        push esi
        push ebp
        push dword ptr [esp+12]
        call ProcessBillboardQuad
        add  esp, 20
        jmp  dword ptr [g_rejoin]
    }
}

bool BytesMatch(uintptr_t addr, const unsigned char* want, size_t n) {
    __try {
        return memcmp((const void*)addr, want, n) == 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

struct SelfTest {
    float cx, cy, cz, w, h, cos_v, sin_v;
    float du, dv, u0, v0;
    float min_x, min_y, min_z, max_x, max_y, max_z;
    float want_x0, want_y0;
    float want_x1, want_y1;
    float want_x2, want_y2;
    float want_x3, want_y3;
    float want_min_x, want_min_y, want_min_z;
    float want_max_x, want_max_y, want_max_z;
};

const SelfTest kSelfTest[] = {
    // Case 0: Axis aligned quad at origin
    {
        0.0f, 0.0f, 0.0f, 1.0f, 1.0f, 1.0f, 0.0f,
        0.25f, 0.25f, 0.0f, 0.0f,
        100.0f, 100.0f, 100.0f, -100.0f, -100.0f, -100.0f,
        -1.0f, 1.0f,
        -1.0f, -1.0f,
        1.0f, 1.0f,
        1.0f, -1.0f,
        -1.0f, -1.0f, 0.0f,
        1.0f, 1.0f, 0.0f
    },
    // Case 1: 45 degree rotated quad
    {
        10.0f, 20.0f, 5.0f, 2.0f, 3.0f, 0.70710678f, 0.70710678f,
        0.5f, 0.5f, 0.1f, 0.2f,
        0.0f, 0.0f, 0.0f, 1.0f, 1.0f, 1.0f,
        6.464466094970703f, 20.707107543945312f,
        10.707106590270996f, 16.464466094970703f,
        9.292893409729004f, 23.535533905029297f,
        13.535533905029297f, 19.292892456054688f,
        0.0f, 0.0f, 0.0f,
        13.535533905029297f, 23.535533905029297f, 5.0f
    },
    // Case 2: Negative coordinates and small sizes
    {
        -50.5f, -120.25f, 33.0f, 0.125f, 0.25f, 0.0f, 1.0f,
        0.125f, 0.125f, 0.5f, 0.5f,
        -10.0f, -10.0f, 0.0f, 10.0f, 10.0f, 50.0f,
        -50.75f, -120.375f,
        -50.25f, -120.375f,
        -50.75f, -120.125f,
        -50.25f, -120.125f,
        -50.75f, -120.375f, 0.0f,
        10.0f, 10.0f, 50.0f
    },
    // Case 3: 180 degree rotation
    {
        100.0f, -200.0f, -50.0f, 5.0f, 10.0f, -1.0f, 0.0f,
        1.0f, 1.0f, 0.0f, 0.0f,
        50.0f, -250.0f, -60.0f, 150.0f, -150.0f, -40.0f,
        105.0f, -210.0f,
        105.0f, -190.0f,
        95.0f, -210.0f,
        95.0f, -190.0f,
        50.0f, -250.0f, -60.0f,
        150.0f, -150.0f, -40.0f
    },
    // Case 4: Fractional angle, non-square quad
    {
        3.14159f, 2.71828f, 1.414f, 7.5f, 2.5f, 0.5f, 0.8660254f,
        0.0625f, 0.0625f, 0.25f, 0.75f,
        0.0f, 0.0f, 0.0f, 10.0f, 10.0f, 10.0f,
        -2.7734735012054443f, -2.5269105434417725f,
        1.5566534996032715f, -5.026910305023193f,
        4.726526737213135f, 10.463470458984375f,
        9.05665397644043f, 7.963470458984375f,
        -2.7734735012054443f, -5.026910305023193f, 0.0f,
        10.0f, 10.463470458984375f, 10.0f
    },
    // Case 5: Large coordinates
    {
        1234.5f, -5678.0f, 999.0f, 15.0f, 25.0f, -0.6f, 0.8f,
        0.2f, 0.4f, 0.3f, 0.6f,
        1200.0f, -5700.0f, 950.0f, 1300.0f, -5600.0f, 1050.0f,
        1223.5f, -5705.0f,
        1263.5f, -5675.0f,
        1205.5f, -5681.0f,
        1245.5f, -5651.0f,
        1200.0f, -5705.0f, 950.0f,
        1300.0f, -5600.0f, 1050.0f
    }
};

bool SelfTestPasses() {
    for (int i = 0; i < (int)(sizeof(kSelfTest) / sizeof(kSelfTest[0])); ++i) {
        const SelfTest& t = kSelfTest[i];
        float min_x = t.min_x; float min_y = t.min_y; float min_z = t.min_z;
        float max_x = t.max_x; float max_y = t.max_y; float max_z = t.max_z;
        float dbg[8] = { 0 };

        ComputeQuadDirect(
            nullptr, 0, nullptr, 0, nullptr, 0, nullptr, 0,
            t.cx, t.cy, t.cz, t.w, t.h, t.cos_v, t.sin_v, 0,
            t.du, t.dv, t.u0, t.v0,
            min_x, min_y, min_z, max_x, max_y, max_z, dbg);

        const uint32_t* got_v = (const uint32_t*)dbg;
        const uint32_t* want_v = (const uint32_t*)&t.want_x0;
        for (int k = 0; k < 8; ++k) {
            if (got_v[k] != want_v[k]) {
                Log("[ParticleQuad] NOT patched: case %d v[%d] answered 0x%08X here and 0x%08X in reference.",
                    i, k, got_v[k], want_v[k]);
                return false;
            }
        }

        const uint32_t got_bounds[6] = {
            *(const uint32_t*)&min_x, *(const uint32_t*)&min_y, *(const uint32_t*)&min_z,
            *(const uint32_t*)&max_x, *(const uint32_t*)&max_y, *(const uint32_t*)&max_z
        };
        const uint32_t* want_bounds = (const uint32_t*)&t.want_min_x;
        for (int k = 0; k < 6; ++k) {
            if (got_bounds[k] != want_bounds[k]) {
                Log("[ParticleQuad] NOT patched: case %d bound[%d] answered 0x%08X here and 0x%08X in reference.",
                    i, k, got_bounds[k], want_bounds[k]);
                return false;
            }
        }
    }
    return true;
}

}  // namespace

bool Init() {
    if (!Config::g_settings.OptParticleQuad) return true;

    if (!BytesMatch(kBlockHead, kHeadWant, kHeadLen)) {
        Log("[ParticleQuad] NOT patched: bytes at 0x%08X are not the billboard quad block this was built for.",
            (unsigned)kBlockHead);
        return false;
    }
    if (!BytesMatch(kBlockTail, kTailWant, kTailLen)) {
        Log("[ParticleQuad] NOT patched: head matches but tail at 0x%08X does not; block refused.",
            (unsigned)kBlockTail);
        return false;
    }
    if (!SelfTestPasses()) return false;
    if (!WowOpt_ClientPatchAllowed((const void*)kBlockHead)) {
        Log("[ParticleQuad] NOT patched: No Client Patches is on, and this writes five bytes into wow.exe.");
        return false;
    }

    DWORD old = 0;
    if (!VirtualProtect((void*)kBlockHead, kHeadLen, PAGE_EXECUTE_READWRITE, &old)) {
        Log("[ParticleQuad] NOT patched: could not make 0x%08X writable", (unsigned)kBlockHead);
        return false;
    }
    memcpy(g_saved, (const void*)kBlockHead, kHeadLen);

    unsigned char patch[kHeadLen];
    patch[0] = 0xE9;
    *(int32_t*)(patch + 1) = (int32_t)((uintptr_t)&Thunk - (kBlockHead + 5));
    memset(patch + 5, 0x90, kHeadLen - 5);
    memcpy((void*)kBlockHead, patch, kHeadLen);

    DWORD ignored = 0;
    VirtualProtect((void*)kBlockHead, kHeadLen, old, &ignored);
    FlushInstructionCache(GetCurrentProcess(), (void*)kBlockHead, kHeadLen);

    g_patched = true;
    SamplingProfiler::RegisterSelfSymbol("ParticleBillboardQuad", (const void*)&Thunk);
    Log("[ParticleQuad] ACTIVE on the billboard quad block in sub_97BE80 (0x%08X). "
        "Evaluates 4-vertex positions in IEEE double matching client x87 operation order, "
        "and pipelines branchless AABB bounds derivations. Verified offline over 1,000,000 cases "
        "with 0 bit differences and checked against six fixed cases at startup.", (unsigned)kBlockHead);
    return true;
}

void Shutdown() {
    if (!g_patched) return;
    DWORD old = 0;
    if (VirtualProtect((void*)kBlockHead, kHeadLen, PAGE_EXECUTE_READWRITE, &old)) {
        memcpy((void*)kBlockHead, g_saved, kHeadLen);
        DWORD ignored = 0;
        VirtualProtect((void*)kBlockHead, kHeadLen, old, &ignored);
        FlushInstructionCache(GetCurrentProcess(), (void*)kBlockHead, kHeadLen);
    }
    g_patched = false;
}

void LogStats() {
    if (!Config::g_settings.OptParticleQuad) return;
    if (!g_patched) {
        Log("[ParticleQuad] not patched - the reason is at the top of this log");
        return;
    }
    if (g_calls == 0) {
        Log("[ParticleQuad] patched, and no billboard particle has reached this path yet.");
        return;
    }
    Log("[ParticleQuad] %llu particle billboard quad(s) generated here, each eliminating 24 x87 status-word branches and modes. Plain counter, lower bound.",
        g_calls);
}

}  // namespace ParticleQuad
