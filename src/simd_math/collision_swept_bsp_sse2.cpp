// ============================================================================
// Module: collision_swept_bsp_sse2.cpp
//
// Iterative walk of the collision tree for sub_7CB260's queries (sub_7CA600).
//
// sub_7CA600 is sub_7CA180's code instruction for instruction, 0x480 bytes
// further on, with sub_7C9AB0 as the leaf handler; this module is
// collision_segment_bsp_sse2.cpp with the names changed, and everything said
// there about the arithmetic, the NaN branches and the order holds here. The
// leaf handler reaches sub_7C6600, which appends to word_D25BF8 and keeps the
// nearest hit with <=, so the order of leaves is part of the answer.
//
// An earlier version did the slab test on min/max of the endpoints and the cut
// in single, and walked the far child through the client first when the stack
// filled. Its self-test ran a copy of the loop.
//
// Off by default under the experimental switch CollisionSweptBsp.
// ============================================================================

#include "collision_swept_bsp_sse2.h"
#include "MinHook.h"
#include "version.h"
#include "config.h"
#include "ab_test.h"
#include "sampling_profiler.h"

#include <windows.h>
#include <cstdint>
#include <cstring>

extern "C" void Log(const char* fmt, ...);
MH_STATUS WineSafe_CreateHook(void* target, void* detour, void** original);
MH_STATUS WO_EnableHook(void* target);

