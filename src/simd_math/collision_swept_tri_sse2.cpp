// ============================================================================
// Module: collision_swept_tri_sse2
//
// sub_7C6600 (258 bytes at 0x007C6600) without the x87 status-word branches.
//
// sub_7C6600 is the per-triangle test of the BSP walk sub_7CA600, called once
// per triangle by its leaf handler sub_7C9AB0. It skips a triangle whose flag
// byte matches the query's filter dword, returns at once when the candidate
// list at 0x00D25BF8 is full (count at 0x00D2DBF8, capacity 0x2000), appends
// and marks it with 0x80, runs the ray-triangle test sub_983490 on the ray at
// query+0x10, and on a hit at or after the start keeps up to two nearest hits
// with <=, chosen by the flag byte as it was before the mark: 0x20 feeds both,
// 0x08 the first (query+0x4C/+0x54), 0x04 the second (query+0x50/+0x58). A NaN
// parameter fails the first compare (test ah,1) and changes nothing. The
// comparisons here are single-precision compares of the same floats, which
// answer as the x87 ones do; there is no arithmetic.
//
// CollisionSweptLeaf inlines this same body into sub_7C9AB0; with that switch
// on, sub_7C6600 is not called and this hook sees nothing.
//
// A startup self-test runs the body against sub_7C6600 transcribed from the
// disassembly on local lists and compares the list, the flags and both hit
// slots. Not measured in game; off by default under the experimental switch
// CollisionSweptTri.
// ============================================================================

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <cstddef>
#include <cstdint>
#include <cstring>

#include "collision_swept_tri_sse2.h"
#include "MinHook.h"
#include "version.h"
#include "config.h"
#include "sampling_profiler.h"
#include "ab_test.h"

extern "C" void Log(const char* fmt, ...);
MH_STATUS WineSafe_CreateHook(void* target, void* detour, void** original);
MH_STATUS WO_EnableHook(void* target);

namespace CollisionSweptTri {

namespace {

constexpr uintptr_t kTarget     = 0x007C6600;
constexpr uintptr_t kRayTriFn   = 0x00983490;
constexpr uintptr_t kHitCount   = 0x00D2DBF8;
constexpr uintptr_t kHitArray   = 0x00D25BF8;
constexpr uint32_t  kRingCap    = 0x2000;
constexpr float     kRayEpsilon = 0.0020000001f;     // flt_A32F78, same bits

// Expected 16-byte prologue at 0x007C6600
static const uint8_t kExpectedPrologue[16] = {
    0x55, 0x8b, 0xec, 0x53, 0x56, 0x8b, 0xf1, 0x66,
    0x8b, 0x4d, 0x08, 0x8b, 0x46, 0x04, 0x57, 0x0f
};

typedef void (__thiscall* OrigSweptTri_fn)(void* this_ptr, uint32_t tri_arg);
typedef bool (__cdecl* RayTri_fn)(const float* ray, const float* verts, const uint16_t* tri,
                                  float* hit_t, void* unused, float epsilon);

OrigSweptTri_fn g_origSweptTri = nullptr;
bool g_installed = false;
bool g_abSubject = false;

// Main-thread counter, read by the periodic report; a lower bound.
unsigned long long g_calls = 0;

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

template <typename RayTri>
__forceinline void TriBody(SweptQuery* q, uint16_t t, uint32_t* hit_count, uint16_t* hit_list,
                           RayTri ray_tri) {
    const uint32_t flag = q->flags[2u * t];
    if (q->filter & flag) return;                                    // 0x007C6616
    const uint32_t hits = *hit_count;
    if (hits >= kRingCap) return;                                    // 0x007C6624
    hit_list[hits] = t;
    *hit_count = hits + 1;
    q->flags[2u * t] |= 0x80u;

    float hit_t = 0.0f;
    if (!ray_tri(q->ray, q->verts, q->indices + 3u * t, &hit_t, nullptr, kRayEpsilon)) return;
    if (!(flag & 0x2Cu)) return;
    if (!(hit_t >= 0.0f)) return;                                    // test ah,1: < 0 or NaN
    if ((flag & 0x28u) && hit_t <= q->dist1) {                       // 0x20 or 0x08
        q->dist1 = hit_t;
        q->tri1 = t;
    }
    if ((flag & 0x20u) || !(flag & 0x08u)) {                         // 0x20, or 0x04 alone
        if (hit_t <= q->dist2) {
            q->tri2 = t;
            q->dist2 = hit_t;
        }
    }
}

__declspec(safebuffers)
void __fastcall Hooked_TestSweptTri(void* this_ptr, void* /*dummy_edx*/, uint32_t tri_arg) {
    if (g_abSubject && AbTest::StandAside()) {
        g_origSweptTri(this_ptr, tri_arg);
        return;
    }
    g_calls++;
    TriBody((SweptQuery*)this_ptr, (uint16_t)tri_arg, (uint32_t*)kHitCount, (uint16_t*)kHitArray,
            (RayTri_fn)kRayTriFn);
}

// ----------------------------------------------------------------------------
// Startup self-test
// ----------------------------------------------------------------------------

// fcom st, m; fnstsw: "test ah,1" sets on below or unordered, "test ah,41h"
// with jp on above or unordered.
bool BelowOrUnordered(double st, double m) { return st < m || st != st || m != m; }
bool AboveOrUnordered(double st, double m) { return st > m || st != st || m != m; }

// sub_7C6600 as the disassembly at 0x007C6600 has it.
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
        Log("[CollisionSweptTri] NOT active: no memory for the startup self-test.");
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

