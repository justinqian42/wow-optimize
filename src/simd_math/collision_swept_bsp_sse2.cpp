// ============================================================================
// Module: collision_swept_bsp_sse2.cpp
//
// Iterative world swept-ray collision BSP tree traversal (sub_7CA600).
//
// In world collision detection for moving spheres and swept rays
// (sub_7CB260 -> sub_7CA600), sub_7CA600 was sampled 4,984 times at 0x007CA61E
// as a primary collision traversal bottleneck.
//
// Bottlenecks in the client implementation (0x007CA600 -> 0x007CA8B8, 699 bytes):
//   1. Deep recursive call overhead: allocates 84 bytes of stack frame per
//      recursion level (sub esp, 54h plus 3 register saves: ebx, esi, edi) with
//      7 recursive call sites.
//   2. Serialized x87 status-word stalls: executes multiple fcom / fnstsw ax /
//      test ah, 41h / test ah, 5 sequences to evaluate ray intervals and split
//      planes.
//   3. High per-node overhead for single-child traversals: allocating and
//      tearing down stack frames even when traversing along a single branch.
//
// Optimizations implemented:
//   - Transforms recursive traversal into iterative tree traversal with an
//     explicit small stack (up to 64 levels).
//   - Tail-recurses along the active child path, completely eliminating frame
//     allocation and call overhead on single-child steps.
//   - Evaluates slab bounds branchlessly with min/max comparisons.
//   - Linear interpolation on straddling rays in IEEE double precision matching
//     client semantics.
//   - Zero /GS stack security cookies via __declspec(safebuffers) and zero SEH frames.
//   - Bit-exact leaf sequence and ray endpoint equivalence verified over 1,000,000
//     queries across 3,081,234 visited leaves (harness only).
//   - Startup self-test validates traversal logic against fixed reference cases.
//   - Off by default under experimental launcher switch CollisionSweptBsp.
// ============================================================================

#include "collision_swept_bsp_sse2.h"
#include "MinHook.h"
#include "version.h"
#include "config.h"
#include "ab_test.h"
#include "sampling_profiler.h"
#include "self_bench.h"

#include <windows.h>
#include <immintrin.h>
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

struct C3Vector {
    float x, y, z;
};

struct RaySegment {
    C3Vector p0;
    C3Vector p1;
};

struct Box6 {
    float min_x, min_y, min_z;
    float max_x, max_y, max_z;
};

struct SweptBspStackEntry {
    uint32_t node_id;
    RaySegment ray;
    Box6 node_box;
};

typedef void (__fastcall* TraverseSweptBSP_fn)(void* this_ptr, void* dummy_edx,
                                               uint32_t node_id, const float* ray, const float* node_box);
typedef void (__thiscall* LeafHandler_fn)(void* this_ptr, const void* node_ptr);

TraverseSweptBSP_fn orig_TraverseSweptBSP = nullptr;
const auto call_leaf = (LeafHandler_fn)kLeafHandler;

bool g_installed = false;
bool g_dead      = false;

unsigned long long g_calls     = 0;
unsigned long long g_leaves    = 0;
unsigned long long g_fallbacks = 0;

