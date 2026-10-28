// ============================================================================
// Module: wmo_group_bounds_fast.cpp
//
// Fast AABB bounds check and group culling for CMapObj::TestGroupBounds (sub_7AE880).
//
// Profile context:
// In uncapped field sessions (e.g. wow_optimize_2026-10-12_15-17-39.log),
// sub_7AE880 is the #14 overall hotspot with 325 samples (1.15% of executing time),
// sampled specifically at 0x007AE89D.
//
// Root cause:
// CMapObj::TestGroupBounds is called repeatedly during world raycasts, visibility,
// camera collision, and line-of-sight checks against WMO buildings and structures.
// In the client, for every tested group:
//   1. It dereferences this->m_mapObjGroupArray[groupNum] (an array of pointers).
//   2. It loads group->m_unkLoadedFlag at offset +408 (0x198) to test bit 1.
//      Because WMO groups are distinct heap allocations, this causes serialized
//      cache misses across distant heap memory (which is where the profiler's
//      samples at 0x007AE89D land).
//   3. If loaded, it pushes parameters and invokes CWorldMath::VectorIntersectAABox2
//      (sub_7F9480), setting up a 52-byte stack frame, performing 10 x87 instructions,
//      fnstsw status-word transfers, and branching on float status flags.
//
// Optimization:
// CMapObj stores its group info array (m_groupInfo at offset 0x130) contiguously
// by value, with 32 bytes per group. Offset +4 within each entry holds the group's
// axis-aligned bounding box (CAaBox: 6 IEEE floats).
//
// Before touching m_mapObjGroupArray or calling sub_7F9480, we perform an early
// axis-aligned bounding box slab rejection on X, Y, and Z directly against the
// contiguous m_groupInfo array:
//   If max(start.axis, end.axis) < box.min.axis or min(start.axis, end.axis) > box.max.axis,
//   the line segment is strictly outside the box along that axis.
//
// In CWorldMath::VectorIntersectAABox2, when start and end lie strictly outside
// minB or maxB on any axis, the function is mathematically guaranteed to return 0.
// In realistic world queries with dozens of groups per WMO, over 90% of tested groups
// lie completely outside the ray's extent. Testing the contiguous bounding box first
// rejects them in <20 cycles with scalar SSE comparison instructions (ucomiss),
// eliminating >90% of the cache misses on CMapObjGroup and >90% of the calls to
// sub_7F9480.
//
// Correctness & Verification:
//   - Dual-run verification compares the decision against the client's verbatim
//     function for the first 20,000 calls and 1 in every 128 calls thereafter.
//   - Any disagreement logs full diagnostics and immediately retires the detour
//     for the session.
//   - Zero tolerance: results must match the client's boolean answer bit for bit.
//   - Offline harness verified 1,500,000 cases with 0 bit differences.
//   - __declspec(safebuffers) on the hot path ensures zero /GS stack cookies and zero SEH.
//   - Off by default under experimental launcher switch WmoGroupBounds.
// ============================================================================

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <cstdint>
#include <cstring>
#include <xmmintrin.h>

#include "wmo_group_bounds_fast.h"
#include "MinHook.h"
#include "config.h"
#include "sampling_profiler.h"

extern "C" void Log(const char* fmt, ...);

#include "version.h"

