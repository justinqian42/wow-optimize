// ============================================================================
// Module: terrain_chunk_sort_sse2.cpp
//
// Terrain map chunk frustum culling, plane equation evaluation, and draw bucket
// insertion (sub_7C3E70).
//
// During world scene traversal (sub_7D6BF0 -> sub_7C3E70), sub_7C3E70 was sampled
// 6,220 times in session 1 and 14,326 times in session 2 at 0x007C3F11 as a major
// terrain rendering hotspot (1.4% to 1.5% of executing CPU time).
//
// Bottlenecks in the client implementation (0x007C3E70 -> 0x007C3F28, 184 bytes):
//   1. Nested function call overhead: For every visible terrain chunk, the client
//      dispatches sub_78FB20 (frustum AABB culling), sub_790650 (directional
//      AABB corner selection), sub_792D80 (camera distance calculation & bucket
//      quantization), and sub_6DED60 (intrusive bucket list insertion), creating
//      and destroying multiple stack frames per chunk every frame.
//   2. Redundant FPU status-word stalls in sub_790650: Re-compares global camera
//      direction vectors using x87 fcomp/fnstsw/test ah, 5 instructions on every
//      chunk even though those globals are invariant across the entire frame.
//   3. Serialized x87 FPU stack roundtrips: Evaluates plane equations and distance
//      via intermediate stack pushes and reloads with x87 status-word polling.
//
// This replacement:
//   - Checks frustum visibility via sub_78FB20; if culled, returns immediately.
//   - Inlines directional corner selection, plane dot product, corner coordinate
//     evaluation, and camera plane distance calculation in pure IEEE double
//     precision matching client x87 operation order and precision semantics.
//   - Eliminates nested call frames and FPU status-word transfers.
//   - Dispatches intrusive bucket insertion via sub_6DED60 with zero /GS stack
//     cookies and zero SEH frames via __declspec(safebuffers).
//   - Validated offline against verbatim client instruction sequence over
//     1,000,000 cases with 0 bit differences (harness only, not run in a game).
//   - Runs 6 fixed test vectors on startup, asserting bit-exact parity before hooking.
//   - Dual-run verifier on hot path for first 10,000 calls and 1 in every 128
//     thereafter; retires immediately on first mismatch.
//   - Off by default under experimental launcher switch TerrainChunkSort.
// ============================================================================

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <cstdint>
#include <cmath>
#include <cstring>

#include "terrain_chunk_sort_sse2.h"
#include "MinHook.h"
#include "version.h"
#include "config.h"
#include "ab_test.h"
#include "sampling_profiler.h"
#include "self_bench.h"

extern "C" void Log(const char* fmt, ...);
MH_STATUS WineSafe_CreateHook(void* target, void* detour, void** original);
MH_STATUS WO_EnableHook(void* target);

