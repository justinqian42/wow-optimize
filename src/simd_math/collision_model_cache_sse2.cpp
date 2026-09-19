#include <windows.h>
#include <emmintrin.h>
#include <intrin.h>
#include <cstdint>
#include <cstring>

#include "collision_model_cache_sse2.h"
#include "MinHook.h"
#include "version.h"
#include "config.h"
#include "sampling_profiler.h"

extern "C" void Log(const char* fmt, ...);

MH_STATUS WineSafe_CreateHook(void* target, void* detour, void** original);
MH_STATUS WO_EnableHook(void* target);

namespace CollisionModelCache {

namespace {

constexpr uintptr_t kFindModel = 0x0079B1F0; // __thiscall, ECX = cache, 5 stack args, retn 14h

// Verification sample rates
constexpr unsigned long kVerifyFirst  = 20000;
constexpr unsigned long kResampleMask = 1023;

// sub_79B1F0 takes cache in ECX and 5 stack arguments cleaned via retn 14h.
// In 32-bit MSVC, __fastcall puts 1st arg in ECX, 2nd arg in EDX, and pushes the rest.
// The caller of __thiscall does not touch EDX, leaving it as a dummy parameter.
typedef void* (__fastcall* FindModel_fn)(void* cache, void* edx,
                                         int a0, uint32_t key, int a2, int a3, int a4);
FindModel_fn orig_FindModel = nullptr;

bool g_installed = false;
bool g_dead      = false;

unsigned long g_calls    = 0;
unsigned long g_hits     = 0;
unsigned long g_misses   = 0;
unsigned long g_verified = 0;
unsigned long g_mismatch = 0;

__forceinline void* FindModel_Fast(void* cache, uint32_t key) {
    // Replicate hash from sub_79B1F0:
    // v8 = ((unsigned __int8)(key >> 5) ^ (unsigned __int8)*(_WORD *)(key + 6)) & 0x7F;
    const uint16_t tag = *(const uint16_t*)(uintptr_t)(key + 6);
    const uint32_t set_idx = ((key >> 5) ^ tag) & 0x7Fu;

    const uint32_t* set_keys = (const uint32_t*)cache + (set_idx * 8);

    const __m128i target = _mm_set1_epi32((int)key);
    const __m128i k0 = _mm_loadu_si128((const __m128i*)set_keys);
    const __m128i k1 = _mm_loadu_si128((const __m128i*)(set_keys + 4));

    const __m128i eq0 = _mm_cmpeq_epi32(k0, target);
    const __m128i eq1 = _mm_cmpeq_epi32(k1, target);

    const int m0 = _mm_movemask_ps(_mm_castsi128_ps(eq0));
    const int m1 = _mm_movemask_ps(_mm_castsi128_ps(eq1));
    const int hit_mask = m0 | (m1 << 4);

    if (hit_mask != 0) {
        unsigned long slot_in_set;
        _BitScanForward(&slot_in_set, (unsigned long)hit_mask);
        const uint32_t total_slot = (set_idx << 3) + slot_in_set;
        uint8_t* entry = (uint8_t*)cache + 0x1000 + (total_slot * 0x2460);
        if (entry[4] == 0) {
            return entry;
        }
        return nullptr;
    }

    return (void*)(uintptr_t)0xFFFFFFFF; // Miss sentinel
}

void* __fastcall Hooked_FindModel(void* cache, void* edx,
                                  int a0, uint32_t key, int a2, int a3, int a4) {
    if (g_dead || !cache || key < 0x1000u) {
        return orig_FindModel(cache, edx, a0, key, a2, a3, a4);
    }

    ++g_calls;
    void* fast_res = (void*)(uintptr_t)0xFFFFFFFF;

    __try {
        fast_res = FindModel_Fast(cache, key);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        g_dead = true;
        Log("[CollisionModelCache] Exception during fast lookup, retiring hook\n");
        return orig_FindModel(cache, edx, a0, key, a2, a3, a4);
    }

    if (fast_res == (void*)(uintptr_t)0xFFFFFFFF) {
        // Cache miss: let original code handle insertion, allocation, and population
        ++g_misses;
        return orig_FindModel(cache, edx, a0, key, a2, a3, a4);
    }

    ++g_hits;

    // In verification mode, compare hit result against original client lookup
    const bool should_verify = (g_calls <= kVerifyFirst) || ((g_calls & kResampleMask) == 0);
    if (should_verify) {
        void* orig_res = orig_FindModel(cache, edx, a0, key, a2, a3, a4);
        if (fast_res != orig_res) {
            ++g_mismatch;
            Log("[CollisionModelCache] Verification mismatch! key=0x%08X fast=%p orig=%p\n",
                key, fast_res, orig_res);
            if (g_mismatch > 10) {
                g_dead = true;
                Log("[CollisionModelCache] Too many mismatches, retiring hook\n");
            }
            return orig_res;
        }
        ++g_verified;
    }

    return fast_res;
}

} // namespace

bool Init() {
    if (!Config::g_settings.OptCollisionModelCache) {
        return true;
    }

    if (IsBadReadPtr((void*)kFindModel, 8)) {
        Log("[CollisionModelCache] 0x%08X unreadable - not installing\n", (unsigned)kFindModel);
        return false;
    }

    if (WineSafe_CreateHook((void*)kFindModel, (void*)Hooked_FindModel,
                            (void**)&orig_FindModel) != MH_OK) {
        Log("[CollisionModelCache] hook NOT created\n");
        return false;
    }

    if (WO_EnableHook((void*)kFindModel) != MH_OK) {
        Log("[CollisionModelCache] hook created but could not be enabled\n");
        return false;
    }

    g_installed = true;
    SamplingProfiler::RegisterSelfSymbol("CollisionModelCache_SSE2", (const void*)&Hooked_FindModel);
    Log("[CollisionModelCache] ACTIVE on sub_79B1F0 (0x%08X) - 8-way associative BSP collision model cache lookup. "
        "Replaced 8-iteration scalar loop with dual 128-bit SSE2 vector comparison and bitscan. "
        "Verifying first %lu calls, then 1 in %d.\n",
        (unsigned)kFindModel, kVerifyFirst, (int)(kResampleMask + 1));
    return true;
}

void LogStats() {
    if (!Config::g_settings.OptCollisionModelCache) {
        return;
    }
    if (!g_installed) {
        Log("[CollisionModelCache] not installed - nothing measured\n");
        return;
    }
    if (g_calls == 0) {
        Log("[CollisionModelCache] installed but never called\n");
        return;
    }
    const double hit_pct = (g_calls > 0) ? (100.0 * (double)g_hits / (double)g_calls) : 0.0;
    Log("[CollisionModelCache] %lu calls, %lu hits (%.1f%%), %lu misses, %lu verified, %lu mismatches%s\n",
        g_calls, g_hits, hit_pct, g_misses, g_verified, g_mismatch,
        g_dead ? " - DISABLED" : (g_calls < kVerifyFirst ? " (still verifying)" : ""));
}

} // namespace CollisionModelCache
