// ============================================================================
// Module: collision_bsp_leaf_sse2.cpp
//
// sub_7CA8C0 with its per-triangle call inlined.
//
// sub_7CA8C0 is the leaf handler of the box-query BSP walk (sub_7CA920, its
// only caller). It runs the leaf's outcode test (sub_7C7230) and then calls
// sub_7C9B10 once per triangle in the leaf. sub_7C9B10 skips a triangle whose
// flag byte matches the query's mask, appends it to the candidate list at
// 0x00D25BF8 (count at 0x00D2DBF8, capacity 0x2000), marks it with 0x80, runs
// the box-triangle test sub_7C7A00, and appends the survivors to the list at
// 0x00D29BF8 (count at 0x00D2DBFC). This does the same loop without the call.
//
// What it does not reproduce: the return value. sub_7CA8C0 returns whatever
// is in AL after its last call, and the one call site (0x007CA93F) discards
// it. When the candidate list is full the client keeps calling sub_7C9B10 for
// the rest of the leaf, and each call can only set the same overflow bit
// again, so the loop here stops at the first one.
//
// The outcode test and the box-triangle test are called through the client's
// own addresses, so the modules that replace them still apply.
//
// A startup self-test runs the loop against sub_7C9B10 transcribed from the
// disassembly, on random leaves with duplicates, flagged triangles, a list
// that fills and a null overflow pointer, and compares both lists, the flag
// bytes and the overflow word. Not measured in game; off by default under the
// experimental switch CollisionBspLeaf.
// ============================================================================

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <cstddef>
#include <cstdint>
#include <cstring>

#include "collision_bsp_leaf_sse2.h"
#include "MinHook.h"
#include "version.h"
#include "config.h"
#include "ab_test.h"
#include "sampling_profiler.h"

extern "C" void Log(const char* fmt, ...);
MH_STATUS WineSafe_CreateHook(void* target, void* detour, void** original);
MH_STATUS WO_EnableHook(void* target);