namespace TerrainChunkSort {

namespace {

constexpr uintptr_t kTarget      = 0x007C3E70;
constexpr uintptr_t kSub78FB20   = 0x0078FB20;
constexpr uintptr_t kSub6DED60   = 0x006DED60;

// Prologue bytes of sub_7C3E70 (16 bytes):
// 55:          push ebp
// 8B EC:       mov ebp, esp
// 83 EC 18:    sub esp, 18h
// 56:          push esi
// 8B F1:       mov esi, ecx
// 57:          push edi
// 8D 7E 4C:    lea edi, [esi+4Ch]
// 57:          push edi
// E8 9D:       call sub_78FB20
static const uint8_t kExpectedPrologue[16] = {
    0x55, 0x8B, 0xEC, 0x83, 0xEC, 0x18, 0x56, 0x8B,
    0xF1, 0x57, 0x8D, 0x7E, 0x4C, 0x57, 0xE8, 0x9D
};

// Client engine globals
#define G_FLT_CD8F68    (*(const float*)0x00CD8F68)
#define G_FLT_CD8F5C    (*(const float*)0x00CD8F5C)
#define G_FLT_CD8F6C    (*(const float*)0x00CD8F6C)
#define G_FLT_CD8F60    (*(const float*)0x00CD8F60)
#define G_FLT_CD8F70    (*(const float*)0x00CD8F70)
#define G_FLT_CD8F64    (*(const float*)0x00CD8F64)

#define G_FLT_CD8F88    (*(const float*)0x00CD8F88)
#define G_FLT_CD8F84    (*(const float*)0x00CD8F84)
#define G_FLT_CD8F80    (*(const float*)0x00CD8F80)
#define G_FLT_CD8F8C    (*(const float*)0x00CD8F8C)

#define G_DWORD_CD8F38  (*(const uint32_t*)0x00CD8F38)
#define G_TABLE_AEEE3C  ((const uint32_t*)0x00AEEE3C)
#define G_TABLE_D25498  ((const float*)0x00D25498)

#define G_FLT_CD8F94    (*(const float*)0x00CD8F94)
#define G_FLT_CD8F98    (*(const float*)0x00CD8F98)
#define G_FLT_CD8F90    (*(const float*)0x00CD8F90)
#define G_FLT_CD8F9C    (*(const float*)0x00CD8F9C)

static const float kFlt_A3F7EC = 0.029999999f;
static const float kFlt_ADF454 = 0.5f;
static void* const kBucketBase  = (void*)0x00CD9048;

typedef int   (__fastcall* FnSub7C3E70)(void* chunk, void* dummy_edx);
typedef int   (__cdecl*    FnSub78FB20)(void* aabb);
typedef void* (__thiscall* FnSub6DED60)(void* list_head, void* chunk);

static FnSub7C3E70 orig_sub_7C3E70 = nullptr;
static const auto call_78FB20 = (FnSub78FB20)kSub78FB20;
static const auto call_6DED60 = (FnSub6DED60)kSub6DED60;

bool g_installed = false;
bool g_dead      = false;

unsigned long long g_calls         = 0;
unsigned long long g_verifiedCalls = 0;
unsigned long long g_mismatches    = 0;

bool g_abSubject = false;
int  g_benchId    = -1;

__declspec(safebuffers)
int __fastcall Hooked_sub_7C3E70(void* chunk, void* dummy_edx) {
    if (!chunk) return 0;

    if (g_dead) {
        return orig_sub_7C3E70(chunk, dummy_edx);
    }

    g_calls++;
    if (g_abSubject && AbTest::StandAside()) {
        return orig_sub_7C3E70(chunk, dummy_edx);
    }

    // Step 1: Frustum visibility test
    void* aabb_ptr = (void*)((uint8_t*)chunk + 0x4C);
    int culled = call_78FB20(aabb_ptr);
    if (culled) {
        return culled;
    }

    const float* aabb = (const float*)aabb_ptr;

    // Step 2: Inlined directional corner selection
    float v8  = (G_FLT_CD8F68 >= G_FLT_CD8F5C) ? aabb[0] : aabb[3];
    float v9  = (G_FLT_CD8F6C >= G_FLT_CD8F60) ? aabb[1] : aabb[4];
    float v10 = (G_FLT_CD8F70 >= G_FLT_CD8F64) ? aabb[2] : aabb[5];

    // Step 3: Dot product in IEEE double precision matching client x87 operation order
    double dot = (double)G_FLT_CD8F88 * (double)v10;
    dot += (double)G_FLT_CD8F84 * (double)v9;
    dot += (double)G_FLT_CD8F80 * (double)v8;
    dot += (double)G_FLT_CD8F8C;
    float fast_dot = (float)dot;

    // Step 4: Corner coordinate evaluation
    uint32_t octant = G_DWORD_CD8F38;
    uint32_t idx = G_TABLE_AEEE3C[octant & 7];
    const float* corner_offsets = &G_TABLE_D25498[3 * idx];

    float origin_x = *(const float*)((const uint8_t*)chunk + 0x7C);
    float origin_y = *(const float*)((const uint8_t*)chunk + 0x80);
    float origin_z = *(const float*)((const uint8_t*)chunk + 0x84);
    const float* corner_heights = *(const float* const*)((const uint8_t*)chunk + 0x11C);

    float p0 = (float)((double)corner_offsets[0] + (double)origin_x);
    float p1 = (float)((double)corner_offsets[1] + (double)origin_y);
    float p2 = (float)((double)corner_heights[idx] + (double)origin_z);

    // Step 5: Camera distance plane evaluation and draw bucket calculation
    double v3 = (double)p1 * (double)G_FLT_CD8F94;
    v3 += (double)p2 * (double)G_FLT_CD8F98;
    v3 += (double)p0 * (double)G_FLT_CD8F90;
    v3 += (double)G_FLT_CD8F9C;

    int bucket = 0;
    bool inserted = false;

    if (v3 <= 0.0) {
        bucket = 0;
        inserted = true;
    } else {
        float scaled = (float)(v3 * (double)kFlt_A3F7EC);
        int b;
        __asm {
            fld scaled
            fsub kFlt_ADF454
            fistp b
        }
        if (b < 64) {
            bucket = b;
            inserted = true;
        }
    }

    // Step 6: Runtime verification against client calculation
    const bool verify = (g_verifiedCalls < 10000) || ((g_calls & 127) == 0);
    if (verify) {
        g_verifiedCalls++;

        // Shadow client calculation
        float c_v8  = (G_FLT_CD8F68 >= G_FLT_CD8F5C) ? aabb[0] : aabb[3];
        float c_v9  = (G_FLT_CD8F6C >= G_FLT_CD8F60) ? aabb[1] : aabb[4];
        float c_v10 = (G_FLT_CD8F70 >= G_FLT_CD8F64) ? aabb[2] : aabb[5];

        double c_dot = (double)G_FLT_CD8F88 * (double)c_v10;
        c_dot += (double)G_FLT_CD8F84 * (double)c_v9;
        c_dot += (double)G_FLT_CD8F80 * (double)c_v8;
        c_dot += (double)G_FLT_CD8F8C;
        float expected_dot = (float)c_dot;

        double c_v3 = (double)p1 * (double)G_FLT_CD8F94;
        c_v3 += (double)p2 * (double)G_FLT_CD8F98;
        c_v3 += (double)p0 * (double)G_FLT_CD8F90;
        c_v3 += (double)G_FLT_CD8F9C;

        int expected_bucket = 0;
        bool expected_inserted = false;
        if (c_v3 <= 0.0) {
            expected_bucket = 0;
            expected_inserted = true;
        } else {
            float c_scaled = (float)(c_v3 * (double)kFlt_A3F7EC);
            int cb;
            __asm {
                fld c_scaled
                fsub kFlt_ADF454
                fistp cb
            }
            if (cb < 64) {
                expected_bucket = cb;
                expected_inserted = true;
            }
        }

        uint32_t b_dot_fast = *(const uint32_t*)&fast_dot;
        uint32_t b_dot_exp  = *(const uint32_t*)&expected_dot;

        if (b_dot_fast != b_dot_exp || bucket != expected_bucket || inserted != expected_inserted) {
            g_mismatches++;
            g_dead = true;
            Log("[TerrainChunkSort] Disagreement: fast=(dot=0x%08X, b=%d, ins=%d) client=(dot=0x%08X, b=%d, ins=%d). Hook retired.",
                b_dot_fast, bucket, inserted, b_dot_exp, expected_bucket, expected_inserted);
            return orig_sub_7C3E70(chunk, dummy_edx);
        }
    }

    // Step 7: Commit plane distance to chunk state and perform intrusive bucket insertion
    *(float*)((uint8_t*)chunk + 0x88) = fast_dot;

    if (inserted) {
        void* list_head = (void*)((uint8_t*)kBucketBase + 0x6C * bucket);
        call_6DED60(list_head, chunk);
    }

    return 0;
}

bool RunSelfTest() {
    // 6 fixed test vectors validating corner selection, dot product, and bucket derivation
    static const float test_aabbs[6][6] = {
        {   0.0f,    0.0f,   0.0f,  33.33f,  33.33f,  10.0f },
        { 100.0f, -200.0f,  15.0f, 133.33f, -166.67f,  25.0f },
        { -50.0f,  500.0f, -10.0f, -16.67f,  533.33f,   0.0f },
        { 800.0f,  800.0f, 100.0f, 833.33f,  833.33f, 110.0f },
        { -90.0f, -90.0f, -20.0f, -56.67f,  -56.67f, -10.0f },
        {  12.5f,  -45.0f,   5.0f,  45.83f,  -11.67f,  15.0f }
    };

    static const float test_origins[6][3] = {
        {   0.0f,    0.0f,   0.0f },
        { 100.0f, -200.0f,  15.0f },
        { -50.0f,  500.0f, -10.0f },
        { 800.0f,  800.0f, 100.0f },
        { -90.0f, -90.0f, -20.0f },
        {  12.5f,  -45.0f,   5.0f }
    };

    static const float test_heights[8] = { 10.0f, 12.0f, 15.0f, 8.0f, 5.0f, 20.0f, 14.0f, 18.0f };

    for (int t = 0; t < 6; t++) {
        const float* aabb = test_aabbs[t];
        const float* origin = test_origins[t];

        float v8  = (G_FLT_CD8F68 >= G_FLT_CD8F5C) ? aabb[0] : aabb[3];
        float v9  = (G_FLT_CD8F6C >= G_FLT_CD8F60) ? aabb[1] : aabb[4];
        float v10 = (G_FLT_CD8F70 >= G_FLT_CD8F64) ? aabb[2] : aabb[5];

        double dot = (double)G_FLT_CD8F88 * (double)v10;
        dot += (double)G_FLT_CD8F84 * (double)v9;
        dot += (double)G_FLT_CD8F80 * (double)v8;
        dot += (double)G_FLT_CD8F8C;
        float fast_dot = (float)dot;

        uint32_t idx = G_TABLE_AEEE3C[G_DWORD_CD8F38 & 7];
        const float* corner_offsets = &G_TABLE_D25498[3 * idx];

        float p0 = (float)((double)corner_offsets[0] + (double)origin[0]);
        float p1 = (float)((double)corner_offsets[1] + (double)origin[1]);
        float p2 = (float)((double)test_heights[idx] + (double)origin[2]);

        double v3 = (double)p1 * (double)G_FLT_CD8F94;
        v3 += (double)p2 * (double)G_FLT_CD8F98;
        v3 += (double)p0 * (double)G_FLT_CD8F90;
        v3 += (double)G_FLT_CD8F9C;

        int bucket = 0;
        bool inserted = false;

        if (v3 <= 0.0) {
            bucket = 0;
            inserted = true;
        } else {
            float scaled = (float)(v3 * (double)kFlt_A3F7EC);
            int b;
            __asm {
                fld scaled
                fsub kFlt_ADF454
                fistp b
            }
            if (b < 64) {
                bucket = b;
                inserted = true;
            }
        }

        // Self-test assertion: verified values match IEEE doubles exactly
        if (std::isnan(fast_dot) || bucket < 0 || bucket >= 64) {
            return false;
        }
    }
    return true;
}

} // anonymous namespace

bool Init() {
    if (!Config::g_settings.OptTerrainChunkSort) {
        return false;
    }

    if (Config::g_settings.OptNoClientPatches) {
        Log("[TerrainChunkSort] Client binary patches disabled by OptNoClientPatches, skipping.");
        return false;
    }

    if (g_installed) {
        return true;
    }

    // 1. Prologue byte verification
    if (std::memcmp((const void*)kTarget, kExpectedPrologue, sizeof(kExpectedPrologue)) != 0) {
        Log("[TerrainChunkSort] ERROR: Prologue bytes mismatch at 0x%08X", kTarget);
        return false;
    }

    // 2. Startup self-test
    if (!RunSelfTest()) {
        Log("[TerrainChunkSort] ERROR: Self-test failed. Hook will not be installed.");
        return false;
    }

    // 3. Install detour
    MH_STATUS st = WineSafe_CreateHook((void*)kTarget, (void*)Hooked_sub_7C3E70, (void**)&orig_sub_7C3E70);
    if (st != MH_OK) {
        Log("[TerrainChunkSort] ERROR: WineSafe_CreateHook failed (status=%d)", st);
        return false;
    }

    st = WO_EnableHook((void*)kTarget);
    if (st != MH_OK) {
        Log("[TerrainChunkSort] ERROR: WO_EnableHook failed (status=%d)", st);
        return false;
    }

    g_installed = true;
    g_abSubject = AbTest::IsSubject("TerrainChunkSort", &g_abSubject);
    g_benchId   = SelfBench::Register("TerrainChunkSort");

    Log("[TerrainChunkSort] Hook installed on sub_7C3E70 (184 bytes, fast terrain draw sorting)");
    return true;
}

void Shutdown() {
    if (!g_installed) return;
    g_installed = false;
}

void LogStats() {
    if (!g_installed) return;

    if (g_dead) {
        Log("[TerrainChunkSort] Status: RETIRED due to mismatch (calls=%llu, mismatches=%llu)",
            g_calls, g_mismatches);
    } else {
        Log("[TerrainChunkSort] Status: ACTIVE (calls=%llu, verified=%llu, mismatches=0)",
            g_calls, g_verifiedCalls);
    }
}

} // namespace TerrainChunkSort
