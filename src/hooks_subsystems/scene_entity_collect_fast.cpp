// ============================================================================
// Module: scene_entity_collect_fast.cpp
//
// sub_7A2760 (508 bytes) without its stack spills and duplicated branches.
//
// sub_7A2760 walks one spatial-hash cell list (a1: stride at +0, head at +8;
// a node's entity at +4, the next node at node + stride + 4, a zero or odd
// pointer ends it) and, for each entity not yet stamped with the current
// query frame (dword_CE04C4) and not excluded by its flags, links the
// entity's scene node into its parent's list at +0x114 (unless it is linked
// already), records a mode chosen from a2, and stamps the entity. An entity
// without a scene node is not stamped. Callers are sub_7A3570 and sub_7A30D0;
// neither reads the return value.
//
// Order kept from the client: the frame number and the list stride are read
// on every node, and the next node is read after the current one has been
// processed, so a body that writes the link it is walking still walks the
// same list. The prefetch reads the next pointer early only as a hint.
//
// A startup self-test builds random cell lists over a private arena (entities
// with and without scene nodes, stamped and excluded entities, scene nodes
// already linked, every mode bit of a2, duplicates) and compares the whole
// arena after this walk with the arena after sub_7A2760 transcribed from the
// disassembly. Not measured in game; off by default under the experimental
// switch SceneEntityCollect.
// ============================================================================

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <cstdint>
#include <cstring>
#include <immintrin.h>

#include "scene_entity_collect_fast.h"
#include "MinHook.h"
#include "version.h"
#include "config.h"
#include "sampling_profiler.h"
#include "ab_test.h"

extern "C" void Log(const char* fmt, ...);
MH_STATUS WineSafe_CreateHook(void* target, void* detour, void** original);
MH_STATUS WO_EnableHook(void* target);