        for (uint32_t i = 0; i < count; ++i) {
            TriBody(&a.q, tris[i], &a.hit_count, hitA, StubRayTri);
            RefTri(&b.q, tris[i], &b.hit_count, hitB, StubRayTri);
        }

        a.q.flags = b.q.flags;
        if (memcmp(&a, &b, sizeof(a)) != 0 || memcmp(hitA, hitB, kListBytes) != 0) {
            Log("[CollisionSweptTri] NOT active: case %d left a different list, flag or hit "
                "slot from the transcription of sub_7C6600.", c);
            ok = false;
        }
        if (a.hit_count >= kRingCap) ++filled;
        if (a.q.tri1 != 0xABCD) ++first;
        if (a.q.tri2 != 0xDCBA) ++second;
    }
    VirtualFree(mem, 0, MEM_RELEASE);
    if (ok && (filled == 0 || first == 0 || second == 0)) {
        Log("[CollisionSweptTri] NOT active: the self-test never filled the list or never "
            "recorded a hit in one of the two slots.");
        ok = false;
    }
    return ok;
}

} // namespace

bool Init() {
    if (!Config::g_settings.OptCollisionSweptTri) {
        return true;
    }

    if (std::memcmp((const void*)kTarget, kExpectedPrologue, sizeof(kExpectedPrologue)) != 0) {
        Log("[CollisionSweptTri] NOT active: prologue mismatch at 0x%08X", (unsigned)kTarget);
        return false;
    }

    if (!RunSelfTest()) {
        return false;
    }

    MH_STATUS status = WineSafe_CreateHook((void*)kTarget, (void*)&Hooked_TestSweptTri, (void**)&g_origSweptTri);
    if (status != MH_OK) {
        Log("[CollisionSweptTri] NOT active: CreateHook failed at 0x%08X (status=%d)", (unsigned)kTarget, status);
        return false;
    }

    status = WO_EnableHook((void*)kTarget);
    if (status != MH_OK) {
        MH_RemoveHook((void*)kTarget);
        Log("[CollisionSweptTri] NOT active: EnableHook failed at 0x%08X (status=%d)", (unsigned)kTarget, status);
        return false;
    }

    g_installed = true;
    g_abSubject = AbTest::IsSubject("CollisionSweptTri", &g_abSubject);
    SamplingProfiler::RegisterSelfSymbol("CollisionSweptTri", (const void*)&Hooked_TestSweptTri);
    Log("[CollisionSweptTri] ACTIVE on sub_7C6600 (0x%08X). Off by default, not measured in game.",
        (unsigned)kTarget);
    return true;
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
    Log("[CollisionSweptTri] %llu calls", g_calls);
}

} // namespace CollisionSweptTri
