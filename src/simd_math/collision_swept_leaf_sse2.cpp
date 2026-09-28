// ============================================================================
// Module: collision_swept_leaf_sse2.cpp
//
// sub_7C9AB0 with its per-triangle call inlined.
//
// sub_7C9AB0 is the leaf handler of the BSP walk sub_7CA600 (its only caller).
// It runs the leaf test sub_7C6790 and then calls sub_7C6600 once per
// triangle. sub_7C6600 skips a triangle whose flag byte matches the query's
// filter dword, returns at once when the candidate list at 0x00D25BF8 is full
// (count at 0x00D2DBF8, capacity 0x2000; no overflow bit here), appends and
// marks it with 0x80, runs the ray-triangle test sub_983490 on the ray at
// query+0x10, and on a hit at or after the start keeps up to two nearest hits
// with <=, chosen by the flag byte as it was before the mark: 0x20 feeds
// both, 0x08 the first (query+0x4C/+0x54), 0x04 the second (query+0x50/+0x58).
// A NaN parameter fails the first compare (test ah,1) and changes nothing.
// This does the same loop without the call.
//
// What it does not reproduce: the return value, which the one call site
// (0x007CA624) discards. When the list is full the client keeps calling
// sub_7C6600 for the rest of the leaf and every call returns at once, so the
// loop here stops at the first one.
//
// The leaf test and the ray-triangle test are called through the client's own
// addresses. A startup self-test runs the loop against sub_7C6600 transcribed
// from the disassembly and compares the list, the flags and both hit slots.
// Not measured in game; off by default under the experimental switch
// CollisionSweptLeaf.
// ============================================================================

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <cstddef>
#include <cstdint>
#include <cstring>

#include "collision_swept_leaf_sse2.h"
#include "MinHook.h"
#include "version.h"
#include "config.h"
#include "sampling_profiler.h"
#include "ab_test.h"

extern "C" void Log(const char* fmt, ...);
extern "C" void WowOpt_NoteClientPatchRefused();
MH_STATUS WowOpt_CreateHookGuarded(void* pTarget, void* pDetour, void** ppOrig);
MH_STATUS WO_EnableHook(void* target);
bool WowOpt_InsideClientImage(const void* ptr);

