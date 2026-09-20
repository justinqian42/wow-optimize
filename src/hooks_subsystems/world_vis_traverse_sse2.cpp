// ============================================================================
// Module: world_vis_traverse_sse2
//
// The World Visibility Traversal family (sub_7BCC00 for multi-pass / shadow
// cascades and sub_7BCF20 for single-pass scene rendering) is 3.45% to 4.5% of
// main-thread CPU time during world rendering.
//
// Every frame, the engine traverses a 2D bounding grid of terrain tiles (64x64)
// and chunk cells (16x16). Within each cell:
// 1. Cell AABB Overlap (0x007BCCA4 - 0x007BCD14): Six serialized x87 float
//    comparisons, each followed by `fnstsw ax` and a pipeline stall. Hundreds
//    of cells evaluated per pass produce thousands of pipeline stalls per frame.
// 2. Object Distance Cull (0x007BCE12 - 0x007BCE4B): 14 serialized x87 instructions
//    calculating (x - camX)^2 + (y - camY)^2 + (z - camZ)^2 <= maxDistSq[cat],
//    followed by `fnstsw ax`.
//
// Vectorization:
// - Cell AABB Overlap: Two ordered 128-bit vector comparisons (_mm_cmple_ps and
//   _mm_cmpge_ps) test all 3 dimensions simultaneously in ~5 instructions with
//   zero fnstsw pipeline stalls.
// - Object Distance Cull: Preserves exact 53-bit IEEE float double precision
//   in the client's operation order ((dy*dy + dz*dz) + dx*dx) using SSE2 scalar
//   double instructions with zero x87 status-word stalls.
// ============================================================================

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <emmintrin.h>
#include <cstdint>
#include <cstring>

#include "world_vis_traverse_sse2.h"
#include "MinHook.h"
#include "version.h"
#include "config.h"
#include "ab_test.h"
#include "sampling_profiler.h"

extern "C" void Log(const char* fmt, ...);

MH_STATUS WineSafe_CreateHook(void* target, void* detour, void** original);
MH_STATUS WO_EnableHook(void* target);
MH_STATUS WO_DisableHook(void* target);