namespace CollisionSweptBsp {

namespace {

constexpr uintptr_t kTarget      = 0x007CA600;
constexpr uintptr_t kLeafHandler = 0x007C9AB0;

// Prologue bytes of sub_7CA600 (16 bytes):
// 55:          push ebp
// 8B EC:       mov ebp, esp
// 83 EC 54:    sub esp, 54h
// 53:          push ebx
// 56:          push esi
// 8B 75 08:    mov esi, [ebp+arg_0]
// 8B D9:       mov ebx, ecx
// 8B 03:       mov eax, [ebx]
// C1:          (shl esi, 4)
static const uint8_t kExpectedPrologue[16] = {
    0x55, 0x8B, 0xEC, 0x83, 0xEC, 0x54, 0x53, 0x56,
    0x8B, 0x75, 0x08, 0x8B, 0xD9, 0x8B, 0x03, 0xC1
};

#pragma pack(push, 1)
struct WowBspNode {
    uint16_t flags;       // & 4: leaf; & 3: axis (0=X, 1=Y, 2=Z)
    uint16_t left_child;  // 0xFFFF if none
    uint16_t right_child; // 0xFFFF if none
    uint16_t leaf_count;  // only if leaf
    uint32_t leaf_offset; // only if leaf
    float split_plane;    // coordinate along axis
};
#pragma pack(pop)
static_assert(sizeof(WowBspNode) == 16, "WowBspNode must be 16 bytes");

struct SweptBspStackEntry {
    uint32_t node_id;
    float    ray[6];       // p0, p1
    float    node_box[6];  // min, max
};

typedef void (__fastcall *FnTraverseSweptBSP)(void* this_ptr, void* dummy_edx,
                                               uint32_t node_id, const float* ray, const float* node_box);
static FnTraverseSweptBSP orig_TraverseSweptBSP = nullptr;

typedef char (__thiscall *FnLeafHandler)(void* this_ptr, const void* node_ptr);
static const FnLeafHandler call_leaf = (FnLeafHandler)kLeafHandler;

// Main-thread counters, read by the periodic report; a lower bound.
static unsigned long long g_calls  = 0;
static unsigned long long g_leaves = 0;
static unsigned long long g_handed = 0;

static bool g_installed = false;
static bool g_abSubject = false;

// flt_A104B4 and flt_9F1968, widened. Both are exact in double.
constexpr double kLo = (double)-0.01f;
constexpr double kHi = (double)0.01f;

// The traversal, written once: the hook passes the client's leaf handler and
// the client's own function for a node the stack cannot take, the self-test
// passes a recorder and the reference.
template <typename Visit, typename Hand>
__declspec(safebuffers) __forceinline void Walk(const WowBspNode* nodes, uint32_t root_node_id,
                        const float* root_ray, const float* root_box,
                        Visit visit, Hand hand_to_client) {
    SweptBspStackEntry stack[64];
    int top = 0;

    stack[0].node_id = root_node_id;
    for (int i = 0; i < 6; ++i) {
        stack[0].ray[i] = root_ray[i];
        stack[0].node_box[i] = root_box[i];
    }

    while (top >= 0) {
        SweptBspStackEntry cur = stack[top--];

        while (true) {
            const WowBspNode* node = &nodes[cur.node_id];

            if (node->flags & 4) {
                visit(node, cur.ray);
                break;
            }

            // Axis 3 would make the client read past both arrays. Whatever
            // it finds there, it finds in its own frame, so let it.
            const uint32_t axis = node->flags & 3;
            if (axis == 3) {
                hand_to_client(cur.node_id, cur.ray, cur.node_box);
                break;
            }

            const double p0   = cur.ray[axis];
            const double p1   = cur.ray[axis + 3];
            const double bmin = cur.node_box[axis];
            const double bmax = cur.node_box[axis + 3];

            // 0x007CA63B-0x007CA68D. A NaN difference passes.
            if ((p0 - bmin < kLo && p1 - bmin < kLo) ||
                (bmax - p0 < kHi && bmax - p1 < kHi)) {
                break;
            }

            const float  split = node->split_plane;
            const double d0 = p0 - (double)split;
            const double d1 = p1 - (double)split;
            const uint16_t left  = node->left_child;   // +2, box max = split
            const uint16_t right = node->right_child;  // +4, box min = split

            // Which child first, and the segment each one gets. Filled in by
            // the four cases below; `second` is 0xFFFF when there is only one.
            uint16_t first = 0xFFFF, second = 0xFFFF;
            bool first_is_right = false;
            bool cut = false;
            float s[3];

            if ((d0 >= kLo && d0 <= kHi) || (d1 >= kLo && d1 <= kHi)) {
                first = right; second = left; first_is_right = true;
            } else if (d0 > kHi && d1 > kHi) {
                first = right; first_is_right = true;
            } else if (d0 < kLo && d1 < kLo) {
                first = left;
            } else {
                // sub_78F4D0 with t rounded to float, as the client passes it.
                const float t = (float)(d0 / (d0 - d1));
                for (int k = 0; k < 3; ++k) {
                    const double a = cur.ray[k];
                    s[k] = (float)(((double)cur.ray[k + 3] - a) * (double)t + a);
                }
                cut = true;
                if ((float)d0 > 0.0f) {
                    first = right; second = left; first_is_right = true;
                } else {
                    first = left; second = right;
                }
            }

            if (first == 0xFFFF) {
                // Only the second child is there; it gets the far piece.
                if (second == 0xFFFF) break;
                first = second;
                second = 0xFFFF;
                first_is_right = !first_is_right;
                if (cut) for (int k = 0; k < 3; ++k) cur.ray[k] = s[k];
            } else if (second != 0xFFFF) {
                if (top >= 63) {
                    // Neither child has been visited yet.
                    hand_to_client(cur.node_id, cur.ray, cur.node_box);
                    break;
                }
                SweptBspStackEntry& far_e = stack[++top];
                far_e.node_id = second;
                for (int i = 0; i < 6; ++i) {
                    far_e.ray[i] = cur.ray[i];
                    far_e.node_box[i] = cur.node_box[i];
                }
                if (cut) for (int k = 0; k < 3; ++k) far_e.ray[k] = s[k];
                if (first_is_right) far_e.node_box[axis + 3] = split;
                else                far_e.node_box[axis] = split;
                if (cut) for (int k = 0; k < 3; ++k) cur.ray[k + 3] = s[k];
            } else if (cut) {
                for (int k = 0; k < 3; ++k) cur.ray[k + 3] = s[k];
            }

            cur.node_id = first;
            if (first_is_right) cur.node_box[axis] = split;
            else                cur.node_box[axis + 3] = split;
        }
    }
}

__declspec(safebuffers)
void __fastcall Hooked_TraverseSweptBSP(void* this_ptr, void* dummy_edx,
                                         uint32_t node_id, const float* ray, const float* node_box) {
    if (g_abSubject && AbTest::StandAside()) {
        orig_TraverseSweptBSP(this_ptr, dummy_edx, node_id, ray, node_box);
        return;
    }
    const uintptr_t model_struct = *(const uintptr_t*)this_ptr;
    const WowBspNode* nodes = model_struct ? *(const WowBspNode* const*)(model_struct + 4) : nullptr;
    if (!nodes) {
        // The client makes the same null checks, or faults; either way it is
        // the client's answer.
        orig_TraverseSweptBSP(this_ptr, dummy_edx, node_id, ray, node_box);
        return;
    }

    g_calls++;
    Walk(nodes, node_id, ray, node_box,
         [this_ptr](const WowBspNode* leaf, const float*) {
             g_leaves++;
             call_leaf(this_ptr, leaf);
         },
         [this_ptr](uint32_t id, const float* r, const float* b) {
             g_handed++;
             orig_TraverseSweptBSP(this_ptr, nullptr, id, r, b);
         });
}

// sub_7CA600 transcribed branch by branch from the disassembly, recursive, in
// double. It shares nothing with Walk but the node layout. Each leaf is
// recorded with the segment that reached it, so a cut point one bit off fails.
struct LeafLog {
    uint32_t idx[512];
    float    ray[512][6];
    uint32_t count;
    bool     overflow;
};

void Record(LeafLog& log, uint32_t id, const float* ray) {
    if (log.count >= 512) { log.overflow = true; return; }
    log.idx[log.count] = id;
    memcpy(log.ray[log.count], ray, sizeof(float) * 6);
    ++log.count;
}

void RefSwept(const WowBspNode* nodes, uint32_t a2, const float* a3, const float* a4,
                LeafLog& log) {
    const WowBspNode* v4 = &nodes[a2];
    if (v4->flags & 4) { Record(log, a2, a3); return; }            // 0x007CA624
    const uint32_t v5 = v4->flags & 3;
    const double split = v4->split_plane;

    // 0x007CA647 fcom; test ah,41h jnz: d >= -0.01 or unordered passes.
    // 0x007CA659 fcomp; test ah,5 jnp: fails only on an ordered d < -0.01.
    const double e0 = (double)a3[v5] - a4[v5];
    const double e1 = (double)a3[v5 + 3] - a4[v5];
    if (!(e0 >= kLo || e0 != e0) && e1 < kLo) return;
    const double f0 = (double)a4[v5 + 3] - a3[v5];                 // 0x007CA666
    const double f1 = (double)a4[v5 + 3] - a3[v5 + 3];
    if (!(f0 >= kHi || f0 != f0) && f1 < kHi) return;

    float v23[6], v24[6];                                          // +4 box, +2 box
    for (int i = 0; i < 6; ++i) { v23[i] = a4[i]; v24[i] = a4[i]; }
    v23[v5] = (float)split;
    v24[v5 + 3] = (float)split;

    const double v8 = (double)a3[v5] - split;                      // 0x007CA6EA
    const float v27 = (float)v8;                                   // fst [ebp+arg_0]
    const double v9 = (double)a3[v5 + 3] - split;

    // 0x007CA6F9-0x007CA71D: test ah,1 jnz takes an unordered d to the next
    // test, so a NaN is never "within 0.01".
    bool mid = false;
    if (!(v8 < kLo || v8 != v8) && v8 <= kHi) mid = true;
    else if (!(v9 < kLo || v9 != v9) && v9 <= kHi) mid = true;
    if (mid) {
        if (v4->right_child != 0xFFFF) RefSwept(nodes, v4->right_child, a3, v23, log);
        if (v4->left_child != 0xFFFF)  RefSwept(nodes, v4->left_child, a3, v24, log);
        return;
    }
    if (v8 > kHi && v9 > kHi) {                                    // 0x007CA749-0x007CA75B
        if (v4->right_child != 0xFFFF) RefSwept(nodes, v4->right_child, a3, v23, log);
        return;
    }
    if (v8 < kLo && v9 < kLo) {                                    // 0x007CA78E-0x007CA7A0
        if (v4->left_child != 0xFFFF) RefSwept(nodes, v4->left_child, a3, v24, log);
        return;
    }

    // 0x007CA7CF fsubr st,st(1); fdivp: v8 / (v8 - v9), stored as float.
    const float t = (float)(v8 / (v8 - v9));
    float v26[3];                                                  // sub_78F4D0
    for (int k = 0; k < 3; ++k)
        v26[k] = (float)(((double)a3[k + 3] - a3[k]) * t + a3[k]);
    float v22[6];

    // 0x007CA7E2 fldz; fcomp v27; test ah,5 jp: only 0 < v27 falls through.
    if (0.0f < v27) {
        if (v4->right_child != 0xFFFF) {
            for (int k = 0; k < 3; ++k) { v22[k] = a3[k]; v22[k + 3] = v26[k]; }
            RefSwept(nodes, v4->right_child, v22, v23, log);
        }
        if (v4->left_child != 0xFFFF) {
            for (int k = 0; k < 3; ++k) { v22[k] = v26[k]; v22[k + 3] = a3[k + 3]; }
            RefSwept(nodes, v4->left_child, v22, v24, log);
        }
    } else {
        if (v4->left_child != 0xFFFF) {
            for (int k = 0; k < 3; ++k) { v22[k] = a3[k]; v22[k + 3] = v26[k]; }
            RefSwept(nodes, v4->left_child, v22, v24, log);
        }
        if (v4->right_child != 0xFFFF) {
            for (int k = 0; k < 3; ++k) { v22[k] = v26[k]; v22[k + 3] = a3[k + 3]; }
            RefSwept(nodes, v4->right_child, v22, v23, log);
        }
    }
}

uint32_t g_rng = 0x2545F491u;
uint32_t Rng() { g_rng ^= g_rng << 13; g_rng ^= g_rng >> 17; g_rng ^= g_rng << 5; return g_rng; }
float RngF(float lo, float hi) { return lo + (hi - lo) * (float)(Rng() & 0xFFFFFF) / 16777215.0f; }

// A random tree with children always at higher indices, or with `chain` set a
// run of nodes that all split X at 0 with the segment starting on the plane,
// so every level is "within 0.01" and pushes a node: deep enough to fill the
// stack.
uint32_t BuildTree(WowBspNode* t, uint32_t cap, bool chain) {
    for (uint32_t i = 0; i < cap; ++i) t[i] = WowBspNode{};
    if (chain) {
        const uint32_t k = (cap - 1) / 2;
        for (uint32_t i = 0; i < k; ++i) {
            t[i].flags = 0;
            t[i].split_plane = 0.0f;
            t[i].right_child = (uint16_t)(i + 1 < k ? i + 1 : k);
            t[i].left_child  = (uint16_t)(k + 1 + i);
        }
        for (uint32_t i = k; i < cap; ++i) t[i].flags = 4;
        return cap;
    }
    const uint32_t count = 8 + Rng() % (cap - 8);
    for (uint32_t i = 0; i < count; ++i) {
        const bool leaf = (i * 2 + 1 >= count) || (Rng() % 5 == 0);
        if (leaf) { t[i].flags = 4; continue; }
        t[i].flags = (uint16_t)(Rng() % 3);
        t[i].split_plane = RngF(-10.0f, 10.0f);
        const uint32_t span = count - (i + 1);
        t[i].left_child  = (Rng() % 7 == 0) ? 0xFFFF : (uint16_t)(i + 1 + Rng() % span);
        t[i].right_child = (Rng() % 7 == 0) ? 0xFFFF : (uint16_t)(i + 1 + Rng() % span);
    }
    return count;
}

// Walk against the transcription: random trees and segments, some with a NaN
// coordinate, and the chain, where Walk has to hand a node to the "client".
bool RunSelfTest() {
    static WowBspNode tree[200];
    static LeafLog want, got;
    for (int c = 0; c < 400; ++c) {
        const bool chain = (c == 0);
        BuildTree(tree, chain ? 199 : 200, chain);
        float r[6], nb[6];
        for (int i = 0; i < 6; ++i) r[i] = RngF(-12.0f, 12.0f);
        for (int i = 0; i < 3; ++i) { nb[i] = -10.0f; nb[i + 3] = 10.0f; }
        if (chain) { r[0] = 0.0f; r[3] = 5.0f; }
        // A NaN in the segment, or in a split near the root: the client's
        // compares send it down particular branches, and those must match.
        const uint32_t nan = 0x7FC00000u;
        if (c % 8 == 1) memcpy(&r[Rng() % 6], &nan, 4);
        if (c % 8 == 2) memcpy(&tree[Rng() % 4].split_plane, &nan, 4);

        want.count = 0; want.overflow = false;
        RefSwept(tree, 0, r, nb, want);
        got.count = 0; got.overflow = false;
        int handed = 0;
        Walk(tree, 0, r, nb,
             [&](const WowBspNode* leaf, const float* ray) {
                 Record(got, (uint32_t)(leaf - tree), ray);
             },
             [&](uint32_t id, const float* rr, const float* bb) {
                 ++handed;
                 RefSwept(tree, id, rr, bb, got);
             });
        if (want.overflow || got.overflow || want.count != got.count ||
            memcmp(want.idx, got.idx, want.count * sizeof(uint32_t)) != 0 ||
            memcmp(want.ray, got.ray, want.count * sizeof(want.ray[0])) != 0) {
            Log("[CollisionSweptBsp] NOT active: case %d reached %u leaves where the "
                "transcription of the client reaches %u, or in another order, or with "
                "another segment.", c, got.count, want.count);
            return false;
        }
        if (chain && handed == 0) {
            Log("[CollisionSweptBsp] NOT active: the deep test tree never filled the "
                "stack, so the hand-over to the client went untested.");
            return false;
        }
    }
    return true;
}

} // anonymous namespace

bool Init() {
    if (!Config::g_settings.OptCollisionSweptBsp) {
        return false;
    }

    if (Config::g_settings.OptNoClientPatches) {
        Log("[CollisionSweptBsp] Client binary patches disabled by OptNoClientPatches, skipping.");
        return false;
    }

    if (g_installed) {
        return true;
    }

    // Verify prologue bytes at sub_7CA600
    if (std::memcmp((const void*)kTarget, kExpectedPrologue, sizeof(kExpectedPrologue)) != 0) {
        Log("[CollisionSweptBsp] Refusing to hook 0x%08X: prologue bytes mismatch", kTarget);
        return false;
    }

    // Run startup self-test
    if (!RunSelfTest()) {
        return false;
    }

    MH_STATUS status = WineSafe_CreateHook((void*)kTarget, (void*)Hooked_TraverseSweptBSP, (void**)&orig_TraverseSweptBSP);
    if (status != MH_OK) {
        Log("[CollisionSweptBsp] WineSafe_CreateHook failed with %d", status);
        return false;
    }

    status = WO_EnableHook((void*)kTarget);
    if (status != MH_OK) {
        Log("[CollisionSweptBsp] WO_EnableHook failed with %d", status);
        return false;
    }

    g_installed = true;
    g_abSubject = AbTest::IsSubject("CollisionSweptBsp", &g_abSubject);

    SamplingProfiler::RegisterSelfSymbol("CollisionSweptBsp", (const void*)&Hooked_TraverseSweptBSP);

    Log("[CollisionSweptBsp] ACTIVE on sub_7CA600 (iterative swept-ray BSP tree traversal, 699 bytes, off by default).");
    return true;
}

void Shutdown() {
    if (!g_installed) return;
    MH_DisableHook((void*)kTarget);
    g_installed = false;
    Log("[CollisionSweptBsp] Shutdown complete");
}

void LogStats() {
    if (!g_installed) return;
    Log("[CollisionSweptBsp] %llu calls, %llu leaves, %llu nodes handed to the client "
        "(stack full)", g_calls, g_leaves, g_handed);
}

} // namespace CollisionSweptBsp