namespace CollisionSweptLeaf {

namespace {

constexpr uintptr_t kTargetSub7C9AB0 = 0x007C9AB0;
constexpr uintptr_t kTargetSub7C6790 = 0x007C6790;   // leaf test
constexpr uintptr_t kRayTriFn        = 0x00983490;   // ray-triangle test

constexpr uintptr_t kHitCount   = 0x00D2DBF8;
constexpr uintptr_t kHitArray   = 0x00D25BF8;
constexpr uint32_t  kRingCap    = 0x2000;
constexpr float     kRayEpsilon = 0.0020000001f;     // flt_A32F78, same bits

// Expected 16-byte prologue at 0x007C9AB0
static const uint8_t kExpectedPrologue[16] = {
    0x55, 0x8B, 0xEC, 0x53, 0x57, 0x8B, 0x7D, 0x08,
    0x8B, 0xD9, 0x8B, 0x03, 0x8B, 0x4B, 0x04, 0x57
};

typedef char (__thiscall* OrigSub7C9AB0_fn)(void* this_ptr, const void* pLeaf);
typedef char (__thiscall* LeafTest_fn)(void* query, const void* pBspHeader, const void* pLeaf);
typedef bool (__cdecl* RayTri_fn)(const float* ray, const float* verts, const uint16_t* tri,
                                  float* hit_t, void* unused, float epsilon);

OrigSub7C9AB0_fn g_orig_Sub7C9AB0 = nullptr;
bool g_installed = false;
bool g_abSubject = false;

// Main-thread counters, read by the periodic report; a lower bound.
unsigned long long g_leaves = 0;
unsigned long long g_tris   = 0;

// What sub_7C6600 reads through ECX.
struct SweptQuery {
    uint32_t        pad00;
    uint8_t*        flags;      // +0x04, two bytes per triangle, the first is used
    const float*    verts;      // +0x08
    const uint16_t* indices;    // +0x0C, three per triangle
    float           ray[15];    // +0x10, passed by address; the test reads what it needs
    float           dist1;      // +0x4C
    float           dist2;      // +0x50
    uint32_t        tri1;       // +0x54
    uint32_t        tri2;       // +0x58
    uint32_t        filter;     // +0x5C
};
static_assert(offsetof(SweptQuery, ray) == 0x10, "SweptQuery layout");
static_assert(offsetof(SweptQuery, dist1) == 0x4C, "SweptQuery layout");
static_assert(offsetof(SweptQuery, filter) == 0x5C, "SweptQuery layout");

// The triangle loop of sub_7C9AB0 with sub_7C6600 (0x007C6600) inlined. Every
// field is read through `q` on every triangle, as the client does.
template <typename RayTri>
__forceinline void TriLoop(SweptQuery* q, const uint16_t* tris, uint32_t count,
                           uint32_t* hit_count, uint16_t* hit_list, RayTri ray_tri) {
    for (uint32_t i = 0; i < count; ++i) {
        const uint16_t t = tris[i];
        const uint32_t flag = q->flags[2u * t];
        if (q->filter & flag) continue;                              // 0x007C6616
        const uint32_t hits = *hit_count;
        if (hits >= kRingCap) return;                                // 0x007C6624
        hit_list[hits] = t;
        *hit_count = hits + 1;
        q->flags[2u * t] |= 0x80u;

        float hit_t = 0.0f;
        if (!ray_tri(q->ray, q->verts, q->indices + 3u * t, &hit_t, nullptr, kRayEpsilon))
            continue;
        if (!(flag & 0x2Cu)) continue;
        if (!(hit_t >= 0.0f)) continue;                              // test ah,1: < 0 or NaN
        if ((flag & 0x28u) && hit_t <= q->dist1) {                   // 0x20 or 0x08
            q->dist1 = hit_t;
            q->tri1 = t;
        }
        if ((flag & 0x20u) || !(flag & 0x08u)) {                     // 0x20, or 0x04 alone
            if (hit_t <= q->dist2) {
                q->tri2 = t;
                q->dist2 = hit_t;
            }
        }
    }
}

__declspec(safebuffers)
char __fastcall Hook_sub_7C9AB0(void* pCtx, void* /*edx*/, const void* pLeaf) {
    if (g_abSubject && AbTest::StandAside()) {
        return g_orig_Sub7C9AB0(pCtx, pLeaf);
    }

    void* header = *(void**)pCtx;
    SweptQuery* q = *(SweptQuery**)((uint8_t*)pCtx + 4);

    const char culled = ((LeafTest_fn)kTargetSub7C6790)(q, header, pLeaf);
    if (culled) return culled;

    const uint32_t count = *(const uint16_t*)((const uint8_t*)pLeaf + 6);
    const uint16_t* tris = *(const uint16_t* const*)((const uint8_t*)header + 8)
                         + *(const uint32_t*)((const uint8_t*)pLeaf + 8);
    g_leaves++;
    g_tris += count;

    TriLoop(q, tris, count, (uint32_t*)kHitCount, (uint16_t*)kHitArray, (RayTri_fn)kRayTriFn);
    return 0;
}

// ----------------------------------------------------------------------------
// Startup self-test
// ----------------------------------------------------------------------------

// fcom st, m; fnstsw: "test ah,1" sets on below or unordered, "test ah,41h"
// with jp on above or unordered.
bool BelowOrUnordered(double st, double m) { return st < m || st != st || m != m; }
bool AboveOrUnordered(double st, double m) { return st > m || st != st || m != m; }

// sub_7C6600 as the disassembly at 0x007C6600 has it, one call per triangle,
// and the loop of sub_7C9AB0 around it with no early exit.
template <typename RayTri>
void RefTri(SweptQuery* esi, uint16_t cx, uint32_t* hit_count, uint16_t* hit_list,
            RayTri ray_tri) {
    const uint32_t ebx = esi->flags[2u * cx];                        // movzx ebx, byte
    if (esi->filter & ebx) return;                                   // test [esi+5Ch], ebx
    const uint32_t eax = *hit_count;
    if (eax >= 0x2000) return;                                       // jnb
    hit_list[eax] = cx;
    *hit_count = eax + 1;
    esi->flags[2u * cx] |= 0x80;
    float arg0 = 0.0f;
    if (!ray_tri(esi->ray, esi->verts, esi->indices + 3u * cx, &arg0, nullptr, kRayEpsilon))
        return;
    const double st = arg0;
    if (ebx & 0x20) {
        if (BelowOrUnordered(st, 0.0)) return;                       // test ah,1 jnz
        if (!AboveOrUnordered(st, esi->dist1)) {                     // test ah,41h jp 0x007C66A1
            esi->dist1 = (float)st;
            esi->tri1 = cx;
        }
        if (AboveOrUnordered(st, esi->dist2)) return;                // test ah,41h jp
        esi->tri2 = cx;
        esi->dist2 = (float)st;
        return;
    }
    if (ebx & 8) {
        if (BelowOrUnordered(st, 0.0)) return;
        if (AboveOrUnordered(st, esi->dist1)) return;
        esi->tri1 = cx;
        esi->dist1 = (float)st;
        return;
    }
    if (ebx & 4) {
        if (BelowOrUnordered(st, 0.0)) return;                       // test ah,1 jz 0x007C66A1
        if (AboveOrUnordered(st, esi->dist2)) return;
        esi->tri2 = cx;
        esi->dist2 = (float)st;
    }
}

template <typename RayTri>
void RefLeaf(SweptQuery* q, const uint16_t* tris, uint32_t count, uint32_t* hit_count,
             uint16_t* hit_list, RayTri ray_tri) {
    for (uint32_t esi = 0; esi < count; ++esi) RefTri(q, tris[esi], hit_count, hit_list, ray_tri);
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
    SweptQuery q;
    uint8_t  flags[64 * 2];
    uint32_t hit_count;
};

bool RunSelfTest() {
    const size_t kListBytes = kRingCap * sizeof(uint16_t);
    uint16_t* mem = (uint16_t*)VirtualAlloc(nullptr, 2 * kListBytes, MEM_COMMIT | MEM_RESERVE,
                                            PAGE_READWRITE);
    if (!mem) {
        Log("[CollisionSweptLeaf] NOT active: no memory for the startup self-test.");
        return false;
    }
    uint16_t* hitA = mem;
    uint16_t* hitB = mem + kRingCap;

    float verts[40 * 3];
    uint16_t indices[64 * 3];
    for (int i = 0; i < 40 * 3; ++i) verts[i] = (float)i;
    for (int i = 0; i < 64 * 3; ++i) indices[i] = (uint16_t)(Rng() % 40);

    const uint32_t kNanBits = 0x7FC00000u;
    float nan_f;
    memcpy(&nan_f, &kNanBits, 4);
    static const uint32_t kFilters[] = { 0x80, 0x81, 0x00, 0xFFFFFFFFu, 0x10 };
    bool ok = true;
    int filled = 0, first = 0, second = 0;
    for (int c = 0; c < 400 && ok; ++c) {
        uint16_t tris[48];
        const uint32_t count = 1 + Rng() % 48;
        for (uint32_t i = 0; i < count; ++i) tris[i] = (uint16_t)(Rng() % 64);   // duplicates too

        TestState a;
        memset(&a, 0, sizeof(a));
        for (int i = 0; i < 64; ++i)
            a.flags[2 * i] = (uint8_t)((Rng() & 0x2C) | ((Rng() % 4 == 0) ? (Rng() & 0x91) : 0));
        a.hit_count = (c % 4 == 0) ? kRingCap - Rng() % 6 : Rng() % 32;
        a.q.verts = verts;
        a.q.indices = indices;
        for (int i = 0; i < 15; ++i) a.q.ray[i] = (float)i;
        a.q.dist1 = (c % 9 == 4) ? nan_f : 1.0f;     // a NaN slot now and then
        a.q.dist2 = (c % 11 == 5) ? nan_f : 0.75f;
        a.q.tri1 = 0xABCD;
        a.q.tri2 = 0xDCBA;
        a.q.filter = kFilters[c % 5];
        TestState b;
        memcpy(&b, &a, sizeof(a));
        memset(mem, 0, 2 * kListBytes);
        a.q.flags = a.flags;
        b.q.flags = b.flags;

        TriLoop(&a.q, tris, count, &a.hit_count, hitA, StubRayTri);
        RefLeaf(&b.q, tris, count, &b.hit_count, hitB, StubRayTri);

        a.q.flags = b.q.flags;
        if (memcmp(&a, &b, sizeof(a)) != 0 || memcmp(hitA, hitB, kListBytes) != 0) {
            Log("[CollisionSweptLeaf] NOT active: case %d left a different list, flag or hit "
                "slot from the transcription of sub_7C6600.", c);
            ok = false;
        }
        if (a.hit_count >= kRingCap) ++filled;
        if (a.q.tri1 != 0xABCD) ++first;
        if (a.q.tri2 != 0xDCBA) ++second;
    }
    VirtualFree(mem, 0, MEM_RELEASE);
    if (ok && (filled == 0 || first == 0 || second == 0)) {
        Log("[CollisionSweptLeaf] NOT active: the self-test never filled the list or never "
            "recorded a hit in one of the two slots.");
        ok = false;
    }
    return ok;
}

} // anonymous namespace

bool Init() {
    if (!Config::g_settings.OptCollisionSweptLeaf) {
        return false;
    }

    if (std::memcmp((const void*)kTargetSub7C9AB0, kExpectedPrologue, sizeof(kExpectedPrologue)) != 0) {
        Log("[CollisionSweptLeaf] NOT active: prologue mismatch at 0x%08X", (unsigned)kTargetSub7C9AB0);
        return false;
    }

    if (!WowOpt_InsideClientImage((const void*)kTargetSub7C9AB0)) {
        WowOpt_NoteClientPatchRefused();
        return false;
    }

    if (!RunSelfTest()) {
        return false;
    }

    MH_STATUS status = WowOpt_CreateHookGuarded(
        (void*)kTargetSub7C9AB0,
        (void*)&Hook_sub_7C9AB0,
        (void**)&g_orig_Sub7C9AB0
    );
    if (status != MH_OK) {
        Log("[CollisionSweptLeaf] NOT active: CreateHook failed (%d) at 0x%08X",
            (int)status, (unsigned)kTargetSub7C9AB0);
        return false;
    }

    status = WO_EnableHook((void*)kTargetSub7C9AB0);
    if (status != MH_OK) {
        MH_RemoveHook((void*)kTargetSub7C9AB0);
        Log("[CollisionSweptLeaf] NOT active: EnableHook failed (%d)", (int)status);
        return false;
    }

    g_installed = true;
    g_abSubject = AbTest::IsSubject("CollisionSweptLeaf", &g_abSubject);

    SamplingProfiler::RegisterSelfSymbol("CollisionSweptLeaf_sub_7C9AB0", (const void*)&Hook_sub_7C9AB0);

    Log("[CollisionSweptLeaf] ACTIVE on sub_7C9AB0 (0x%08X): the per-triangle call to "
        "sub_7C6600 is inlined. Off by default, not measured in game.",
        (unsigned)kTargetSub7C9AB0);
    return true;
}

void Shutdown() {
    if (!g_installed) return;
    MH_DisableHook((void*)kTargetSub7C9AB0);
    MH_RemoveHook((void*)kTargetSub7C9AB0);
    g_installed = false;
}

void LogStats() {
    if (!g_installed) return;
    Log("[CollisionSweptLeaf] %llu leaves past the leaf test, %llu triangles in them",
        g_leaves, g_tris);
}

} // namespace CollisionSweptLeaf
