// ============================================================================
// Module: collision_segment_leaf_sse2
//
// sub_7C9A00 with its per-triangle call inlined.
//
// sub_7C9A00 is the leaf handler of the segment BSP walk (sub_7CA180, its only
// caller). It runs the leaf test sub_7C6D50 and then calls sub_7C6C30 once
// per triangle. sub_7C6C30 skips a triangle whose flag byte matches the low
// byte of the query's mask, or whose material (flag byte 1, 64-byte records at
// query+0x50) the mask excludes through bits 0x100/0x200; appends it to the
// candidate list at 0x00D25BF8 (count at 0x00D2DBF8, capacity 0x2000), marks it
// with 0x80, runs the ray-triangle test sub_983490, and keeps the nearest hit
// at or after the start with <=, so a tie goes to the later triangle. The hit
// sets 0x00D29BF8 and 0x00D2DBFC and writes the distance, clamped to the
// query's maximum, through the query's output pointer. This does the same loop
// without the call.
//
// The client's x87 product of the hit parameter and the segment length is a
// float times a float, exact at 53 bits and rounded once on store, which is
// what a float multiply gives. The clamp replaces the distance only on an
// ordered "greater than", so a NaN distance stays (0x007C6D29).
//
// What it does not reproduce: the return value, which the one call site
// (0x007CA1A4) discards. When the candidate list is full the client keeps
// calling sub_7C6C30 for the rest of the leaf, and each call can only set the
// same overflow bit again, so the loop here stops at the first one.
//
// The leaf test and the ray-triangle test are called through the client's own
// addresses. A startup self-test runs the loop against sub_7C6C30 transcribed
// from the disassembly and compares every list, flag, hit field and the
// overflow word. Not measured in game; off by default under the experimental
// switch CollisionSegmentLeaf.
// ============================================================================

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <cstddef>
#include <cstdint>
#include <cstring>

#include "collision_segment_leaf_sse2.h"
#include "config.h"
#include "ab_test.h"
#include "sampling_profiler.h"
#include "MinHook.h"
#include "version.h"

extern "C" void Log(const char* fmt, ...);