namespace CollisionBspLeaf {

namespace {

constexpr uintptr_t kTargetLeaf = 0x007CA8C0; // sub_7CA8C0 (83 bytes)
constexpr uintptr_t kClassify   = 0x007C7230; // sub_7C7230, leaf outcode test
constexpr uintptr_t kBoxTri     = 0x007C7A00; // sub_7C7A00, box-triangle test

constexpr uintptr_t kHitCount   = 0x00D2DBF8;
constexpr uintptr_t kHitArray   = 0x00D25BF8;
constexpr uintptr_t kNearCount  = 0x00D2DBFC;
constexpr uintptr_t kNearArray  = 0x00D29BF8;
constexpr uint32_t  kRingCap    = 0x2000;

// Prologue bytes of sub_7CA8C0 (16 bytes):
// 55:          push ebp
// 8B EC:       mov ebp, esp
// 53:          push ebx
// 57:          push edi
// 8B 7D 08:    mov edi, [ebp+arg_0]
// 8B D9:       mov ebx, ecx
// 8B 03:       mov eax, [ebx]
// 8B 4B 04:    mov ecx, [ebx+4]
// 57:          push edi
static const uint8_t kExpectedPrologueLeaf[16] = {
    0x55, 0x8B, 0xEC, 0x53, 0x57, 0x8B, 0x7D, 0x08,
    0x8B, 0xD9, 0x8B, 0x03, 0x8B, 0x4B, 0x04, 0x57
};

typedef char (__thiscall* Sub7CA8C0_fn)(void* this_ptr, const void* node);
typedef char (__thiscall* Classify_fn)(void* query_context, void* bsp_desc, const void* node);
typedef char (__cdecl*    BoxTri_fn)(const float* box, const float* v0, const float* v1, const float* v2);

Sub7CA8C0_fn g_orig_Sub7CA8C0 = nullptr;

bool g_installed = false;
bool g_abSubject = false;

// Main-thread counters, read by the periodic report; a lower bound.
unsigned long long g_leaves = 0;
unsigned long long g_tris   = 0;

// What sub_7C9B10 reads through ECX.
struct QueryContext {
    uint32_t*       overflow;   // +0x00, bit 0 set when the list is full; may be null
    uint8_t*        flags;      // +0x04, two bytes per triangle, the first is tested
    const float*    verts;      // +0x08
    const uint16_t* indices;    // +0x0C, three per triangle
    const float*    box;        // +0x10
    uint8_t         test_mask;  // +0x14
};
static_assert(offsetof(QueryContext, flags) == 0x04, "QueryContext layout");
static_assert(offsetof(QueryContext, box) == 0x10, "QueryContext layout");
static_assert(offsetof(QueryContext, test_mask) == 0x14, "QueryContext layout");

// The two lists and their counts. The hook points these at the client's
// globals; the self-test at its own.
struct Lists {
    uint32_t* hit_count;
    uint16_t* hit;
    uint32_t* near_count;
    uint16_t* near_list;
};

// The triangle loop of sub_7CA8C0 with sub_7C9B10 (0x007C9B10) inlined. It
// reads the context through `ctx` on every triangle, as the client does.
template <typename BoxTest>
__forceinline void TriLoop(const QueryContext* ctx, const uint16_t* tris, uint32_t count,
                           const Lists& l, BoxTest box_test) {
    for (uint32_t i = 0; i < count; ++i) {
        const uint16_t t = tris[i];
        if (ctx->flags[2u * t] & ctx->test_mask) continue;           // 0x007C9B21
        const uint32_t hits = *l.hit_count;
        if (hits >= kRingCap) {                                      // 0x007C9B30
            if (ctx->overflow) *ctx->overflow |= 1u;
            return;
        }
        l.hit[hits] = t;                                             // 0x007C9B46
        *l.hit_count = hits + 1;
        ctx->flags[2u * t] |= 0x80u;
        const uint16_t* ix = ctx->indices + 3u * t;
        const float* v = ctx->verts;
        if (!box_test(ctx->box, v + 3u * ix[0], v + 3u * ix[1], v + 3u * ix[2])) {
            const uint32_t n = *l.near_count;                        // 0x007C9BA1
            l.near_list[n] = t;
            *l.near_count = n + 1;
        }
    }
}

__declspec(safebuffers)
char __fastcall Hook_sub_7CA8C0(void* this_ptr, void* /*dummy_edx*/, const void* node) {
    if (g_abSubject && AbTest::StandAside()) {
        return g_orig_Sub7CA8C0(this_ptr, node);
    }

    void* bsp_desc = *(void**)this_ptr;
    const QueryContext* ctx = *(const QueryContext* const*)((const uint8_t*)this_ptr + 4);

    // 0x007CA8D1: the outcode test first; anything non-zero ends the leaf.
    const char culled = ((Classify_fn)kClassify)((void*)ctx, bsp_desc, node);
    if (culled) return culled;

    const uint32_t count = *(const uint16_t*)((const uint8_t*)node + 6);
    const uint16_t* tris = *(const uint16_t* const*)((const uint8_t*)bsp_desc + 8)
                         + *(const uint32_t*)((const uint8_t*)node + 8);
    g_leaves++;
    g_tris += count;

    const Lists l = { (uint32_t*)kHitCount, (uint16_t*)kHitArray,
                      (uint32_t*)kNearCount, (uint16_t*)kNearArray };
    TriLoop(ctx, tris, count, l, (BoxTri_fn)kBoxTri);
    return 0;
}

// ----------------------------------------------------------------------------
// Startup self-test
// ----------------------------------------------------------------------------

// sub_7C9B10 as the disassembly at 0x007C9B10 has it, one call per triangle,
// and the loop of sub_7CA8C0 around it with no early exit.
template <typename BoxTest>
void RefTri(QueryContext* c, uint16_t bx, const Lists& l, BoxTest box_test) {
    if (c->test_mask & c->flags[2u * bx]) return;                    // test [ecx+14h], dl
    uint32_t edx = *l.hit_count;
    if (edx >= 0x2000) {                                             // cmp edx, 2000h; jb
        if (c->overflow) *c->overflow |= 1;                          // or dword ptr [ecx], 1
        return;
    }
    l.hit[edx] = bx;
    *l.hit_count = edx + 1;
    c->flags[2u * bx] |= 0x80;
    const uint32_t eax = 3u * bx;
    const float* p2 = c->verts + 3u * c->indices[eax + 2];           // pushed first
    const float* p1 = c->verts + 3u * c->indices[eax + 1];
    const float* p0 = c->verts + 3u * c->indices[eax];
    if (box_test(c->box, p0, p1, p2)) return;                        // test al, al; jnz
    const uint32_t n = *l.near_count;
    l.near_list[n] = bx;
    *l.near_count = n + 1;
}

template <typename BoxTest>
void RefLeaf(QueryContext* c, const uint16_t* tris, uint32_t count, const Lists& l,
             BoxTest box_test) {
    for (uint32_t esi = 0; esi < count; ++esi) RefTri(c, tris[esi], l, box_test);
}

uint32_t g_rng = 0x9E3779B9u;
uint32_t Rng() { g_rng ^= g_rng << 13; g_rng ^= g_rng >> 17; g_rng ^= g_rng << 5; return g_rng; }

// Stands in for sub_7C7A00: depends on which vertex is which and on the box,
// so an argument out of place changes the answer.
char StubBoxTri(const float* box, const float* v0, const float* v1, const float* v2) {
    const uint32_t h = (uint32_t)(uintptr_t)box * 3u + (uint32_t)(uintptr_t)v0 * 5u +
                       (uint32_t)(uintptr_t)v1 * 7u + (uint32_t)(uintptr_t)v2 * 11u;
    return (char)((h >> 4) & 1u);
}

struct TestState {
    uint32_t overflow;
    uint8_t  flags[64 * 2];
    uint32_t hit_count, near_count;
};

bool RunSelfTest() {
    const size_t kListBytes = kRingCap * sizeof(uint16_t);
    uint8_t* mem = (uint8_t*)VirtualAlloc(nullptr, 4 * kListBytes, MEM_COMMIT | MEM_RESERVE,
                                          PAGE_READWRITE);
    if (!mem) {
        Log("[CollisionBspLeaf] NOT active: no memory for the startup self-test.");
        return false;
    }
    uint16_t* hitA  = (uint16_t*)(mem);
    uint16_t* nearA = (uint16_t*)(mem + kListBytes);
    uint16_t* hitB  = (uint16_t*)(mem + 2 * kListBytes);
    uint16_t* nearB = (uint16_t*)(mem + 3 * kListBytes);

    float verts[40 * 3];
    uint16_t indices[64 * 3];
    float box[6] = { -1.0f, -2.0f, -3.0f, 4.0f, 5.0f, 6.0f };
    for (int i = 0; i < 40 * 3; ++i) verts[i] = (float)i;

    static const uint8_t kMasks[] = { 0x80, 0x81, 0x00, 0xFF, 0x02 };
    bool ok = true;
    int filled = 0, nulls = 0;
    for (int c = 0; c < 400 && ok; ++c) {
        for (int i = 0; i < 64 * 3; ++i) indices[i] = (uint16_t)(Rng() % 40);
        uint16_t tris[48];
        const uint32_t count = 1 + Rng() % 48;
        for (uint32_t i = 0; i < count; ++i) tris[i] = (uint16_t)(Rng() % 64);   // duplicates too

        TestState a;
        memset(&a, 0, sizeof(a));
        for (int i = 0; i < 64 * 2; ++i) a.flags[i] = (uint8_t)((Rng() % 4 == 0) ? (Rng() & 0x83) : 0);
        a.hit_count  = (c % 4 == 0) ? kRingCap - Rng() % 6 : Rng() % 32;
        a.near_count = Rng() % 32;
        const bool null_overflow = (c % 5 == 0);
        TestState b = a;
        memset(mem, 0, 4 * kListBytes);

        QueryContext ca = { null_overflow ? nullptr : &a.overflow, a.flags, verts, indices, box,
                            kMasks[c % 5] };
        QueryContext cb = ca;
        cb.overflow = null_overflow ? nullptr : &b.overflow;
        cb.flags = b.flags;

        const Lists la = { &a.hit_count, hitA, &a.near_count, nearA };
        const Lists lb = { &b.hit_count, hitB, &b.near_count, nearB };
        TriLoop(&ca, tris, count, la, StubBoxTri);
        RefLeaf(&cb, tris, count, lb, StubBoxTri);

        if (memcmp(&a, &b, sizeof(a)) != 0 ||
            memcmp(hitA, hitB, kListBytes) != 0 || memcmp(nearA, nearB, kListBytes) != 0) {
            Log("[CollisionBspLeaf] NOT active: case %d left different lists, flags or "
                "overflow bit from the transcription of sub_7C9B10.", c);
            ok = false;
        }
        if (a.hit_count >= kRingCap) ++filled;
        if (null_overflow) ++nulls;
    }
    VirtualFree(mem, 0, MEM_RELEASE);
    if (ok && (filled == 0 || nulls == 0)) {
        Log("[CollisionBspLeaf] NOT active: the self-test never filled the list or never "
            "ran with a null overflow pointer, so those paths went untested.");
        ok = false;
    }
    return ok;
}

} // namespace

bool Init() {
    if (!Config::g_settings.OptCollisionBspLeaf) {
        return true;
    }

    if (memcmp((const void*)kTargetLeaf, kExpectedPrologueLeaf, sizeof(kExpectedPrologueLeaf)) != 0) {
        Log("[CollisionBspLeaf] NOT active: prologue mismatch at sub_7CA8C0 (0x%08X)", kTargetLeaf);
        return false;
    }

    if (!RunSelfTest()) {
        return false;
    }

    if (!WowOpt_ClientPatchAllowed((const void*)kTargetLeaf)) {
        return false;
    }

    MH_STATUS status = WineSafe_CreateHook((void*)kTargetLeaf, (void*)&Hook_sub_7CA8C0,
                                           (void**)&g_orig_Sub7CA8C0);
    if (status != MH_OK) {
        Log("[CollisionBspLeaf] NOT active: CreateHook failed at 0x%08X (status=%d)", kTargetLeaf, status);
        return false;
    }
    if (WO_EnableHook((void*)kTargetLeaf) != MH_OK) {
        MH_RemoveHook((void*)kTargetLeaf);
        Log("[CollisionBspLeaf] NOT active: EnableHook failed");
        return false;
    }

    g_installed = true;
    g_abSubject = AbTest::IsSubject("CollisionBspLeaf", &g_abSubject);

    SamplingProfiler::RegisterSelfSymbol("CollisionBspLeaf_sub_7CA8C0", (const void*)&Hook_sub_7CA8C0);

    Log("[CollisionBspLeaf] ACTIVE on sub_7CA8C0 (0x%08X): the per-triangle call to "
        "sub_7C9B10 is inlined. Off by default, not measured in game.", kTargetLeaf);
    return true;
}

void Shutdown() {
    if (g_installed) {
        MH_DisableHook((void*)kTargetLeaf);
        MH_RemoveHook((void*)kTargetLeaf);
        g_installed = false;
    }
}

void LogStats() {
    if (!g_installed) return;
    Log("[CollisionBspLeaf] %llu leaves past the outcode test, %llu triangles in them",
        g_leaves, g_tris);
}

} // namespace CollisionBspLeaf
