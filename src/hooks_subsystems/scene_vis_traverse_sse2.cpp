// ============================================================================
// Module: scene_vis_traverse_sse2
//
// sub_7A50C0 is the scene graph visibility traversal and bounding box culling
// routine (384 bytes) called on every rendered frame from sub_7A55E0 and
// sub_7A5A60.
//
// For every node in the active render list, it checks pass masks, loads 6-float
// bounding boxes (minX, minY, minZ, maxX, maxY, maxZ), invokes AABB_Transform
// (sub_7F9430) and tests visibility overlap via AABB_Overlap (sub_78F370).
//
// Optimization:
// - Vectorizes the 6-float bounding box copies with 128-bit SSE vector moves.
// - Performs fast-path bounds checks and early rejection before calling
//   sub_7A3E40 and sub_7A4AF0.
// - Fully preserves caller register contract (ebx=node, edi=a3, edx=a2).
// ============================================================================

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <emmintrin.h>
#include <cstdint>
#include <cstring>

#include "scene_vis_traverse_sse2.h"
#include "MinHook.h"
#include "version.h"
#include "config.h"
#include "ab_test.h"

extern "C" void Log(const char* fmt, ...);

namespace SceneVisTraverse {

namespace {

constexpr uintptr_t kTarget = 0x007A50C0;

// Prologue in 3.3.5a (12340):
// 7A50C0: 55                push    ebp
// 7A50C1: 8B EC             mov     ebp, esp
// 7A50C3: 8B 55 14          mov     edx, [ebp+14h]
// 7A50C6: 83 EC 34          sub     esp, 34h
static const uint8_t kExpectedPrologue[8] = {
    0x55, 0x8B, 0xEC, 0x8B, 0x55, 0x14, 0x83, 0xEC
};

typedef int (__cdecl* SceneVisTraverse_fn)(uint32_t* list, int a2, int a3, int a4);
SceneVisTraverse_fn g_orig_SceneVisTraverse = nullptr;

bool g_installed = false;
bool g_dead = false;
bool g_abSubject = false;

unsigned long g_calls = 0;
unsigned long g_nodesChecked = 0;
unsigned long g_nodesPassed = 0;
unsigned long g_nodesCulled = 0;

// Internal struct with 6 floats to avoid MSVC /GS security buffer cookies
struct AABB6 {
    float minX;
    float minY;
    float minZ;
    float maxX;
    float maxY;
    float maxZ;
};

typedef void (__cdecl* Sub7F9430_fn)(const void* transform, const void* in_box, void* out_box);
typedef bool (__thiscall* Sub78F370_fn)(void* this_ptr, const void* box);

constexpr uintptr_t fn_sub_7F9430 = 0x007F9430;
constexpr uintptr_t fn_sub_78F370 = 0x0078F370;
constexpr uintptr_t fn_sub_7A3E40 = 0x007A3E40;
constexpr uintptr_t fn_sub_7A4AF0 = 0x007A4AF0;

__declspec(safebuffers) int __cdecl Hook_SceneVisTraverse(uint32_t* a1, int a2, int a3, int a4) {
    ++g_calls;
    if (g_dead || (g_abSubject && AbTest::StandAside())) {
        return g_orig_SceneVisTraverse(a1, a2, a3, a4);
    }

    if ((a4 & 0x00F0000F) == 0) {
        return 1;
    }

    if (!a1) {
        return g_orig_SceneVisTraverse(a1, a2, a3, a4);
    }

    uint32_t v5 = a1[2];
    uint32_t node_ptr = 0;
    if ((v5 & 1) == 0 && v5 != 0) {
        node_ptr = v5;
    }

    const uint32_t pass_id = *(const volatile uint32_t*)0x00CE04C4;

    while ((node_ptr & 1) == 0 && node_ptr != 0) {
        uint32_t ebx = *(const uint32_t*)(node_ptr + 4);
        uint32_t flags = *(const uint32_t*)(ebx + 0x0C);

        if ((flags & 0x100) == 0 &&
            *(const uint32_t*)(ebx + 0x2C) != pass_id &&
            *(const uint32_t*)(ebx + 0x34) != 0)
        {
            uint32_t q0 = *(const uint32_t*)(ebx + 0xB8);
            uint32_t q1 = *(const uint32_t*)(ebx + 0xBC);
            bool pass_ok = (q0 | q1) ? ((a4 & 0x00F00000) != 0) : ((a4 & 0x0F) != 0);

            if (pass_ok) {
                ++g_nodesChecked;

                AABB6 box;
                // Vectorize 6-float bounding box copy
                __m128 b0 = _mm_loadu_ps((const float*)(ebx + 0xC0));
                __m128 b1 = _mm_load_ss((const float*)(ebx + 0xD0));
                __m128 b2 = _mm_load_ss((const float*)(ebx + 0xD4));
                _mm_storeu_ps(&box.minX, b0);
                _mm_store_ss(&box.maxY, b1);
                _mm_store_ss(&box.maxZ, b2);

                if (*(const int8_t*)(ebx + 0x0C) >= 0) {
                    *(uint32_t*)(ebx + 0x7C) |= 0x10000;
                    uint32_t e34 = *(const uint32_t*)(ebx + 0x34);
                    uint32_t e2c = *(const uint32_t*)(e34 + 0x2C);
                    const float* src_box = (const float*)(e2c + 0x154);

                    AABB6 v12;
                    __m128 m0 = _mm_loadu_ps(&src_box[0]);
                    __m128 m1 = _mm_load_ss(&src_box[4]);
                    __m128 m2 = _mm_load_ss(&src_box[5]);
                    _mm_storeu_ps(&v12.minX, m0);
                    _mm_store_ss(&v12.maxY, m1);
                    _mm_store_ss(&v12.maxZ, m2);

                    ((Sub7F9430_fn)fn_sub_7F9430)((const void*)(ebx + 0xD8), &v12, &box);
                }

                // Visibility overlap test
                bool overlap = ((Sub78F370_fn)fn_sub_78F370)((void*)a2, &box);
                if (overlap) {
                    ++g_nodesPassed;
                    if (*(const int8_t*)(ebx + 0x0C) >= 0) {
                        // sub_7A3E40 expects ecx = &box, edi = a3, ebx = ebx
                        __asm {
                            push ebx
                            push edi
                            push esi
                            lea ecx, box
                            mov edi, a3
                            mov ebx, ebx
                            mov eax, fn_sub_7A3E40
                            call eax
                            pop esi
                            pop edi
                            pop ebx
                        }
                    } else {
                        // sub_7A4AF0 expects edx = a2, edi = a3, ebx = ebx
                        __asm {
                            push ebx
                            push edi
                            push esi
                            mov edx, a2
                            mov edi, a3
                            mov ebx, ebx
                            mov eax, fn_sub_7A4AF0
                            call eax
                            pop esi
                            pop edi
                            pop ebx
                        }
                    }
                } else {
                    ++g_nodesCulled;
                }

                *(uint32_t*)(ebx + 0x2C) = pass_id;
            }
        }
        node_ptr = *(const uint32_t*)(node_ptr + *a1 + 4);
    }
    return 1;
}

} // namespace

bool Init() {
    if (!Config::g_settings.OptSceneVisTraverse) {
        return false;
    }

    if (!WowOpt_ClientPatchAllowed((const void*)kTarget)) {
        Log("[SceneVisTraverse] Client patches forbidden; stand down");
        return false;
    }

    if (std::memcmp((const void*)kTarget, kExpectedPrologue, sizeof(kExpectedPrologue)) != 0) {
        Log("[SceneVisTraverse] Prologue mismatch at 0x%08X; aborting", (unsigned)kTarget);
        return false;
    }

    MH_STATUS status = WineSafe_CreateHook((void*)kTarget, (void*)&Hook_SceneVisTraverse,
                                          (void**)&g_orig_SceneVisTraverse);
    if (status != MH_OK) {
        Log("[SceneVisTraverse] MH_CreateHook failed: %d", status);
        return false;
    }

    status = WO_EnableHook((void*)kTarget);
    if (status != MH_OK) {
        Log("[SceneVisTraverse] MH_EnableHook failed: %d", status);
        return false;
    }

    g_installed = true;
    g_abSubject = AbTest::IsSubject("SceneVisTraverse", &g_abSubject);
    Log("[SceneVisTraverse] Hook armed at 0x%08X (SSE2 bounds traversal)", (unsigned)kTarget);
    return true;
}

void Shutdown() {
    if (!g_installed) return;
    MH_DisableHook((LPVOID)kTarget);
    MH_RemoveHook((LPVOID)kTarget);
    g_installed = false;
    Log("[SceneVisTraverse] Hook removed");
}

void LogStats() {
    if (!g_installed) return;
    Log("[SceneVisTraverse] calls=%lu checked=%lu passed=%lu culled=%lu",
        g_calls, g_nodesChecked, g_nodesPassed, g_nodesCulled);
}

} // namespace SceneVisTraverse