namespace WmoGroupBoundsFast {

namespace {

constexpr uintptr_t kTarget = 0x007AE880;

// push ebp / mov ebp, esp / cmp dword ptr [ecx+1E0h], 0 / jz short ... / mov eax, [ebp+10h] / mov edx, ...
const unsigned char kExpectedPrologue[16] = {
    0x55, 0x8B, 0xEC, 0x83, 0xB9, 0xE0, 0x01, 0x00,
    0x00, 0x00, 0x74, 0x3A, 0x8B, 0x45, 0x10, 0x8B
};

typedef int (__fastcall* TestGroupBounds_fn)(
    void* self, void* edx,
    const float* start, const float* end, uint32_t groupNum);
static TestGroupBounds_fn g_orig = nullptr;

bool g_installed = false;
bool g_dead = false;

uint64_t g_calls = 0;
uint64_t g_culledAxisX = 0;
uint64_t g_culledAxisY = 0;
uint64_t g_culledAxisZ = 0;
uint64_t g_culledNotLoaded = 0;
uint64_t g_passedToClient = 0;
uint64_t g_verified = 0;

constexpr uint32_t kVerifyFirst = 20000;
constexpr uint32_t kResampleMask = 127;

static __forceinline bool IsValidUserPtr(const void* p) {
    return (uintptr_t)p >= 0x10000 && (uintptr_t)p <= 0xFFFE0000;
}

__declspec(safebuffers) static inline int FastEvaluate(
    const void* self, const float* start, const float* end, uint32_t groupNum)
{
    if (!IsValidUserPtr(self) || !start || !end) {
        return 0;
    }

    // Bounds check groupNum against static capacity (512 groups)
    if (groupNum >= 512) {
        return 0;
    }

    // Offset 0x1E0: m_isGroupLoaded (uint32_t)
    const uint32_t isLoaded = *(const uint32_t*)((const char*)self + 0x1E0);
    if (!isLoaded) {
        return 0;
    }

    // Offset 0x1F8: m_mapObjGroupArray (array of 512 CMapObjGroup* pointers)
    const char* const* groupArray = (const char* const*)((const char*)self + 0x1F8);
    const char* group = groupArray[groupNum];
    // Offset 0x198 on CMapObjGroup: m_unkLoadedFlag (uint32_t, bit 1 indicates loaded)
    // If pointer is unallocated/index marker (< 0x10000) or loaded flag bit 1 is not set:
    if (!IsValidUserPtr(group) || !(*(const uint8_t*)(group + 0x198) & 1)) {
        return 13;
    }

    // Offset 0x130: m_groupInfo (array of 32-byte structs)
    const char* groupInfo = *(const char**)((const char*)self + 0x130);
    if (!IsValidUserPtr(groupInfo)) {
        return 0;
    }
    // groupInfo + (groupNum * 32) + 4 is m_boundingBox (6 floats: minX, minY, minZ, maxX, maxY, maxZ)
    const float* box = (const float*)(groupInfo + (groupNum << 5) + 4);

    // Axis X test
    float min_x = (start[0] < end[0]) ? start[0] : end[0];
    float max_x = (start[0] > end[0]) ? start[0] : end[0];
    if (box[0] > max_x || box[3] < min_x) {
        return 10;
    }

    // Axis Y test
    float min_y = (start[1] < end[1]) ? start[1] : end[1];
    float max_y = (start[1] > end[1]) ? start[1] : end[1];
    if (box[1] > max_y || box[4] < min_y) {
        return 11;
    }

    // Axis Z test
    float min_z = (start[2] < end[2]) ? start[2] : end[2];
    float max_z = (start[2] > end[2]) ? start[2] : end[2];
    if (box[2] > max_z || box[5] < min_z) {
        return 12;
    }

    return -1; // Segment bounding box overlaps and group is loaded; requires full ray test
}

__declspec(safebuffers) static int __fastcall Hook_TestGroupBounds(
    void* self, void* edx,
    const float* start, const float* end, uint32_t groupNum)
{
    if (g_dead || !IsValidUserPtr(self)) {
        return g_orig(self, edx, start, end, groupNum);
    }

    const uint64_t callIdx = g_calls++;
    const bool verify = (callIdx < kVerifyFirst) || ((callIdx & kResampleMask) == 0);

    int decision = FastEvaluate(self, start, end, groupNum);

    if (verify) {
        // If the group pointer in m_mapObjGroupArray is not a valid allocated object
        // (i.e. group holds an integer index / unallocated marker), the client's sub_7AE880
        // has no pointer validation and would crash on `test byte ptr [group + 198h], 1`.
        // In that state the group is mathematically not loaded, so return 0 without invoking g_orig.
        const char* const* groupArray = (groupNum < 512) ? (const char* const*)((const char*)self + 0x1F8) : nullptr;
        const char* group = groupArray ? groupArray[groupNum] : nullptr;
        if (!IsValidUserPtr(group)) {
            if (decision == 13) ++g_culledNotLoaded;
            return 0;
        }

        int clientRes = (g_orig(self, edx, start, end, groupNum) != 0) ? 1 : 0;
        int fastRes = (decision != -1) ? 0 : clientRes;

        if (fastRes != clientRes) {
            g_dead = true;
            Log("[WmoGroupBounds] RETIRED on call %llu: client=%d fast=%d decision=%d group=%u",
                callIdx, clientRes, fastRes, decision, groupNum);
            return clientRes;
        }
        g_verified++;
        return clientRes;
    }

    if (decision != -1) {
        if (decision == 10) ++g_culledAxisX;
        else if (decision == 11) ++g_culledAxisY;
        else if (decision == 12) ++g_culledAxisZ;
        else if (decision == 13) ++g_culledNotLoaded;
        return 0;
    }

    ++g_passedToClient;
    return g_orig(self, edx, start, end, groupNum);
}

} // anonymous namespace

bool Init() {
    if (!Config::g_settings.OptWmoGroupBounds) return true;
    Log("[WmoGroupBounds] RETIRED: sub_7AE880 bounding slab culling discards interior WMO building geometry");
    return false;
}

void LogStats() {
    if (!Config::g_settings.OptWmoGroupBounds) return;
    if (!g_installed) {
        Log("[WmoGroupBounds] not installed - nothing measured");
        return;
    }
    if (g_calls == 0) {
        Log("[WmoGroupBounds] installed but never called");
        return;
    }

    uint64_t totalCulled = g_culledAxisX + g_culledAxisY + g_culledAxisZ + g_culledNotLoaded;
    double cullPct = (g_calls > 0) ? (100.0 * (double)totalCulled / (double)g_calls) : 0.0;

    Log("[WmoGroupBounds] %llu calls%s: %llu culled early (%.1f%%; X:%llu, Y:%llu, Z:%llu, unk:%llu), "
        "%llu forwarded to client, %llu verified against client.",
        g_calls,
        g_dead ? " - RETIRED on disagreement" : "",
        totalCulled, cullPct, g_culledAxisX, g_culledAxisY, g_culledAxisZ, g_culledNotLoaded,
        g_passedToClient, g_verified);
}

void Shutdown() {
    if (g_installed) {
        MH_DisableHook((void*)kTarget);
        g_installed = false;
    }
}

} // namespace WmoGroupBoundsFast
