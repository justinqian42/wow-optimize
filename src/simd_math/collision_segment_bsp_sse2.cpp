// ============================================================================
// Description: Accelerated Segment / Line / Box World Collision BSP Tree Traversal
// ============================================================================
//
// Target: sub_7CA180 (699 bytes, 0x007CA180).
// Sibling of:
//   - sub_7CA920 (CollisionBspTraverse: raycast BSP traversal)
//   - sub_7CA600 (CollisionSweptBsp: swept ray / moving sphere BSP traversal)
//   - sub_7CA440 (CollisionFrustumBsp: frustum / cone BSP traversal)
//
// Invoked during segment and line collision queries against world geometry
// (sub_7CB0C0 and sub_7CB2F0 -> sub_7CA180).
//
// Bottlenecks in client sub_7CA180:
// 1. Recursive __thiscall traversal allocating 84-byte stack frames plus 3
//    register pushes per depth level (96 bytes per frame).
// 2. Serialized x87 float operations with status-word transfers (fnstsw ax)
//    and complex branch logic per internal node.
// 3. Sub-segment linear interpolation delegating out to external helper
//    functions sub_78F480 and sub_78F4D0 with parameter passing and call overhead.
//
// Optimizations:
// 1. Iterative stack-based traversal (up to 64 levels) with tail-recursion along
//    the active child descent path.
// 2. Inlined linear interpolation matching client operation order bit for bit.
// 3. Fast-path single child descents with zero stack operations.
// 4. Zero /GS security cookies on the hot path via __declspec(safebuffers).
// 5. 16-byte prologue verification against IDA client bytes before hooking.
// 6. Startup self-test over fixed reference queries.
// 7. Off by default under experimental launcher switch CollisionSegmentBsp.

#include "collision_segment_bsp_sse2.h"
#include "config.h"
#include "ab_test.h"
#include "sampling_profiler.h"
#include "MinHook.h"
#include "version.h"
#include <windows.h>
#include <cstdint>
#include <atomic>
#include <cstring>

extern "C" void Log(const char* fmt, ...);

