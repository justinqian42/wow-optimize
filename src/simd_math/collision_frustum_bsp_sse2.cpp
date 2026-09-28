// ============================================================================
// Module: collision_frustum_bsp_sse2.cpp
//
// Iterative world collision BSP tree traversal accelerating sub_7CA440
// (frustum/cone collision queries).
//
// In world collision detection for frustum and cone queries (sub_7CB180 ->
// sub_7CA440), sub_7CA440 traverses BSP trees to classify candidate triangles
// in leaves via sub_7C9A60 for sub_7C7660 (CollisionTriTest, sampled 2,893
// times at 0x007C76C1, 1.15% CPU time).
//
// Bottlenecks in the client implementation (0x007CA440 -> 0x007CA5F2, 434 bytes):
//   1. Recursive function overhead: Allocates 76 bytes of stack frame per node
//      plus 3 register saves (88 bytes total frame per depth level). On deep
//      trees 16 to 24 levels deep, this constantly creates and destroys call frames.
//   2. Serialized x87 status-word transfers: Each internal node executes up to
//      four separate x87 comparisons (fcomp / fnstsw ax / test ah, 41h / test ah, 5)
//      to evaluate bounding box overlaps and split plane classification, stalling
//      the execution pipeline on floating-point status word transfers.
//   3. Redundant 24-byte box copying: The client scalar-copies 6 floats for the
//      left and right child bounding boxes on the stack for every candidate branch.
//
// This replacement:
//   - Transforms the recursive tree walk into an iterative traversal with an
//     explicit small stack (up to 64 levels) and tail recursion along the active
//     child path (0 pushes/pops on single-child descents, 1 push on straddles).
//   - Evaluates box overlap and split-plane comparisons directly in IEEE single
//     precision, eliminating all x87 status-word transfers and pipeline stalls.
//   - Disassembly audit confirms 0 /GS security cookies and 0 SEH frames on the
//     hot path via __declspec(safebuffers).
//   - At startup, before hooking, the shipped traversal is run against a
//     recursive transcription of sub_7CA440 on 400 random trees and queries,
//     and the full sequence of leaves must match, order included. One of
//     the trees is deep enough to fill the 64-entry stack, so the hand-over
//     to the client is exercised too. Any difference and nothing is hooked.
//     The earlier self-test ran its own copy of the loop and compared leaf
//     counts only; it could not see the ordering bug fixed on 2026-09-28,
//     and this one fails on it. Not run in a game.
//   - Off by default under experimental launcher switch CollisionFrustumBsp.
// ============================================================================

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <cstdint>
#include <cstring>

#include "collision_frustum_bsp_sse2.h"
#include "MinHook.h"
#include "version.h"
#include "config.h"
#include "ab_test.h"
#include "sampling_profiler.h"

extern "C" void Log(const char* fmt, ...);
MH_STATUS WineSafe_CreateHook(void* target, void* detour, void** original);
MH_STATUS WO_EnableHook(void* target);