namespace WorldVisTraverse {

namespace {

constexpr uintptr_t kMultiPassAddr  = 0x007BCC00;
constexpr uintptr_t kSinglePassAddr = 0x007BCF20;

typedef void (__cdecl *VisibilityTraverseMultiPass_fn)(uintptr_t worldCtx, int minPass, int maxPass);
typedef void (__cdecl *VisibilityTraverseSinglePass_fn)(uintptr_t worldCtx, int pass);

VisibilityTraverseMultiPass_fn  orig_VisibilityTraverse_MultiPass  = nullptr;
VisibilityTraverseSinglePass_fn orig_VisibilityTraverse_SinglePass = nullptr;

bool g_installed = false;
bool g_abSubject = false;

// Telemetry counters
static volatile unsigned long g_multiPassCalls     = 0;
static volatile unsigned long g_singlePassCalls    = 0;
static volatile unsigned long g_cellsTested        = 0;
static volatile unsigned long g_cellsAabbPassed    = 0;
static volatile unsigned long g_cellsFrustumPassed = 0;
static volatile unsigned long g_objectsTested      = 0;
static volatile unsigned long g_objectsDistCulled  = 0;
static volatile unsigned long g_objectsRendered    = 0;

struct GridBounds {
    int minX;
    int minY;
    int maxX;
    int maxY;
};

// Custom __usercall callee at sub_7BB460: eax = queryAABB, esi = outBounds[4]
static inline void Call_CoordConvert(const float* queryAABB, GridBounds* outBounds) {
    __asm {
        mov eax, queryAABB
        mov esi, outBounds
        mov edx, 0x007BB460
        call edx
    }
}

// Callee prototypes
typedef float* (__cdecl *FnGetPassLimits)(void* worldCtx, int pass, int* outPass, float* outMaxDist);
#define Call_GetPassLimits ((FnGetPassLimits)0x007BA8F0)

typedef int (__thiscall *FnIsAABBVisible)(void* frustumContext, const float* bounds);
#define Call_IsAABBVisible ((FnIsAABBVisible)0x009839E0)

typedef int (__thiscall *FnIsAABBDetailVisible)(void* frustumDetailContext, const float* bounds);
#define Call_IsAABBDetailVisible ((FnIsAABBDetailVisible)0x00983A60)

typedef int (__cdecl *FnOcclusionTest)(const float* sphereOrBox);
#define Call_OcclusionTest ((FnOcclusionTest)0x007CCE00)

typedef int (__thiscall *FnBoxOverlap)(const float* queryBox, const float* objBox);
#define Call_BoxOverlap ((FnBoxOverlap)0x0078F370)

typedef void (__thiscall *FnModelAddBatches)(void* model, void* queueA, void* queueB);
#define Call_ModelAddBatches ((FnModelAddBatches)0x00834660)

// Fast Vectorized Cell AABB Overlap (replaces 6 serialized x87 fcomp + fnstsw ax instructions)
static inline bool FastCellAABBOverlap(const float* qMin, const float* qMax, const float* cMin, const float* cMax) {
    __m128 vQMin = _mm_loadu_ps(qMin);
    __m128 vQMax = _mm_loadu_ps(qMax);
    __m128 vCMin = _mm_loadu_ps(cMin);
    __m128 vCMax = _mm_loadu_ps(cMax);

    // vCMin <= vQMax  <=>  vQMax >= vCMin
    __m128 cmp1 = _mm_cmple_ps(vCMin, vQMax);
    // vCMax >= vQMin  <=>  vQMin <= vCMax
    __m128 cmp2 = _mm_cmpge_ps(vCMax, vQMin);

    __m128 mask = _mm_and_ps(cmp1, cmp2);
    return (_mm_movemask_ps(mask) & 7) == 7;
}

__declspec(safebuffers) void __cdecl Hook_VisibilityTraverse_MultiPass(uintptr_t worldCtx, int minPass, int maxPass) {
    if (g_abSubject && AbTest::StandAside()) {
        orig_VisibilityTraverse_MultiPass(worldCtx, minPass, maxPass);
        return;
    }

    ++g_multiPassCalls;

    GridBounds bounds = { 0, 0, 0, 0 };
    uint32_t passIdx = *(const uint32_t*)(worldCtx + 0x0A84);
    const float* queryAabb = (const float*)(worldCtx + passIdx * 24 + 0x24);
    Call_CoordConvert(queryAabb, &bounds);

    int minX = bounds.minX;
    int minY = bounds.minY;
    int maxX = bounds.maxX;
    int maxY = bounds.maxY;

    if (minX > maxX) return;

    const uintptr_t* worldTiles = (const uintptr_t*)0x00CE48D0;
    const uint8_t curFrameToken = *(const uint8_t*)0x00D25300;
    const float* camPos = (const float*)0x00CD8F5C;
    const float* maxDistSqTable = (const float*)0x00ADF3DC;

    const float* maxPassQueryMin = (const float*)(worldCtx + maxPass * 24 + 0x24);
    const float* maxPassQueryMax = (const float*)(worldCtx + maxPass * 24 + 0x30);
    void* maxPassFrustumCtx = (void*)(worldCtx + maxPass * 0xF4 + 0x624);

    for (int cellX = minX; cellX <= maxX; ++cellX) {
        for (int cellY = minY; cellY <= maxY; ++cellY) {
            int tileX = (cellX >> 4) & 0x3F;
            int tileY = (cellY >> 4) & 0x3F;
            int tileIdx = (tileX << 6) + tileY;
            uintptr_t tile = worldTiles[tileIdx];
            if (!tile) continue;
            if (*(const uint32_t*)(tile + 0x70) != 0) continue;

            int cellSubX = cellX & 0x0F;
            int cellSubY = cellY & 0x0F;
            int cellIdx = (cellSubX << 4) + cellSubY;
            uintptr_t cell = *(const uintptr_t*)(tile + cellIdx * 4 + 0xBC);
            if (!cell) continue;

            ++g_cellsTested;

            const float* cellBoxMin = (const float*)(cell + 0x4C);
            const float* cellBoxMax = (const float*)(cell + 0x58);

            // Accelerated Cell AABB Overlap
            if (!FastCellAABBOverlap(maxPassQueryMin, maxPassQueryMax, cellBoxMin, cellBoxMax)) {
                continue;
            }

            ++g_cellsAabbPassed;

            // Frustum visibility test
            if (!Call_IsAABBVisible(maxPassFrustumCtx, cellBoxMin)) {
                continue;
            }

            // Occlusion culling test
            if (Call_OcclusionTest((const float*)(cell + 0x3C)) != 0) {
                continue;
            }

            ++g_cellsFrustumPassed;

            // Traverse object list in cell
            uintptr_t node = *(const uintptr_t*)(cell + 0xCC);
            while (node && !(node & 1)) {
                uintptr_t obj = *(const uintptr_t*)(node + 4);
                uintptr_t model = *(const uintptr_t*)(obj + 0x34);
                if (model && (*(const int8_t*)(obj + 0x0C) < 0)) {
                    ++g_objectsTested;
                    bool isSpecial = (*(const uint8_t*)(model + 0x10) & 1) &&
                                     (*(const uint16_t*)(*(const uintptr_t*)(model + 0x2C) + 0x198) > 0);

                    for (int pass = minPass; pass <= maxPass; ++pass) {
                        uint8_t* pVisToken = (uint8_t*)(obj + pass + 0x28);
                        if (*pVisToken != curFrameToken) {
                            *pVisToken = curFrameToken;

                            const float* objPos = (const float*)(obj + 0x38);
                            uint8_t cat = *(const uint8_t*)(obj + 0x24);

                            // Double-precision distance check matching x87 operation order:
                            double dx = (double)objPos[0] - (double)camPos[0];
                            double dy = (double)objPos[1] - (double)camPos[1];
                            double dz = (double)objPos[2] - (double)camPos[2];
                            double distSq = (dy * dy + dz * dz) + (dx * dx);
                            double maxDistSq = (double)maxDistSqTable[cat];

                            if (distSq <= maxDistSq) {
                                uint8_t passFlag = *(const uint8_t*)(worldCtx + pass * 4);
                                bool flagPass = isSpecial ? ((passFlag & 4) != 0) : ((passFlag & 8) != 0);
                                if (flagPass) {
                                    const float* passQueryMin = (const float*)(worldCtx + pass * 24 + 0x24);
                                    const float* objBounds = (const float*)(obj + 0x48);
                                    if (Call_BoxOverlap(passQueryMin, objBounds)) {
                                        void* passFrustumCtx = (void*)(worldCtx + pass * 0xF4 + 0x624);
                                        if (Call_IsAABBVisible(passFrustumCtx, objBounds)) {
                                            uint32_t detailFlag = *(const uint32_t*)(worldCtx + pass * 4 + 0x18);
                                            void* passDetailCtx = (void*)(worldCtx + pass * 0xF4 + 0x348);
                                            if (!detailFlag || Call_IsAABBDetailVisible(passDetailCtx, objBounds) != 3) {
                                                void* queueA = (void*)(0x00D2532C + pass * 36);
                                                void* queueB = (void*)(0x00D25338 + pass * 36);
                                                Call_ModelAddBatches((void*)model, queueA, queueB);
                                                ++g_objectsRendered;
                                            }
                                        }
                                    }
                                }
                            } else {
                                ++g_objectsDistCulled;
                            }
                        }
                    }
                }
                node = *(const uintptr_t*)(node + *(const uint32_t*)(cell + 0xC4) + 4);
            }
        }
    }
}

__declspec(safebuffers) void __cdecl Hook_VisibilityTraverse_SinglePass(uintptr_t worldCtx, int pass) {
    if (g_abSubject && AbTest::StandAside()) {
        orig_VisibilityTraverse_SinglePass(worldCtx, pass);
        return;
    }

    ++g_singlePassCalls;

    float maxDistLimit = 0.0f;
    int dummyPass = pass;
    Call_GetPassLimits((void*)worldCtx, pass, &dummyPass, &maxDistLimit);

    const float* queryAabb = (const float*)(worldCtx + pass * 24 + 0x24);
    GridBounds bounds = { 0, 0, 0, 0 };
    Call_CoordConvert(queryAabb, &bounds);

    int minX = bounds.minX;
    int minY = bounds.minY;
    int maxX = bounds.maxX;
    int maxY = bounds.maxY;

    if (minX > maxX) return;

    const uintptr_t* worldTiles = (const uintptr_t*)0x00CE48D0;
    const uint8_t curFrameToken = *(const uint8_t*)0x00D25300;
    const float* camPos = (const float*)0x00CD8F5C;
    const float* maxDistSqTable = (const float*)0x00ADF3DC;

    const float* passQueryMin = (const float*)(worldCtx + pass * 24 + 0x24);
    const float* passQueryMax = (const float*)(worldCtx + pass * 24 + 0x30);
    void* passFrustumCtx = (void*)(worldCtx + pass * 0xF4 + 0x624);
    void* passDetailCtx = (void*)(worldCtx + pass * 0xF4 + 0x348);
    uint32_t passDetailFlag = *(const uint32_t*)(worldCtx + pass * 4 + 0x18);
    uint8_t passFlag = *(const uint8_t*)(worldCtx + pass * 4);

    for (int cellX = minX; cellX <= maxX; ++cellX) {
        for (int cellY = minY; cellY <= maxY; ++cellY) {
            int tileX = (cellX >> 4) & 0x3F;
            int tileY = (cellY >> 4) & 0x3F;
            int tileIdx = (tileX << 6) + tileY;
            uintptr_t tile = worldTiles[tileIdx];
            if (!tile) continue;
            if (*(const uint32_t*)(tile + 0x70) != 0) continue;

            int cellSubX = cellX & 0x0F;
            int cellSubY = cellY & 0x0F;
            int cellIdx = (cellSubX << 4) + cellSubY;
            uintptr_t cell = *(const uintptr_t*)(tile + cellIdx * 4 + 0xBC);
            if (!cell) continue;

            ++g_cellsTested;

            const float* cellBoxMin = (const float*)(cell + 0x4C);
            const float* cellBoxMax = (const float*)(cell + 0x58);

            // Accelerated Cell AABB Overlap
            if (!FastCellAABBOverlap(passQueryMin, passQueryMax, cellBoxMin, cellBoxMax)) {
                continue;
            }

            ++g_cellsAabbPassed;

            // Frustum visibility test
            if (!Call_IsAABBVisible(passFrustumCtx, cellBoxMin)) {
                continue;
            }

            // Detail visibility test
            if (passDetailFlag != 0 && Call_IsAABBDetailVisible(passDetailCtx, cellBoxMin) == 3) {
                continue;
            }

            ++g_cellsFrustumPassed;

            // Traverse object list in cell
            uintptr_t node = *(const uintptr_t*)(cell + 0xCC);
            while (node && !(node & 1)) {
                uintptr_t obj = *(const uintptr_t*)(node + 4);
                uint8_t* pVisToken = (uint8_t*)(obj + pass + 0x28);
                if (*pVisToken != curFrameToken) {
                    *pVisToken = curFrameToken;

                    uintptr_t model = *(const uintptr_t*)(obj + 0x34);
                    if (model && (*(const int8_t*)(obj + 0x0C) < 0)) {
                        ++g_objectsTested;
                        const float* objPos = (const float*)(obj + 0x38);
                        uint8_t cat = *(const uint8_t*)(obj + 0x24);

                        double dx = (double)objPos[0] - (double)camPos[0];
                        double dy = (double)objPos[1] - (double)camPos[1];
                        double dz = (double)objPos[2] - (double)camPos[2];
                        double distSq = (dy * dy + dz * dz) + (dx * dx);
                        double maxDistSq = (double)maxDistSqTable[cat];

                        if (distSq <= maxDistSq && maxDistLimit >= *(const float*)(obj + 0x44)) {
                            bool isSpecial = (*(const uint8_t*)(model + 0x10) & 1) &&
                                             (*(const uint16_t*)(*(const uintptr_t*)(model + 0x2C) + 0x198) > 0);
                            bool flagPass = isSpecial ? ((passFlag & 4) != 0) : ((passFlag & 8) != 0);
                            if (flagPass) {
                                const float* objBounds = (const float*)(obj + 0x48);
                                if (Call_BoxOverlap(passQueryMin, objBounds)) {
                                    if (Call_IsAABBVisible(passFrustumCtx, objBounds)) {
                                        if (!passDetailFlag || Call_IsAABBDetailVisible(passDetailCtx, objBounds) != 3) {
                                            void* queueA = (void*)(0x00D2532C + pass * 36);
                                            void* queueB = (void*)(0x00D25338 + pass * 36);
                                            Call_ModelAddBatches((void*)model, queueA, queueB);
                                            ++g_objectsRendered;
                                        }
                                    }
                                }
                            }
                        } else {
                            ++g_objectsDistCulled;
                        }
                    }
                }
                node = *(const uintptr_t*)(node + *(const uint32_t*)(cell + 0xC4) + 4);
            }
        }
    }
}

}  // namespace

bool Init() {
    if (!Config::g_settings.OptWorldVisTraverse) {
        return true;
    }

    if (!WowOpt_ClientPatchAllowed((const void*)kMultiPassAddr) ||
        !WowOpt_ClientPatchAllowed((const void*)kSinglePassAddr)) {
        Log("[WorldVisTraverse] NOT active: No Client Patches is on, refusing to hook.");
        return false;
    }

    if (IsBadReadPtr((const void*)kMultiPassAddr, 8) ||
        IsBadReadPtr((const void*)kSinglePassAddr, 8)) {
        Log("[WorldVisTraverse] NOT active: addresses unreadable.");
        return false;
    }

    // Exact 8-byte binary prologues from IDA
    static const unsigned char kExp_MultiPass[8]  = { 0x55, 0x8B, 0xEC, 0x83, 0xEC, 0x3C, 0x8B, 0x4D };
    static const unsigned char kExp_SinglePass[8] = { 0x55, 0x8B, 0xEC, 0x83, 0xEC, 0x2C, 0x53, 0x8B };

    if (memcmp((const void*)kMultiPassAddr, kExp_MultiPass, 8) != 0) {
        Log("[WorldVisTraverse] BAD PROLOGUE at 0x%08X", (unsigned)kMultiPassAddr);
        return false;
    }
    if (memcmp((const void*)kSinglePassAddr, kExp_SinglePass, 8) != 0) {
        Log("[WorldVisTraverse] BAD PROLOGUE at 0x%08X", (unsigned)kSinglePassAddr);
        return false;
    }

    if (WineSafe_CreateHook((void*)kMultiPassAddr, (void*)&Hook_VisibilityTraverse_MultiPass,
                            (void**)&orig_VisibilityTraverse_MultiPass) != MH_OK) {
        Log("[WorldVisTraverse] hook on multi-pass traversal 0x%08X failed", (unsigned)kMultiPassAddr);
        return false;
    }

    if (WineSafe_CreateHook((void*)kSinglePassAddr, (void*)&Hook_VisibilityTraverse_SinglePass,
                            (void**)&orig_VisibilityTraverse_SinglePass) != MH_OK) {
        Log("[WorldVisTraverse] hook on single-pass traversal 0x%08X failed", (unsigned)kSinglePassAddr);
        return false;
    }

    if (WO_EnableHook((void*)kMultiPassAddr) != MH_OK ||
        WO_EnableHook((void*)kSinglePassAddr) != MH_OK) {
        Log("[WorldVisTraverse] failed to enable hooks");
        return false;
    }

    g_abSubject = AbTest::IsSubject("WorldVisTraverse", &g_abSubject);
    SamplingProfiler::RegisterSelfSymbol("WorldVisTraverse_Multi", (const void*)&Hook_VisibilityTraverse_MultiPass);
    SamplingProfiler::RegisterSelfSymbol("WorldVisTraverse_Single", (const void*)&Hook_VisibilityTraverse_SinglePass);

    Log("[WorldVisTraverse] ACTIVE on sub_7BCC00 and sub_7BCF20. Vectorized Cell AABB and object distance cull enabled.");
    if (g_abSubject) {
        Log("[WorldVisTraverse]   under A/B test: alternating control and test frames.");
    }

    g_installed = true;
    return true;
}

void Shutdown() {
    if (!g_installed) return;
    MH_DisableHook((void*)kMultiPassAddr);
    MH_DisableHook((void*)kSinglePassAddr);
    g_installed = false;
}

void LogStats() {
    if (!Config::g_settings.OptWorldVisTraverse) {
        Log("[WorldVisTraverse] not measured: switched off.");
        return;
    }
    if (!g_installed) {
        Log("[WorldVisTraverse] not installed.");
        return;
    }

    Log("[WorldVisTraverse] calls: %lu multi-pass, %lu single-pass.",
        g_multiPassCalls, g_singlePassCalls);
    Log("[WorldVisTraverse] cells: %lu tested, %lu passed vector AABB, %lu passed frustum.",
        g_cellsTested, g_cellsAabbPassed, g_cellsFrustumPassed);
    Log("[WorldVisTraverse] objects: %lu tested, %lu culled by distance, %lu queued for render.",
        g_objectsTested, g_objectsDistCulled, g_objectsRendered);
}

}  // namespace WorldVisTraverse