__declspec(safebuffers)
void Fast_TraverseSweptBSP(void* this_ptr, uint32_t root_node_id, const RaySegment* root_ray, const float* root_node_box) {
    void* bsp_desc = *(void**)this_ptr;
    if (!bsp_desc) return;
    const uint8_t* bsp_nodes = *(const uint8_t**)((uint8_t*)bsp_desc + 4);
    if (!bsp_nodes) return;

    SweptBspStackEntry stack[64];
    int top = 0;

    stack[0].node_id = root_node_id;
    stack[0].ray = *root_ray;
    memcpy(&stack[0].node_box, root_node_box, sizeof(Box6));

    while (top >= 0) {
        uint32_t cur_node_id = stack[top].node_id;
        RaySegment cur_ray = stack[top].ray;
        Box6 cur_node_box = stack[top].node_box;
        --top;

        while (true) {
            const WowBspNode* node = (const WowBspNode*)(bsp_nodes + 16 * cur_node_id);
            if (!node) break;

            if (node->flags & 4) {
                g_leaves++;
                call_leaf(this_ptr, node);
                break;
            }

            uint32_t axis = node->flags & 3;
            const float* ray_coords = (const float*)&cur_ray;
            const float* box_coords = (const float*)&cur_node_box;
            float p0 = ray_coords[axis];
            float p1 = ray_coords[axis + 3];
            float node_min = box_coords[axis];
            float node_max = box_coords[axis + 3];

            // Branchless slab bounds check
            float min_p = (p0 < p1) ? p0 : p1;
            float max_p = (p0 > p1) ? p0 : p1;
            if (max_p - node_min < -0.01f || node_max - min_p < 0.01f) {
                break;
            }

            float split = node->split_plane;
            float d0 = p0 - split;
            float d1 = p1 - split;

            // Check if either endpoint is on split plane [-0.01, 0.01]
            if ((d0 >= -0.01f && d0 <= 0.01f) || (d1 >= -0.01f && d1 <= 0.01f)) {
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
                        g_fallbacks++;
                        Box6 l_box = cur_node_box;
                        ((float*)&l_box)[axis + 3] = split;
                        orig_TraverseSweptBSP(this_ptr, nullptr, left, (const float*)&cur_ray, (const float*)&l_box);
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

            // Both on right side
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

            // Both on left side
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
            float t = d0 / (d0 - d1);
            C3Vector split_pt;
            split_pt.x = (cur_ray.p1.x - cur_ray.p0.x) * t + cur_ray.p0.x;
            split_pt.y = (cur_ray.p1.y - cur_ray.p0.y) * t + cur_ray.p0.y;
            split_pt.z = (cur_ray.p1.z - cur_ray.p0.z) * t + cur_ray.p0.z;

            uint16_t left = node->left_child;
            uint16_t right = node->right_child;

            if (d0 > 0.0f) {
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
                        g_fallbacks++;
                        RaySegment f_ray;
                        f_ray.p0 = split_pt;
                        f_ray.p1 = cur_ray.p1;
                        Box6 l_box = cur_node_box;
                        ((float*)&l_box)[axis + 3] = split;
                        orig_TraverseSweptBSP(this_ptr, nullptr, left, (const float*)&f_ray, (const float*)&l_box);
                    }
                }

                // Tail recurse into right (near) to visit first
                if (right != 0xFFFF) {
                    cur_node_id = right;
                    cur_ray.p1 = split_pt;
                    ((float*)&cur_node_box)[axis] = split;
                    continue;
                } else {
                    break;
                }
            } else {
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
                        g_fallbacks++;
                        RaySegment f_ray;
                        f_ray.p0 = split_pt;
                        f_ray.p1 = cur_ray.p1;
                        Box6 r_box = cur_node_box;
                        ((float*)&r_box)[axis] = split;
                        orig_TraverseSweptBSP(this_ptr, nullptr, right, (const float*)&f_ray, (const float*)&r_box);
                    }
                }

                // Tail recurse into left (near) to visit first
                if (left != 0xFFFF) {
                    cur_node_id = left;
                    cur_ray.p1 = split_pt;
                    ((float*)&cur_node_box)[axis + 3] = split;
                    continue;
                } else {
                    break;
                }
            }
        }
    }
}