namespace CollisionSegmentLeaf {

namespace {

constexpr uintptr_t kTargetSub7C9A00 = 0x007C9A00;
constexpr uintptr_t kTargetSub7C6D50 = 0x007C6D50;   // leaf test
constexpr uintptr_t kRayTriFn        = 0x00983490;   // ray-triangle test

constexpr uintptr_t kHitCount   = 0x00D2DBF8;
constexpr uintptr_t kHitArray   = 0x00D25BF8;
constexpr uintptr_t kBestFlag   = 0x00D2DBFC;
constexpr uintptr_t kBestTri    = 0x00D29BF8;
constexpr uint32_t  kRingCap    = 0x2000;
constexpr float     kRayEpsilon = 0.0020000001f;     // flt_A32F78, same bits

// Expected 16-byte prologue at 0x007C9A00:
// 55 8B EC 53 57 8B 7D 08 8B D9 8B 03 8B 4B 04 57
static const uint8_t kExpectedPrologue[16] = {
    0x55, 0x8B, 0xEC, 0x53, 0x57, 0x8B, 0x7D, 0x08,
    0x8B, 0xD9, 0x8B, 0x03, 0x8B, 0x4B, 0x04, 0x57
};

typedef char (__thiscall* OrigSub7C9A00_fn)(void* this_ptr, const void* pLeaf);
typedef char (__thiscall* LeafTest_fn)(void* query, const void* bspHeader, const void* pLeaf);
typedef bool (__cdecl* RayTri_fn)(const float* ray, const float* verts, const uint16_t* tri,
                                  float* hit_t, void* unused, float epsilon);

OrigSub7C9A00_fn g_orig_Sub7C9A00 = nullptr;
bool g_installed = false;
bool g_abSubject = false;

// Main-thread counters, read by the periodic report; a lower bound.
unsigned long long g_leaves = 0;
unsigned long long g_tris   = 0;

// What sub_7C6C30 reads through ECX.
struct SegmentQuery {
    uint32_t*       overflow;    // +0x00, may be null
    uint8_t*        flags;       // +0x04, two bytes per triangle
    const float*    verts;       // +0x08
    const uint16_t* indices;     // +0x0C, three per triangle
    float*          out_dist;    // +0x10
    float           max_dist;    // +0x14
    uint8_t         pad18[0x18];
    float           ray[6];      // +0x30, passed by address
    float           length;      // +0x48
    float           best_t;      // +0x4C
    const uint8_t*  materials;   // +0x50, 64-byte records, dword at +8
    uint32_t        pad54;
    uint16_t        mask;        // +0x58
};
static_assert(offsetof(SegmentQuery, out_dist) == 0x10, "SegmentQuery layout");
static_assert(offsetof(SegmentQuery, ray) == 0x30, "SegmentQuery layout");
static_assert(offsetof(SegmentQuery, best_t) == 0x4C, "SegmentQuery layout");
static_assert(offsetof(SegmentQuery, mask) == 0x58, "SegmentQuery layout");

// The candidate list and the nearest-hit globals. The hook points these at
// the client's; the self-test at its own.
struct Lists {
    uint32_t* hit_count;
    uint16_t* hit;
    uint32_t* best_flag;
    uint16_t* best_tri;
};

// The triangle loop of sub_7C9A00 with sub_7C6C30 (0x007C6C30) inlined. Every
// field is read through `q` on every triangle, as the client does.
template <typename RayTri>
__forceinline void TriLoop(SegmentQuery* q, const uint16_t* tris, uint32_t count,
                           const Lists& l, RayTri ray_tri) {
    for (uint32_t i = 0; i < count; ++i) {
        const uint16_t t = tris[i];
        const uint16_t mask = q->mask;
        if (q->flags[2u * t] & (uint8_t)mask) continue;              // 0x007C6C45
        const uint8_t mat = q->flags[2u * t + 1];
        const bool has = mat != 0xFF &&
                         *(const uint32_t*)(q->materials + ((uint32_t)mat << 6) + 8) != 0;
        if (mask & (has ? 0x100u : 0x200u)) continue;                // 0x007C6C6B / 0x007C6C73

        const uint32_t hits = *l.hit_count;
        if (hits >= kRingCap) {                                      // 0x007C6C85
            if (q->overflow) *q->overflow |= 1u;
            return;
        }
        l.hit[hits] = t;
        *l.hit_count = hits + 1;
        q->flags[2u * t] |= 0x80u;

        float hit_t = 0.0f;
        if (!ray_tri(q->ray, q->verts, q->indices + 3u * t, &hit_t, nullptr, kRayEpsilon))
            continue;
        if (!(hit_t >= 0.0f)) continue;                              // 0x007C6CF9: < 0 or NaN
        if (!(hit_t <= q->best_t)) continue;                         // 0x007C6D03
        q->best_t = hit_t;
        *l.best_tri = t;
        *l.best_flag = 1;
        *q->out_dist = (float)((double)hit_t * (double)q->length);   // 0x007C6D1C
        float* out = q->out_dist;
        if (*out > q->max_dist) *out = q->max_dist;                  // 0x007C6D29
    }
}

__declspec(safebuffers)
char __fastcall Hook_sub_7C9A00(void* pCtx, void* /*edx*/, const void* pLeaf) {
    if (g_abSubject && AbTest::StandAside()) {
        return g_orig_Sub7C9A00(pCtx, pLeaf);
    }

    void* header = *(void**)pCtx;
    SegmentQuery* q = *(SegmentQuery**)((uint8_t*)pCtx + 4);

    const char culled = ((LeafTest_fn)kTargetSub7C6D50)(q, header, pLeaf);
    if (culled) return culled;

    const uint32_t count = *(const uint16_t*)((const uint8_t*)pLeaf + 6);
    const uint16_t* tris = *(const uint16_t* const*)((const uint8_t*)header + 8)
                         + *(const uint32_t*)((const uint8_t*)pLeaf + 8);
    g_leaves++;
    g_tris += count;

    const Lists l = { (uint32_t*)kHitCount, (uint16_t*)kHitArray,
                      (uint32_t*)kBestFlag, (uint16_t*)kBestTri };
    TriLoop(q, tris, count, l, (RayTri_fn)kRayTriFn);
    return 0;
}

// ----------------------------------------------------------------------------
// Startup self-test
// ----------------------------------------------------------------------------

// sub_7C6C30 as the disassembly at 0x007C6C30 has it, one call per triangle,
// and the loop of sub_7C9A00 around it with no early exit.
template <typename RayTri>
void RefTri(SegmentQuery* esi, uint16_t di, const Lists& l, RayTri ray_tri) {
    const uint32_t edx = esi->mask;                                  // movzx edx, word [esi+58h]
    if (esi->flags[2u * di] & (uint8_t)edx) return;                  // test [ecx+eax*2], dl
    const uint8_t cl = esi->flags[2u * di + 1];
    if (cl < 0xFF && *(const uint32_t*)(esi->materials + ((uint32_t)cl << 6) + 8) != 0) {
        if (edx & 0x100) return;
    } else {
        if (edx & 0x200) return;
    }
    const uint32_t ecx = *l.hit_count;
    if (!(ecx < 0x2000)) {
        if (esi->overflow) *esi->overflow |= 1;
        return;
    }
    l.hit[ecx] = di;
    *l.hit_count = ecx + 1;
    esi->flags[2u * di] |= 0x80;
    float arg0 = 0.0f;                                               // fldz; fstp [ebp+arg_0]
    if (!ray_tri(esi->ray, esi->verts, esi->indices + 3u * di, &arg0, nullptr, kRayEpsilon))
        return;
    const double st = arg0;
    if (st < 0.0 || st != st) return;                                // test ah,1 jnz
    if (st > esi->best_t || st != st) return;                        // test ah,41h jp
    esi->best_t = (float)st;                                         // fst [esi+4Ch]
    *l.best_tri = di;
    *l.best_flag = 1;
    *esi->out_dist = (float)(st * esi->length);                      // fmul; fstp [ecx]
    float* out = esi->out_dist;
    const double o = *out;
    if (o < esi->max_dist || o == esi->max_dist || o != o) *out = (float)o;
    else *out = esi->max_dist;
}

template <typename RayTri>
void RefLeaf(SegmentQuery* q, const uint16_t* tris, uint32_t count, const Lists& l,
             RayTri ray_tri) {
    for (uint32_t esi = 0; esi < count; ++esi) RefTri(q, tris[esi], l, ray_tri);
}

uint32_t g_rng = 0x9E3779B9u;
uint32_t Rng() { g_rng ^= g_rng << 13; g_rng ^= g_rng >> 17; g_rng ^= g_rng << 5; return g_rng; }

// Stands in for sub_983490: a hit or not, and a parameter from a short list
// with repeats, a negative value and a NaN, chosen by the triangle's address,
// the vertex array's and the ray's contents, so a wrong argument gives
// another answer.
bool StubRayTri(const float* ray, const float* verts, const uint16_t* tri, float* t, void* unused,
                float eps) {
    static const uint32_t kT[8] = { 0x3E800000u, 0x3F000000u, 0x3F000000u, 0xBDCCCCCDu,
                                    0x7FC00000u, 0x40000000u, 0x3F400000u, 0x00000000u };
    const uint32_t h = ((uint32_t)(uintptr_t)tri + (uint32_t)(uintptr_t)verts) * 2654435761u ^
                       (uint32_t)(ray[2] * 4096.0f + ray[4] * 64.0f);
    if (unused || eps != kRayEpsilon || ((h >> 7) & 3u) == 0) return false;
    memcpy(t, &kT[(h >> 11) & 7u], 4);
    return true;
}

struct TestState {
    SegmentQuery q;
    uint32_t overflow;
    float    out;
    uint8_t  flags[64 * 2];
    uint32_t hit_count, best_flag;
    uint16_t best_tri;
    uint16_t pad;
};

bool RunSelfTest() {
    const size_t kListBytes = kRingCap * sizeof(uint16_t);
    uint16_t* mem = (uint16_t*)VirtualAlloc(nullptr, 2 * kListBytes, MEM_COMMIT | MEM_RESERVE,
                                            PAGE_READWRITE);
    if (!mem) {
        Log("[CollisionSegmentLeaf] NOT active: no memory for the startup self-test.");
        return false;
    }
    uint16_t* hitA = mem;
    uint16_t* hitB = mem + kRingCap;

    float verts[40 * 3];
    uint16_t indices[64 * 3];
    uint8_t materials[8 * 64];
    for (int i = 0; i < 40 * 3; ++i) verts[i] = (float)i;
    for (int i = 0; i < 64 * 3; ++i) indices[i] = (uint16_t)(Rng() % 40);

    const uint32_t kNanBits = 0x7FC00000u;
    float nan_f;
    memcpy(&nan_f, &kNanBits, 4);
    static const uint16_t kMasks[] = { 0x0080, 0x0180, 0x0280, 0x0381, 0x0000, 0x00FF };
    bool ok = true;
    int filled = 0, nulls = 0, hits = 0;
    for (int c = 0; c < 400 && ok; ++c) {
        memset(materials, 0, sizeof(materials));
        for (int r = 0; r < 8; ++r) materials[r * 64 + 8] = (uint8_t)(Rng() & 1);
        uint16_t tris[48];
        const uint32_t count = 1 + Rng() % 48;
        for (uint32_t i = 0; i < count; ++i) tris[i] = (uint16_t)(Rng() % 64);   // duplicates too

        TestState a;
        memset(&a, 0, sizeof(a));
        for (int i = 0; i < 64; ++i) {
            a.flags[2 * i]     = (uint8_t)((Rng() % 4 == 0) ? (Rng() & 0x83) : 0);
            a.flags[2 * i + 1] = (uint8_t)((Rng() % 3 == 0) ? 0xFF : Rng() % 8);
        }
        a.hit_count = (c % 4 == 0) ? kRingCap - Rng() % 6 : Rng() % 32;
        a.best_tri = 0xABCD;
        a.out = -1.0f;
        a.q.flags = nullptr;            // pointed below, per copy
        a.q.verts = verts;
        a.q.indices = indices;
        a.q.max_dist = (c % 3 == 0) ? 0.75f : 100.0f;
        for (int i = 0; i < 6; ++i) a.q.ray[i] = (float)i;
        a.q.length = (c % 7 == 3) ? nan_f : 2.0f;   // a NaN length now and then
        a.q.best_t = 1.0f;
        a.q.materials = materials;
        a.q.mask = kMasks[c % 6];
        const bool null_overflow = (c % 5 == 0);
        TestState b;
        memcpy(&b, &a, sizeof(a));
        memset(mem, 0, 2 * kListBytes);

        a.q.overflow = null_overflow ? nullptr : &a.overflow;
        a.q.flags = a.flags;
        a.q.out_dist = &a.out;
        b.q.overflow = null_overflow ? nullptr : &b.overflow;
        b.q.flags = b.flags;
        b.q.out_dist = &b.out;

        const Lists la = { &a.hit_count, hitA, &a.best_flag, &a.best_tri };
        const Lists lb = { &b.hit_count, hitB, &b.best_flag, &b.best_tri };
        TriLoop(&a.q, tris, count, la, StubRayTri);
        RefLeaf(&b.q, tris, count, lb, StubRayTri);

        // The queries hold pointers into their own state; compare the rest.
        a.q.overflow = b.q.overflow; a.q.flags = b.q.flags; a.q.out_dist = b.q.out_dist;
        if (memcmp(&a, &b, sizeof(a)) != 0 || memcmp(hitA, hitB, kListBytes) != 0) {
            Log("[CollisionSegmentLeaf] NOT active: case %d left a different list, flag, "
                "hit or overflow bit from the transcription of sub_7C6C30.", c);
            ok = false;
        }
        if (a.hit_count >= kRingCap) ++filled;
        if (null_overflow) ++nulls;
        if (a.best_flag) ++hits;
    }
    VirtualFree(mem, 0, MEM_RELEASE);
    if (ok && (filled == 0 || nulls == 0 || hits == 0)) {
        Log("[CollisionSegmentLeaf] NOT active: the self-test never filled the list, never "
            "ran with a null overflow pointer or never recorded a hit.");
        ok = false;
    }
    return ok;
}

} // anonymous namespace

bool Init() {
    if (!Config::g_settings.OptCollisionSegmentLeaf) {
        return false;
    }

    if (memcmp((const void*)kTargetSub7C9A00, kExpectedPrologue, sizeof(kExpectedPrologue)) != 0) {
        Log("[CollisionSegmentLeaf] NOT active: prologue mismatch at 0x%08X", (unsigned)kTargetSub7C9A00);
        return false;
    }

    if (!WowOpt_InsideClientImage((const void*)kTargetSub7C9A00)) {
        WowOpt_NoteClientPatchRefused();
        return false;
    }

    if (!RunSelfTest()) {
        return false;
    }

    MH_STATUS status = WowOpt_CreateHookGuarded(
        (void*)kTargetSub7C9A00,
        (void*)&Hook_sub_7C9A00,
        (void**)&g_orig_Sub7C9A00
    );
    if (status != MH_OK) {
        Log("[CollisionSegmentLeaf] NOT active: CreateHook failed (%d) at 0x%08X",
            (int)status, (unsigned)kTargetSub7C9A00);
        return false;
    }

    status = WO_EnableHook((void*)kTargetSub7C9A00);
    if (status != MH_OK) {
        MH_RemoveHook((void*)kTargetSub7C9A00);
        Log("[CollisionSegmentLeaf] NOT active: EnableHook failed (%d)", (int)status);
        return false;
    }

    g_installed = true;
    g_abSubject = AbTest::IsSubject("CollisionSegmentLeaf", &g_abSubject);

    SamplingProfiler::RegisterSelfSymbol("CollisionSegmentLeaf_sub_7C9A00", (const void*)&Hook_sub_7C9A00);

    Log("[CollisionSegmentLeaf] ACTIVE on sub_7C9A00 (0x%08X): the per-triangle call to "
        "sub_7C6C30 is inlined. Off by default, not measured in game.",
        (unsigned)kTargetSub7C9A00);
    return true;
}

void Shutdown() {
    if (!g_installed) return;
    MH_DisableHook((void*)kTargetSub7C9A00);
    MH_RemoveHook((void*)kTargetSub7C9A00);
    g_installed = false;
}

void LogStats() {
    if (!g_installed) return;
    Log("[CollisionSegmentLeaf] %llu leaves past the leaf test, %llu triangles in them",
        g_leaves, g_tris);
}

} // namespace CollisionSegmentLeaf