namespace SceneEntityCollect {

namespace {

constexpr uintptr_t kTarget     = 0x007A2760;
constexpr uintptr_t kQueryFrame = 0x00CE04C4;   // dword_CE04C4

// Prologue bytes of sub_7A2760 (16 bytes):
// push ebp; mov ebp, esp; push ecx; mov eax, [ebp+arg_0]; mov eax, [eax+8]; push ebx; push esi; xor ebx, ebx; test al, 1
static const uint8_t kExpectedPrologue[16] = {
    0x55, 0x8B, 0xEC, 0x51, 0x8B, 0x45, 0x08, 0x8B,
    0x40, 0x08, 0x53, 0x56, 0x33, 0xDB, 0xA8, 0x01
};

typedef int (__cdecl* CollectFn)(const uint32_t* a1, uint32_t a2);
CollectFn orig_Collect = nullptr;

bool g_installed = false;
bool g_abSubject = false;

// Main-thread counter, read by the periodic report; a lower bound.
unsigned long long g_calls = 0;

template <typename T> __forceinline T& At(uintptr_t p, uint32_t off) { return *(T*)(p + off); }

// Links a scene node into its parent's list at +0x114 unless it is linked
// already (0x007A27FE and its three copies).
__forceinline void LinkIfUnlinked(uintptr_t node) {
    if (At<uint32_t>(node, 0x2D8) != 0) return;
    const uintptr_t head = At<uint32_t>(node, 0x28) + 0x114;
    At<uint32_t>(node, 0x2D8) = (uint32_t)head;
    At<uint32_t>(node, 0x2DC) = At<uint32_t>(head, 0);
    At<uint32_t>(head, 0) = (uint32_t)node;
    const uintptr_t old = At<uint32_t>(node, 0x2DC);
    if (old != 0) At<uint32_t>(old, 0x2D8) = (uint32_t)(node + 0x2DC);
}

__declspec(safebuffers) __forceinline
void Collect(const uint32_t* a1, uint32_t a2, const volatile uint32_t* frame) {
    uintptr_t curr = a1[2];
    while (curr != 0 && (curr & 1) == 0) {
        _mm_prefetch((const char*)(uintptr_t)At<uint32_t>(curr, a1[0] + 4), _MM_HINT_T0);

        const uintptr_t entity = At<uint32_t>(curr, 4);
        do {
            if ((a2 & 0x01000000) && At<uint8_t>(entity, 0x25) != 0) break;
            const uint32_t flags = At<uint32_t>(entity, 0x0C);
            if ((flags & 0x100) || (int8_t)(uint8_t)flags >= 0) break;
            if (At<uint32_t>(entity, 0x2C) == *frame) break;
            const uintptr_t node = At<uint32_t>(entity, 0x34);
            if (node == 0) break;

            int mode = -1;
            if ((At<uint32_t>(entity, 0xB8) | At<uint32_t>(entity, 0xBC)) != 0) {
                if (a2 & 0x00100000)      mode = 3;
                else if (a2 & 0x00600000) mode = 0;
            } else if (a2 & 1) {
                mode = 3;
            } else if (a2 & 0x0E) {
                mode = (a2 & 8) ? 2 : (int)((a2 >> 24) & 1);
            }
            if (mode >= 0 && (At<uint8_t>(node, 0x10) & 1)) {
                LinkIfUnlinked(node);
                At<uint32_t>(node, 0x2D4) = (uint32_t)mode;
                At<uint32_t>(node, 0x2E0) = (uint32_t)entity;
                At<uint32_t>(node, 0x2E4) = 0;
            }
            At<uint32_t>(entity, 0x2C) = *frame;                     // 0x007A2938
        } while (false);

        curr = At<uint32_t>(curr, a1[0] + 4);                        // 0x007A2943
    }
}

__declspec(safebuffers)
int __cdecl Hooked_CollectEntities(const uint32_t* a1, uint32_t a2) {
    if (g_abSubject && AbTest::StandAside()) return orig_Collect(a1, a2);
    g_calls++;
    Collect(a1, a2, (const volatile uint32_t*)kQueryFrame);
    return 0;
}

// ----------------------------------------------------------------------------
// Startup self-test
// ----------------------------------------------------------------------------

// sub_7A2760 as the disassembly at 0x007A2760 has it: every branch and its
// four copies of the list insertion, registers named as the client uses them.
void RefCollect(const uint32_t* arg0, uint32_t arg4, const volatile uint32_t* frame) {
    uintptr_t var4 = arg0[2];
    if (var4 & 1) var4 = 0;
    for (;;) {
        uintptr_t edx = var4;
        if ((edx & 1) || edx == 0) return;                          // 0x007A2783
        const uint32_t ecx = arg4;
        const uintptr_t esi = At<uint32_t>(edx, 4);
        bool stamp = false;
        if ((ecx & 0x1000000) && At<uint8_t>(esi, 0x25) != 0) goto next;
        {
            const uint32_t eax = At<uint32_t>(esi, 0x0C);
            if (eax & 0x100) goto next;
            if (!(eax & 0x80)) goto next;                            // test al, al; jns
            if (At<uint32_t>(esi, 0x2C) == *frame) goto next;
            const uintptr_t node = At<uint32_t>(esi, 0x34);
            if (node == 0) goto next;
            int mode = -1;
            if ((At<uint32_t>(esi, 0xB8) | At<uint32_t>(esi, 0xBC)) != 0) {
                if (ecx & 0x100000) mode = 3;                        // 0x007A27F4
                else if (ecx & 0x600000) mode = 0;                   // 0x007A283C
            } else {
                if (ecx & 1) mode = 3;                               // 0x007A288C
                else if (ecx & 0x0E) {
                    if (ecx & 8) mode = 2;                           // 0x007A28A3
                    else mode = 4;                                   // 0x007A28E8, below
                }
            }
            if (mode >= 0 && (At<uint8_t>(node, 0x10) & 1)) {
                if (At<uint32_t>(node, 0x2D8) == 0) {
                    const uintptr_t c = At<uint32_t>(node, 0x28) + 0x114;
                    At<uint32_t>(node, 0x2D8) = (uint32_t)c;
                    const uint32_t edi = At<uint32_t>(c, 0);
                    const uintptr_t d = node + 0x2DC;
                    At<uint32_t>(d, 0) = edi;
                    At<uint32_t>(c, 0) = (uint32_t)node;
                    const uint32_t c2 = At<uint32_t>(d, 0);
                    if (c2 != 0) At<uint32_t>(c2, 0x2D8) = (uint32_t)d;
                }
                At<uint32_t>(node, 0x2D4) = (mode == 4) ? ((arg4 >> 24) & 1) : (uint32_t)mode;
                At<uint32_t>(node, 0x2E0) = (uint32_t)esi;
                At<uint32_t>(node, 0x2E4) = 0;
            }
            stamp = true;
        }
    next:
        if (stamp) At<uint32_t>(esi, 0x2C) = *frame;
        var4 = At<uint32_t>(edx + arg0[0], 4);
    }
}

uint32_t g_rng = 0x2545F491u;
uint32_t Rng() { g_rng ^= g_rng << 13; g_rng ^= g_rng >> 17; g_rng ^= g_rng << 5; return g_rng; }

// Arena layout: list nodes, entities, scene nodes, parents.
constexpr uint32_t kNodes = 40, kNodeSize = 16, kStride = 8;
constexpr uint32_t kEnts = 16, kEntSize = 0xC0;
constexpr uint32_t kScenes = 8, kSceneSize = 0x300;
constexpr uint32_t kParents = 3, kParentSize = 0x120;
constexpr uint32_t kOffEnts   = kNodes * kNodeSize;
constexpr uint32_t kOffScenes = kOffEnts + kEnts * kEntSize;
constexpr uint32_t kOffPar    = kOffScenes + kScenes * kSceneSize;
constexpr uint32_t kArena     = kOffPar + kParents * kParentSize;

void BuildCase(uint8_t* m, uint32_t frame, uint32_t* a1) {
    memset(m, 0, kArena);
    const uintptr_t b = (uintptr_t)m;
    for (uint32_t p = 0; p < kParents; ++p)
        if (Rng() % 2) At<uint32_t>(b + kOffPar + p * kParentSize, 0x114) =
            (uint32_t)(b + kOffScenes + (Rng() % kScenes) * kSceneSize);
    for (uint32_t s = 0; s < kScenes; ++s) {
        const uintptr_t n = b + kOffScenes + s * kSceneSize;
        At<uint8_t>(n, 0x10) = (uint8_t)(Rng() % 4 ? 1 : 0);
        At<uint32_t>(n, 0x28) = (uint32_t)(b + kOffPar + (Rng() % kParents) * kParentSize);
        if (Rng() % 4 == 0) At<uint32_t>(n, 0x2D8) = 0x1234;       // linked already
        At<uint32_t>(n, 0x2D4) = 0xEEEE;
        At<uint32_t>(n, 0x2E0) = 0xCCCC;
        At<uint32_t>(n, 0x2E4) = 0xDDDD;
    }
    static const uint32_t kFlags[] = { 0x80, 0x80, 0x180, 0x00, 0x81, 0xFF, 0x7F };
    for (uint32_t e = 0; e < kEnts; ++e) {
        const uintptr_t ent = b + kOffEnts + e * kEntSize;
        At<uint32_t>(ent, 0x0C) = kFlags[Rng() % 7];
        At<uint8_t>(ent, 0x25) = (uint8_t)(Rng() % 3 == 0);
        At<uint32_t>(ent, 0x2C) = (Rng() % 5 == 0) ? frame : 0;
        At<uint32_t>(ent, 0x34) = (Rng() % 5 == 0) ? 0 :
            (uint32_t)(b + kOffScenes + (Rng() % kScenes) * kSceneSize);
        const uint32_t q = Rng() % 3;
        if (q == 1) At<uint32_t>(ent, 0xB8) = 1;
        if (q == 2) At<uint32_t>(ent, 0xBC) = 0x100;
    }
    const uint32_t len = 1 + Rng() % kNodes;
    for (uint32_t k = 0; k < len; ++k) {
        const uintptr_t n = b + k * kNodeSize;
        At<uint32_t>(n, 4) = (uint32_t)(b + kOffEnts + (Rng() % kEnts) * kEntSize);   // duplicates too
        At<uint32_t>(n, kStride + 4) = (k + 1 < len) ? (uint32_t)(n + kNodeSize)
                                                     : ((Rng() % 2) ? 0u : 0x10001u);
    }
    a1[0] = kStride;
    a1[1] = 0;
    a1[2] = (uint32_t)b;
}

bool RunSelfTest() {
    uint8_t* m = (uint8_t*)VirtualAlloc(nullptr, 2 * kArena, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!m) {
        Log("[SceneEntityCollect] NOT active: no memory for the startup self-test.");
        return false;
    }
    uint8_t* want = m + kArena;
    static const uint32_t kBits[] = { 0x1, 0x2, 0x4, 0x8, 0x100000, 0x200000, 0x400000, 0x1000000 };
    bool ok = true;
    int linked = 0;
    for (int c = 0; c < 400 && ok; ++c) {
        const uint32_t frame = 0x5000 + (uint32_t)c;
        uint32_t a2 = 0;
        for (uint32_t i = 0; i < 8; ++i) if (Rng() % 3 == 0) a2 |= kBits[i];
        uint32_t a1[3];
        const uint32_t seed = g_rng;
        BuildCase(m, frame, a1);
        RefCollect(a1, a2, &frame);
        memcpy(want, m, kArena);
        g_rng = seed;
        BuildCase(m, frame, a1);
        Collect(a1, a2, &frame);
        if (memcmp(m, want, kArena) != 0) {
            Log("[SceneEntityCollect] NOT active: case %d left the arena different from the "
                "transcription of sub_7A2760.", c);
            ok = false;
        }
        for (uint32_t s = 0; s < kScenes; ++s)
            if (At<uint32_t>((uintptr_t)m + kOffScenes + s * kSceneSize, 0x2D4) != 0xEEEE) ++linked;
    }
    VirtualFree(m, 0, MEM_RELEASE);
    if (ok && linked == 0) {
        Log("[SceneEntityCollect] NOT active: the self-test never recorded a scene node.");
        ok = false;
    }
    return ok;
}

} // anonymous namespace

bool Init() {
    if (!Config::g_settings.OptSceneEntityCollect) {
        return false;
    }

    if (memcmp((const void*)kTarget, kExpectedPrologue, sizeof(kExpectedPrologue)) != 0) {
        Log("[SceneEntityCollect] NOT active: prologue mismatch at 0x%08X", (unsigned)kTarget);
        return false;
    }

    if (!RunSelfTest()) {
        return false;
    }

    MH_STATUS status = WineSafe_CreateHook((void*)kTarget, (void*)&Hooked_CollectEntities, (void**)&orig_Collect);
    if (status != MH_OK) {
        Log("[SceneEntityCollect] NOT active: CreateHook failed at 0x%08X, status=%d", (unsigned)kTarget, status);
        return false;
    }

    status = WO_EnableHook((void*)kTarget);
    if (status != MH_OK) {
        MH_RemoveHook((void*)kTarget);
        Log("[SceneEntityCollect] NOT active: EnableHook failed at 0x%08X, status=%d", (unsigned)kTarget, status);
        return false;
    }

    g_installed = true;
    g_abSubject = AbTest::IsSubject("SceneEntityCollect", &g_abSubject);
    SamplingProfiler::RegisterSelfSymbol("SceneEntityCollect", (const void*)&Hooked_CollectEntities);
    Log("[SceneEntityCollect] ACTIVE on sub_7A2760 (0x%08X). Off by default, not measured in game.",
        (unsigned)kTarget);
    return true;
}

void Shutdown() {
    if (g_installed) {
        MH_DisableHook((void*)kTarget);
        g_installed = false;
    }
}

void LogStats() {
    if (g_installed) {
        Log("[SceneEntityCollect] %llu calls", g_calls);
    }
}

} // namespace SceneEntityCollect