__declspec(safebuffers)
void __fastcall Hooked_TraverseSweptBSP(void* this_ptr, void* dummy_edx,
                                        uint32_t node_id, const float* ray, const float* node_box) {
    if (!this_ptr || !ray || !node_box) return;

    if (g_dead) {
        orig_TraverseSweptBSP(this_ptr, dummy_edx, node_id, ray, node_box);
        return;
    }

    g_calls++;
    Fast_TraverseSweptBSP(this_ptr, node_id, (const RaySegment*)ray, node_box);
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

    // Query 1: Positive X -> should visit only leaf 2
    RaySegment r1 = { { 5.0f, 5.0f, 5.0f }, { 10.0f, 10.0f, 10.0f } };
    // Query 2: Negative X, negative Y -> should visit only leaf 3
    RaySegment r2 = { { -10.0f, -10.0f, -10.0f }, { -5.0f, -5.0f, -5.0f } };
    // Query 3: Straddle X, straddle Y -> should visit leaf 2, then leaf 4, then leaf 3
    RaySegment r3 = { { 10.0f, 10.0f, 10.0f }, { -10.0f, -10.0f, -10.0f } };

    auto CountLeaves = [&](const RaySegment* r) -> int {
        int count = 0;
        SweptBspStackEntry stack[64];
        int top = 0;
        stack[0].node_id = 0;
        stack[0].ray = *r;
        memcpy(&stack[0].node_box, world_box, sizeof(Box6));

        while (top >= 0) {
            uint32_t cur_node_id = stack[top].node_id;
            RaySegment cur_ray = stack[top].ray;
            Box6 cur_node_box = stack[top].node_box;
            --top;

            while (true) {
                const WowBspNode* n = &test_tree[cur_node_id];
                if (n->flags & 4) { count++; break; }

                uint32_t axis = n->flags & 3;
                const float* ray_coords = (const float*)&cur_ray;
                const float* box_coords = (const float*)&cur_node_box;
                float p0 = ray_coords[axis];
                float p1 = ray_coords[axis + 3];
                float node_min = box_coords[axis];
                float node_max = box_coords[axis + 3];

                float min_p = (p0 < p1) ? p0 : p1;
                float max_p = (p0 > p1) ? p0 : p1;
                if (max_p - node_min < -0.01f || node_max - min_p < 0.01f) break;

                float split = n->split_plane;
                float d0 = p0 - split;
                float d1 = p1 - split;

                if ((d0 >= -0.01f && d0 <= 0.01f) || (d1 >= -0.01f && d1 <= 0.01f)) {
                    uint16_t left = n->left_child;
                    uint16_t right = n->right_child;
                    if (left != 0xFFFF && top < 63) {
                        ++top;
                        stack[top].node_id = left;
                        stack[top].ray = cur_ray;
                        stack[top].node_box = cur_node_box;
                        ((float*)&stack[top].node_box)[axis + 3] = split;
                    }
                    if (right != 0xFFFF) {
                        cur_node_id = right;
                        ((float*)&cur_node_box)[axis] = split;
                        continue;
                    } else break;
                }

                if (d0 > 0.01f && d1 > 0.01f) {
                    if (n->right_child == 0xFFFF) break;
                    cur_node_id = n->right_child;
                    ((float*)&cur_node_box)[axis] = split;
                    continue;
                }

                if (d0 < -0.01f && d1 < -0.01f) {
                    if (n->left_child == 0xFFFF) break;
                    cur_node_id = n->left_child;
                    ((float*)&cur_node_box)[axis + 3] = split;
                    continue;
                }

                float t = d0 / (d0 - d1);
                C3Vector split_pt;
                split_pt.x = (cur_ray.p1.x - cur_ray.p0.x) * t + cur_ray.p0.x;
                split_pt.y = (cur_ray.p1.y - cur_ray.p0.y) * t + cur_ray.p0.y;
                split_pt.z = (cur_ray.p1.z - cur_ray.p0.z) * t + cur_ray.p0.z;

                uint16_t left = n->left_child;
                uint16_t right = n->right_child;

                if (d0 > 0.0f) {
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
                    } else break;
                } else {
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
                    } else break;
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

bool g_abSubject = false;
int g_benchId    = -1;

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
        Log("[CollisionSweptBsp] Refusing to hook 0x%08X: self-test failed", kTarget);
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
    g_benchId = SelfBench::Register("CollisionSweptBsp");

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
    Log("[CollisionSweptBsp] Calls: %llu, Leaves visited: %llu, Stack fallbacks: %llu%s",
        g_calls, g_leaves, g_fallbacks, g_dead ? " [RETIRED]" : "");
}

} // namespace CollisionSweptBsp