namespace CollisionFrustumBsp {

namespace {

constexpr uintptr_t kTarget      = 0x007CA440;
constexpr uintptr_t kLeafHandler = 0x007C9A60;

// Prologue bytes of sub_7CA440 (16 bytes):
// 55:          push ebp
// 8B EC:       mov ebp, esp
// 83 EC 4C:    sub esp, 4Ch
// 8B 01:       mov eax, [ecx]
// 53:          push ebx
// 57:          push edi
// 8B 7D 08:    mov edi, [ebp+arg_0]
// C1 E7 04:    shl edi, 4
// 03 78 04:    add edi, [eax+4]
static const uint8_t kExpectedPrologue[16] = {
    0x55, 0x8B, 0xEC, 0x83, 0xEC, 0x4C, 0x8B, 0x01,
    0x53, 0x57, 0x8B, 0x7D, 0x08, 0xC1, 0xE7, 0x04
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

struct BspStackEntry {
    uint32_t node_id;
    float query_box[6];
    float node_box[6];
};

typedef void (__fastcall* TraverseFrustumBSP_fn)(void* this_ptr, void* dummy_edx,
                                                 uint32_t node_id, const float* query_box, const float* node_box);
typedef void (__thiscall* LeafHandler_fn)(void* this_ptr, const void* node_ptr);

TraverseFrustumBSP_fn orig_TraverseFrustumBSP = nullptr;
const auto call_leaf = (LeafHandler_fn)kLeafHandler;

bool g_installed = false;
bool g_abSubject = false;

unsigned long long g_calls    = 0;
unsigned long long g_leaves   = 0;
unsigned long long g_fallbacks = 0;

// The traversal, written once. The hook passes the client's own leaf handler
// and the client's own function for the case the stack cannot take; the
// self-test passes a recorder and a reference. Before this the self-test ran a
// copy of the loop, which proved only that the copy agreed with itself.
//
// Order is the contract. sub_7CA440 recurses into the child at +4 before the
// child at +2 when the query straddles the split, and the leaf handler it calls
// appends to a list the caller reads in that order. Pushing the +2 child and
// continuing into the +4 child keeps the same depth-first order.
template <typename Visit, typename Hand>
__declspec(safebuffers) __forceinline void Walk(const uint8_t* bsp_nodes, uint32_t root_node_id,
                        const float* root_query_box, const float* root_node_box,
                        Visit visit, Hand hand_to_client) {
    BspStackEntry stack[64];
    int top = 0;

    stack[0].node_id = root_node_id;
    for (int i = 0; i < 6; ++i) {
        stack[0].query_box[i] = root_query_box[i];
        stack[0].node_box[i] = root_node_box[i];
    }

    while (top >= 0) {
        BspStackEntry cur = stack[top--];

        while (true) {
            const WowBspNode* node = (const WowBspNode*)(bsp_nodes + 16 * cur.node_id);

            if (node->flags & 4) {
                visit(node);
                break;
            }

            uint32_t axis = node->flags & 3;
            float node_min = cur.node_box[axis];
            float query_max = cur.query_box[axis + 3];
            float node_max = cur.node_box[axis + 3];
            float query_min = cur.query_box[axis];

            // Overlap check
            if (node_min > query_max || node_max < query_min) {
                break;
            }

            float split = node->split_plane;

            if (split < query_min) {
                // Right child only (tail recurse)
                uint16_t right = node->right_child;
                if (right == 0xFFFF) break;
                cur.node_id = right;
                cur.node_box[axis] = split;
                continue;
            } else if (split > query_max) {
                // Left child only (tail recurse)
                uint16_t left = node->left_child;
                if (left == 0xFFFF) break;
                cur.node_id = left;
                cur.node_box[axis + 3] = split;
                continue;
            } else {
                // Straddle: the client visits RIGHT first, then LEFT.
                uint16_t left = node->left_child;
                uint16_t right = node->right_child;

                if (left != 0xFFFF) {
                    if (top < 63) {
                        ++top;
                        stack[top].node_id = left;
                        for (int i = 0; i < 6; ++i) {
                            stack[top].query_box[i] = cur.query_box[i];
                            stack[top].node_box[i] = cur.node_box[i];
                        }
                        stack[top].query_box[axis + 3] = split;
                        stack[top].node_box[axis + 3] = split;
                    } else {
                        // Stack full. This used to walk LEFT through the
                        // client at once and then carry on into RIGHT, which
                        // is the reverse of the client's order. Neither child
                        // has been visited yet, so the whole node goes to the
                        // client, which walks both in its own order.
                        hand_to_client(cur.node_id, cur.query_box, cur.node_box);
                        break;
                    }
                }

                if (right != 0xFFFF) {
                    cur.node_id = right;
                    cur.query_box[axis] = split;
                    cur.node_box[axis] = split;
                    continue;
                } else {
                    break;
                }
            }
        }
    }
}

__declspec(safebuffers)
void Fast_TraverseFrustumBSP(void* this_ptr, uint32_t root_node_id, const float* root_query_box, const float* root_node_box) {
    void* bsp_desc = *(void**)this_ptr;
    if (!bsp_desc) return;
    const uint8_t* bsp_nodes = *(const uint8_t**)((uint8_t*)bsp_desc + 4);
    if (!bsp_nodes) return;

    Walk(bsp_nodes, root_node_id, root_query_box, root_node_box,
         [this_ptr](const WowBspNode* leaf) {
             g_leaves++;
             call_leaf(this_ptr, leaf);
         },
         [this_ptr](uint32_t id, const float* q, const float* nb) {
             g_fallbacks++;
             orig_TraverseFrustumBSP(this_ptr, nullptr, id, q, nb);
         });
}

__declspec(safebuffers)
void __fastcall Hooked_TraverseFrustumBSP(void* this_ptr, void* dummy_edx,
                                          uint32_t node_id, const float* query_box, const float* node_box) {
    if (!this_ptr || !query_box || !node_box) return;

    if (g_abSubject && AbTest::StandAside()) {
        orig_TraverseFrustumBSP(this_ptr, dummy_edx, node_id, query_box, node_box);
        return;
    }

    g_calls++;
    Fast_TraverseFrustumBSP(this_ptr, node_id, query_box, node_box);
}

// sub_7CA440 transcribed as it is written - recursive, from the decompiled
// body at 0x007CA440. It has the same body as sub_7CA920 with a different
// leaf handler - so that it shares nothing with Walk but the node
// layout. Records leaf indices instead of calling the leaf handler.
struct LeafLog {
    uint32_t idx[512];
    uint32_t count;
    bool     overflow;
};

void RefTraverse(const WowBspNode* nodes, uint32_t id, const float* a3, const float* a4,
                 LeafLog& log) {
    const WowBspNode* v4 = &nodes[id];
    if (v4->flags & 4) {
        if (log.count < 512) log.idx[log.count++] = id; else log.overflow = true;
        return;
    }
    const uint32_t v5 = v4->flags & 3;
    // 0x007CA479 test ah,41h jz and 0x007CA48C test ah,5 jnp: only an ordered
    // compare leaves, so a NaN on either side carries on.
    if (a4[v5] > a3[v5 + 3] || a4[v5 + 3] < a3[v5]) return;
    float v41[6], v39[6];
    for (int i = 0; i < 6; ++i) { v41[i] = a4[i]; v39[i] = a4[i]; }
    v41[v5] = v4->split_plane;          // box for the child at +4
    v39[v5 + 3] = v4->split_plane;      // box for the child at +2
    // 0x007CA4F1 test ah,5 jp: only an ordered split < min goes to +4 alone.
    if (v4->split_plane < a3[v5]) {
        if (v4->right_child != 0xFFFF) RefTraverse(nodes, v4->right_child, a3, v41, log);
        return;
    }
    // 0x007CA526 test ah,41h jnz: only an ordered split > max goes to +2 alone.
    if (v4->split_plane > a3[v5 + 3]) {
        if (v4->left_child != 0xFFFF) RefTraverse(nodes, v4->left_child, a3, v39, log);
        return;
    }
    // Straddling, or a NaN on either side of either compare.
    if (v4->right_child != 0xFFFF) {
        float q[6];
        for (int i = 0; i < 6; ++i) q[i] = a3[i];
        q[v5] = v4->split_plane;
        RefTraverse(nodes, v4->right_child, q, v41, log);
    }
    if (v4->left_child != 0xFFFF) {
        float q[6];
        for (int i = 0; i < 6; ++i) q[i] = a3[i];
        q[v5 + 3] = v4->split_plane;
        RefTraverse(nodes, v4->left_child, q, v39, log);
    }
}

// A random tree, children always at higher indices so it cannot loop. With
// `chain` set, node i straddles into node i + 1 on its +4 side and a leaf on
// its +2 side all the way down, deep enough to fill the 64-entry stack.
uint32_t g_rng = 0x2545F491u;
uint32_t Rng() { g_rng ^= g_rng << 13; g_rng ^= g_rng >> 17; g_rng ^= g_rng << 5; return g_rng; }
float RngF(float lo, float hi) { return lo + (hi - lo) * (float)(Rng() & 0xFFFF) / 65535.0f; }

uint32_t BuildTree(WowBspNode* t, uint32_t cap, bool chain) {
    for (uint32_t i = 0; i < cap; ++i) t[i] = WowBspNode{};
    if (chain) {
        // 0..k-1 internal, each straddling on X at 0; odd leaves hang off +2.
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

// Walk against the transcription on random trees and queries, comparing the
// full sequence of leaves, plus one tree deep enough to reach the stack limit,
// where Walk hands a node to the "client" - here the transcription itself.
bool RunSelfTest() {
    static WowBspNode tree[200];
    static LeafLog want, got;
    for (int c = 0; c < 400; ++c) {
        const bool chain = (c == 0);
        BuildTree(tree, chain ? 199 : 200, chain);
        float q[6], nb[6];
        for (int i = 0; i < 3; ++i) {
            const float x = RngF(-12.0f, 12.0f), y = RngF(-12.0f, 12.0f);
            q[i] = x < y ? x : y;  q[i + 3] = x < y ? y : x;
            nb[i] = -10.0f;        nb[i + 3] = 10.0f;
        }
        if (chain) { q[0] = -5.0f; q[3] = 5.0f; }
        // A NaN in the query or in a split near the root: the client's
        // compares send it down particular branches, and those must match.
        const uint32_t nan = 0x7FC00000u;
        if (c % 8 == 1) memcpy(&q[Rng() % 6], &nan, 4);
        if (c % 8 == 2) memcpy(&tree[Rng() % 4].split_plane, &nan, 4);

        want.count = 0; want.overflow = false;
        RefTraverse(tree, 0, q, nb, want);
        got.count = 0; got.overflow = false;
        int handed = 0;
        Walk((const uint8_t*)tree, 0, q, nb,
             [&](const WowBspNode* leaf) {
                 const uint32_t id = (uint32_t)(leaf - tree);
                 if (got.count < 512) got.idx[got.count++] = id; else got.overflow = true;
             },
             [&](uint32_t id, const float* qq, const float* nn) {
                 ++handed;
                 RefTraverse(tree, id, qq, nn, got);
             });
        if (want.overflow || got.overflow || want.count != got.count ||
            memcmp(want.idx, got.idx, want.count * sizeof(uint32_t)) != 0) {
            Log("[CollisionFrustumBsp] NOT active: case %d visited %u leaves where the "
                "transcription of the client visits %u, or in a different order.",
                c, got.count, want.count);
            return false;
        }
        if (chain && handed == 0) {
            Log("[CollisionFrustumBsp] NOT active: the deep test tree never reached the "
                "stack limit, so the hand-over to the client went untested.");
            return false;
        }
    }
    return true;
}

} // anonymous namespace

bool Init() {
    if (!Config::g_settings.OptCollisionFrustumBsp) {
        return false;
    }

    if (Config::g_settings.OptNoClientPatches) {
        Log("[CollisionFrustumBsp] Client binary patches disabled by OptNoClientPatches, skipping.");
        return false;
    }

    if (g_installed) {
        return true;
    }

    // Verify prologue bytes at sub_7CA440
    if (std::memcmp((const void*)kTarget, kExpectedPrologue, sizeof(kExpectedPrologue)) != 0) {
        Log("[CollisionFrustumBsp] Refusing to hook 0x%08X: prologue bytes mismatch", kTarget);
        return false;
    }

    // Run startup self-test
    if (!RunSelfTest()) {
        Log("[CollisionFrustumBsp] Refusing to hook 0x%08X: self-test failed", kTarget);
        return false;
    }

    MH_STATUS status = WineSafe_CreateHook((void*)kTarget, (void*)Hooked_TraverseFrustumBSP, (void**)&orig_TraverseFrustumBSP);
    if (status != MH_OK) {
        Log("[CollisionFrustumBsp] WineSafe_CreateHook failed with %d", status);
        return false;
    }

    status = WO_EnableHook((void*)kTarget);
    if (status != MH_OK) {
        Log("[CollisionFrustumBsp] WO_EnableHook failed with %d", status);
        return false;
    }

    g_installed = true;
    g_abSubject = AbTest::IsSubject("CollisionFrustumBsp", &g_abSubject);

    SamplingProfiler::RegisterSelfSymbol("CollisionFrustumBsp", (const void*)&Hooked_TraverseFrustumBSP);

    Log("[CollisionFrustumBsp] ACTIVE on sub_7CA440 (iterative frustum BSP tree traversal, 434 bytes, off by default).");
    return true;
}

void Shutdown() {
    if (!g_installed) return;
    MH_DisableHook((void*)kTarget);
    g_installed = false;
    Log("[CollisionFrustumBsp] Shutdown complete");
}

void LogStats() {
    if (!g_installed) return;
    Log("[CollisionFrustumBsp] Calls: %llu, Leaves visited: %llu, Stack fallbacks: %llu",
        g_calls, g_leaves, g_fallbacks);
}

} // namespace CollisionFrustumBsp