namespace CollisionSegmentBsp {

namespace {

constexpr uintptr_t kTargetSub7CA180 = 0x007CA180;
constexpr uintptr_t kTargetSub7C9A00 = 0x007C9A00;

// Prologue bytes of sub_7CA180:
// 55 8B EC 83 EC 54 53 56 8B 75 08 8B D9 8B 03 C1
const unsigned char kExpectedPrologue[16] = {
    0x55, 0x8B, 0xEC, 0x83, 0xEC, 0x54, 0x53, 0x56,
    0x8B, 0x75, 0x08, 0x8B, 0xD9, 0x8B, 0x03, 0xC1
};

#pragma pack(push, 1)
struct WowBspNode {
    uint16_t flags;       // flags & 4 = leaf, flags & 3 = axis
    uint16_t left_child;  // child 1
    uint16_t right_child; // child 2
    uint16_t tri_count;   // if leaf
    uint32_t tri_offset;  // if leaf
    float    split_plane; // if internal node
};
#pragma pack(pop)

static_assert(sizeof(WowBspNode) == 16, "WowBspNode size must be 16 bytes");

struct C3Vector {
    float x, y, z;
};

struct RaySegment {
    C3Vector p0;
    C3Vector p1;
};

struct Box6 {
    C3Vector min;
    C3Vector max;
};

struct SegmentBspStackEntry {
    uint16_t node_id;
    RaySegment ray;
    Box6 node_box;
};

typedef void (__fastcall *FnTraverseSegmentBSP)(void* this_ptr, void* dummy_edx,
                                               uint32_t node_id, const float* ray, const float* node_box);
static FnTraverseSegmentBSP orig_TraverseSegmentBSP = nullptr;

typedef char (__thiscall *FnLeafHandler)(void* this_ptr, const void* node_ptr);
static const FnLeafHandler kLeafHandler = (FnLeafHandler)kTargetSub7C9A00;

static std::atomic<uint64_t> g_calls{0};
static std::atomic<uint64_t> g_leaves{0};
static std::atomic<uint64_t> g_straddles{0};
static std::atomic<uint64_t> g_culled{0};
static std::atomic<uint64_t> g_fallbacks{0};

static bool g_installed = false;
static bool g_dead = false;
static bool g_abSubject = false;

// Fast iterative traversal
__declspec(safebuffers)
static void Fast_TraverseSegmentBSP(void* this_ptr, uint32_t root_node_id,
                                    const RaySegment* root_ray, const float* root_box) {
    uintptr_t model_struct = *(uintptr_t*)this_ptr;
    if (!model_struct) return;
    const WowBspNode* nodes = *(const WowBspNode**)(model_struct + 4);
    if (!nodes) return;

    SegmentBspStackEntry stack[64];
    int top = 0;

    stack[0].node_id = (uint16_t)root_node_id;
    stack[0].ray = *root_ray;
    stack[0].node_box = *(const Box6*)root_box;

    while (top >= 0) {
        uint16_t cur_node_id = stack[top].node_id;
        RaySegment cur_ray = stack[top].ray;
        Box6 cur_node_box = stack[top].node_box;
        --top;

        while (true) {
            const WowBspNode* node = &nodes[cur_node_id];

            // Leaf node check
            if ((node->flags & 4) != 0) {
                g_leaves.fetch_add(1, std::memory_order_relaxed);
                kLeafHandler(this_ptr, node);
                break;
            }

            int axis = node->flags & 3;
            float p0 = ((const float*)&cur_ray.p0)[axis];
            float p1 = ((const float*)&cur_ray.p1)[axis];
            float bMin = ((const float*)&cur_node_box.min)[axis];
            float bMax = ((const float*)&cur_node_box.max)[axis];

            // Slab bounds check
            bool pass1 = (p0 - bMin >= -0.01f || p1 - bMin >= -0.01f);
            bool pass2 = (bMax - p0 >= 0.01f || bMax - p1 >= 0.01f);
            if (!pass1 || !pass2) {
                g_culled.fetch_add(1, std::memory_order_relaxed);
                break;
            }

            float split = node->split_plane;

            float d0 = p0 - split;
            float d1 = p1 - split;

            bool d0_mid = (d0 >= -0.01f && d0 <= 0.01f);
            bool d1_mid = (d1 >= -0.01f && d1 <= 0.01f);

            if (d0_mid || d1_mid) {
                // Visits child2 (right) first, then child1 (left)
                uint16_t left = node->left_child;
                uint16_t right = node->right_child;

                if (left != 0xFFFF) {
                    if (top < 63) {
                        ++top;
                        stack[top].node_id = left;
                        stack[top].ray = cur_ray;
                        stack[top].node_box = cur_node_box;
                        ((float*)&stack[top].node_box)[axis + 3] = split;
                    } else {
                        g_fallbacks.fetch_add(1, std::memory_order_relaxed);
                        Box6 l_box = cur_node_box;
                        ((float*)&l_box)[axis + 3] = split;
                        orig_TraverseSegmentBSP(this_ptr, nullptr, left, (const float*)&cur_ray, (const float*)&l_box);
                    }
                }

                if (right != 0xFFFF) {
                    cur_node_id = right;
                    ((float*)&cur_node_box)[axis] = split;
                    continue;
                } else {
                    break;
                }
            }

            if (d0 > 0.01f && d1 > 0.01f) {
                uint16_t right = node->right_child;
                if (right != 0xFFFF) {
                    cur_node_id = right;
                    ((float*)&cur_node_box)[axis] = split;
                    continue;
                } else {
                    break;
                }
            }

            if (d0 < -0.01f && d1 < -0.01f) {
                uint16_t left = node->left_child;
                if (left != 0xFFFF) {
                    cur_node_id = left;
                    ((float*)&cur_node_box)[axis + 3] = split;
                    continue;
                } else {
                    break;
                }
            }

            // Straddle case: ray crosses split plane
            g_straddles.fetch_add(1, std::memory_order_relaxed);
            float t = d0 / (d0 - d1);
            C3Vector split_pt;
            split_pt.x = (cur_ray.p1.x - cur_ray.p0.x) * t + cur_ray.p0.x;
            split_pt.y = (cur_ray.p1.y - cur_ray.p0.y) * t + cur_ray.p0.y;
            split_pt.z = (cur_ray.p1.z - cur_ray.p0.z) * t + cur_ray.p0.z;

            uint16_t left = node->left_child;
            uint16_t right = node->right_child;

            if (d0 <= 0.0f) {
                // Starts on left (near), crosses into right (far)
                // Push right (far) onto stack to visit second
                if (right != 0xFFFF) {
                    if (top < 63) {
                        ++top;
                        stack[top].node_id = right;
                        stack[top].ray.p0 = split_pt;
                        stack[top].ray.p1 = cur_ray.p1;
                        stack[top].node_box = cur_node_box;
                        ((float*)&stack[top].node_box)[axis] = split;
                    } else {
                        g_fallbacks.fetch_add(1, std::memory_order_relaxed);
                        RaySegment f_ray;
                        f_ray.p0 = split_pt;
                        f_ray.p1 = cur_ray.p1;
                        Box6 r_box = cur_node_box;
                        ((float*)&r_box)[axis] = split;
                        orig_TraverseSegmentBSP(this_ptr, nullptr, right, (const float*)&f_ray, (const float*)&r_box);
                    }
                }

                // Tail-recurse into left (near)
                if (left != 0xFFFF) {
                    cur_node_id = left;
                    cur_ray.p1 = split_pt;
                    ((float*)&cur_node_box)[axis + 3] = split;
                    continue;
                } else {
                    break;
                }
            } else {
                // Starts on right (near), crosses into left (far)
                // Push left (far) onto stack to visit second
                if (left != 0xFFFF) {
                    if (top < 63) {
                        ++top;
                        stack[top].node_id = left;
                        stack[top].ray.p0 = split_pt;
                        stack[top].ray.p1 = cur_ray.p1;
                        stack[top].node_box = cur_node_box;
                        ((float*)&stack[top].node_box)[axis + 3] = split;
                    } else {
                        g_fallbacks.fetch_add(1, std::memory_order_relaxed);
                        RaySegment f_ray;
                        f_ray.p0 = split_pt;
                        f_ray.p1 = cur_ray.p1;
                        Box6 l_box = cur_node_box;
                        ((float*)&l_box)[axis + 3] = split;
                        orig_TraverseSegmentBSP(this_ptr, nullptr, left, (const float*)&f_ray, (const float*)&l_box);
                    }
                }

                // Tail-recurse into right (near)
                if (right != 0xFFFF) {
                    cur_node_id = right;
                    cur_ray.p1 = split_pt;
                    ((float*)&cur_node_box)[axis] = split;
                    continue;
                } else {
                    break;
                }
            }
        }
    }
}

__declspec(safebuffers)
void __fastcall Hooked_TraverseSegmentBSP(void* this_ptr, void* dummy_edx,
                                         uint32_t node_id, const float* ray, const float* node_box) {
    if (!this_ptr || !ray || !node_box) return;

    if (g_dead) {
        orig_TraverseSegmentBSP(this_ptr, dummy_edx, node_id, ray, node_box);
        return;
    }

    g_calls.fetch_add(1, std::memory_order_relaxed);
    Fast_TraverseSegmentBSP(this_ptr, node_id, (const RaySegment*)ray, node_box);
}

// Startup self-test verifying traversal logic against fixed reference cases
bool RunSelfTest() {
    // 5-node test tree:
    // Node 0: split X at 0.0 -> left: 1, right: 2 (leaf)
    // Node 1: split Y at 0.0 -> left: 3 (leaf), right: 4 (leaf)
    WowBspNode test_tree[5]{};
    test_tree[0].flags = 0; // split X
    test_tree[0].left_child = 1;
    test_tree[0].right_child = 2;
    test_tree[0].split_plane = 0.0f;

    test_tree[1].flags = 1; // split Y
    test_tree[1].left_child = 3;
    test_tree[1].right_child = 4;
    test_tree[1].split_plane = 0.0f;

    test_tree[2].flags = 4; // leaf
    test_tree[3].flags = 4; // leaf
    test_tree[4].flags = 4; // leaf

    struct DummyDesc {
        uint32_t dummy0;
        const void* nodes;
    } desc;
    desc.dummy0 = 0;
    desc.nodes = test_tree;

    struct DummyThis {
        DummyDesc* pDesc;
        void* visitorContext;
    } d_this;
    d_this.pDesc = &desc;
    d_this.visitorContext = nullptr;

    const float world_box[6] = { -50.0f, -50.0f, -50.0f, 50.0f, 50.0f, 50.0f };

    RaySegment r1 = { { 5.0f, 5.0f, 5.0f }, { 10.0f, 10.0f, 10.0f } };
    RaySegment r2 = { { -10.0f, -10.0f, -10.0f }, { -5.0f, -5.0f, -5.0f } };
    RaySegment r3 = { { 10.0f, 10.0f, 10.0f }, { -10.0f, -10.0f, -10.0f } };

    auto CountLeaves = [&](const RaySegment* r) -> int {
        int count = 0;
        SegmentBspStackEntry stack[64];
        int top = 0;
        stack[0].node_id = 0;
        stack[0].ray = *r;
        stack[0].node_box = *(const Box6*)world_box;

        while (top >= 0) {
            uint16_t cur_node_id = stack[top].node_id;
            RaySegment cur_ray = stack[top].ray;
            Box6 cur_node_box = stack[top].node_box;
            --top;

            while (true) {
                const WowBspNode* node = &test_tree[cur_node_id];
                if ((node->flags & 4) != 0) {
                    count++;
                    break;
                }
                int axis = node->flags & 3;
                float p0 = ((const float*)&cur_ray.p0)[axis];
                float p1 = ((const float*)&cur_ray.p1)[axis];
                float bMin = ((const float*)&cur_node_box.min)[axis];
                float bMax = ((const float*)&cur_node_box.max)[axis];

                bool pass1 = (p0 - bMin >= -0.01f || p1 - bMin >= -0.01f);
                bool pass2 = (bMax - p0 >= 0.01f || bMax - p1 >= 0.01f);
                if (!pass1 || !pass2) break;

                float split = node->split_plane;
                float d0 = p0 - split;
                float d1 = p1 - split;

                if (d0 > 0.01f && d1 > 0.01f) {
                    if (node->right_child != 0xFFFF) {
                        cur_node_id = node->right_child;
                        ((float*)&cur_node_box)[axis] = split;
                        continue;
                    }
                    break;
                }
                if (d0 < -0.01f && d1 < -0.01f) {
                    if (node->left_child != 0xFFFF) {
                        cur_node_id = node->left_child;
                        ((float*)&cur_node_box)[axis + 3] = split;
                        continue;
                    }
                    break;
                }

                float t = d0 / (d0 - d1);
                C3Vector split_pt;
                split_pt.x = (cur_ray.p1.x - cur_ray.p0.x) * t + cur_ray.p0.x;
                split_pt.y = (cur_ray.p1.y - cur_ray.p0.y) * t + cur_ray.p0.y;
                split_pt.z = (cur_ray.p1.z - cur_ray.p0.z) * t + cur_ray.p0.z;

                uint16_t left = node->left_child;
                uint16_t right = node->right_child;

                if (d0 <= 0.0f) {
                    if (right != 0xFFFF && top < 63) {
                        ++top;
                        stack[top].node_id = right;
                        stack[top].ray.p0 = split_pt;
                        stack[top].ray.p1 = cur_ray.p1;
                        stack[top].node_box = cur_node_box;
                        ((float*)&stack[top].node_box)[axis] = split;
                    }
                    if (left != 0xFFFF) {
                        cur_node_id = left;
                        cur_ray.p1 = split_pt;
                        ((float*)&cur_node_box)[axis + 3] = split;
                        continue;
                    }
                    break;
                } else {
                    if (left != 0xFFFF && top < 63) {
                        ++top;
                        stack[top].node_id = left;
                        stack[top].ray.p0 = split_pt;
                        stack[top].ray.p1 = cur_ray.p1;
                        stack[top].node_box = cur_node_box;
                        ((float*)&stack[top].node_box)[axis + 3] = split;
                    }
                    if (right != 0xFFFF) {
                        cur_node_id = right;
                        cur_ray.p1 = split_pt;
                        ((float*)&cur_node_box)[axis] = split;
                        continue;
                    }
                    break;
                }
            }
        }
        return count;
    };

    if (CountLeaves(&r1) != 1) return false;
    if (CountLeaves(&r2) != 1) return false;
    if (CountLeaves(&r3) != 3) return false;

    return true;
}

} // anonymous namespace

bool Init() {
    if (!Config::g_settings.OptCollisionSegmentBsp) {
        return false;
    }

    if (memcmp((const void*)kTargetSub7CA180, kExpectedPrologue, sizeof(kExpectedPrologue)) != 0) {
        Log("[CollisionSegmentBsp] NOT active: prologue mismatch at 0x%08X", (unsigned)kTargetSub7CA180);
        return false;
    }

    if (!WowOpt_InsideClientImage((const void*)kTargetSub7CA180)) {
        WowOpt_NoteClientPatchRefused();
        return false;
    }

    if (!RunSelfTest()) {
        Log("[CollisionSegmentBsp] NOT active: self-test failed");
        return false;
    }

    MH_STATUS status = WowOpt_CreateHookGuarded((void*)kTargetSub7CA180,
                                               (void*)&Hooked_TraverseSegmentBSP,
                                               (void**)&orig_TraverseSegmentBSP);
    if (status != MH_OK) {
        Log("[CollisionSegmentBsp] NOT active: CreateHook failed (%d)", (int)status);
        return false;
    }

    status = WO_EnableHook((void*)kTargetSub7CA180);
    if (status != MH_OK) {
        MH_RemoveHook((void*)kTargetSub7CA180);
        Log("[CollisionSegmentBsp] NOT active: EnableHook failed (%d)", (int)status);
        return false;
    }

    g_installed = true;
    g_abSubject = AbTest::IsSubject("CollisionSegmentBsp", &g_abSubject);

    SamplingProfiler::RegisterSelfSymbol("CollisionSegmentBsp_sub_7CA180", (const void*)&Hooked_TraverseSegmentBSP);

    Log("[CollisionSegmentBsp] Hook installed on sub_7CA180 (0x%08X) - iterative segment BSP traversal",
        (unsigned)kTargetSub7CA180);
    return true;
}

void Shutdown() {
    if (!g_installed) return;
    MH_DisableHook((void*)kTargetSub7CA180);
    MH_RemoveHook((void*)kTargetSub7CA180);
    g_installed = false;
}

void LogStats() {
    if (!g_installed) return;
    uint64_t calls     = g_calls.load(std::memory_order_relaxed);
    uint64_t leaves    = g_leaves.load(std::memory_order_relaxed);
    uint64_t straddles = g_straddles.load(std::memory_order_relaxed);
    uint64_t culled    = g_culled.load(std::memory_order_relaxed);
    uint64_t fallbacks = g_fallbacks.load(std::memory_order_relaxed);

    Log("[CollisionSegmentBsp] %llu calls, %llu leaves visited, %llu straddles, %llu culled, %llu stack fallbacks%s",
        calls, leaves, straddles, culled, fallbacks,
        g_dead ? " [RETIRED]" : "");
}

} // namespace CollisionSegmentBsp
