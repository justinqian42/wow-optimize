// ============================================================================
// Description: Cache lookup for Client Object Manager queries to optimize
//              GUID-to-Object lookups.
// Safety & Threading: Thread-safe pool of TLB slots. Invalidated on unlink.
// ============================================================================

#include "obj_vis_cache.h"
#include "version.h"
#include "MinHook.h"
#include "lua_optimize.h"
#include <cstring>
#include <intrin.h>

extern "C" void Log(const char* fmt, ...);
#include "crash_dumper.h"
#include "sampling_profiler.h"

#if !TEST_DISABLE_OBJ_VIS_CACHE

static constexpr int CACHE_SIZE = 128;
static constexpr int CACHE_MASK = CACHE_SIZE - 1;
static constexpr int MAX_THREADS = 32;

struct CacheEntry {
    void*    thisPtr;      // Hash table owner ptr (safety check)
    uint32_t hashKey;
    uint32_t guidLow;
    uint32_t guidHigh;
    void*    result;
    uint32_t frameStamp;
};

struct ThreadCacheSlot {
    volatile LONG  tid;          // 0 = unused, otherwise thread ID
    CacheEntry     entries[CACHE_SIZE];
    uint32_t       lastFrame;
};

static ThreadCacheSlot g_cachePool[MAX_THREADS];
static volatile LONG g_frameCounter = 0;
static int g_featureToken = -1;
static volatile LONG g_poolInitDone = 0;

static volatile LONG g_cacheHits = 0;
static volatile LONG g_cacheMisses = 0;
static volatile LONG g_poolExhausted = 0;

typedef void* (__thiscall *HashLookup_fn)(void* thisPtr, uint32_t hashKey, void* guidPtr);
static HashLookup_fn orig_HashLookup = nullptr;

static inline uint32_t HashKey(uint32_t hashKey, uint32_t low, uint32_t high) {
    uint32_t h = hashKey ^ (low * 0x9E3779B9u) ^ (high * 0x85EBCA6Bu);
    h ^= h >> 16;
    h *= 0xC2B2AE35u;
    h ^= h >> 13;
    return h;
}

static ThreadCacheSlot* GetThreadCacheSlot() {
    DWORD tid = GetCurrentThreadId();

    for (int i = 0; i < MAX_THREADS; i++) {
        if ((DWORD)g_cachePool[i].tid == tid) {
            return &g_cachePool[i];
        }
    }

    for (int i = 0; i < MAX_THREADS; i++) {
        if (InterlockedCompareExchange(&g_cachePool[i].tid, (LONG)tid, 0) == 0) {
            memset((void*)g_cachePool[i].entries, 0, sizeof(g_cachePool[i].entries));
            g_cachePool[i].lastFrame = 0;
            return &g_cachePool[i];
        }
    }

    InterlockedIncrement(&g_poolExhausted);
    return nullptr;
}

typedef int (__cdecl *sub_5D9D90_fn)();
static sub_5D9D90_fn orig_sub_5D9D90 = nullptr;
static volatile LONG g_inWorldTeardown = 0;

static int __cdecl hooked_sub_5D9D90() {
    InterlockedExchange(&g_inWorldTeardown, 1);
    int result = orig_sub_5D9D90();
    InterlockedExchange(&g_inWorldTeardown, 0);
    return result;
}

static inline bool IsTeardownState() {
    uintptr_t gL = *(uintptr_t*)0x00D3F78C;
    return (gL < 0x10000 || gL > 0xFFE00000);
}

__declspec(noinline) static bool ValidateCachedObject(const void* resObj, uint64_t expectedGuid) {
    __try {
        const uint64_t actualGuid = *(const uint64_t*)((const char*)resObj + 48);
        return (actualGuid == expectedGuid);
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

static void* __fastcall hooked_HashLookup(
    void* thisPtr, void* /*edx*/, uint32_t hashKey, void* guidPtr)
{
    if (!thisPtr || !guidPtr) {
        return orig_HashLookup(thisPtr, hashKey, guidPtr);
    }

    if ((uintptr_t)guidPtr < 0x10000 || (uintptr_t)guidPtr > 0x7FFE0000) {
        return orig_HashLookup(thisPtr, hashKey, guidPtr);
    }

    if (g_inWorldTeardown || IsTeardownState() || LuaOpt::IsReloading() || LuaOpt::IsSwapping()) {
        return orig_HashLookup(thisPtr, hashKey, guidPtr);
    }

    const uint32_t* gp = (const uint32_t*)guidPtr;
    uint32_t guidLow  = gp[0];
    uint32_t guidHigh = gp[1];

    ThreadCacheSlot* slot = GetThreadCacheSlot();
    if (!slot) {
        return orig_HashLookup(thisPtr, hashKey, guidPtr);
    }

    uint32_t currentFrame = (uint32_t)InterlockedCompareExchange(&g_frameCounter, 0, 0);
    uint32_t idx = HashKey(hashKey, guidLow, guidHigh) & CACHE_MASK;
    CacheEntry* entry = &slot->entries[idx];

    if (entry->frameStamp == currentFrame &&
        entry->thisPtr == thisPtr &&
        entry->hashKey == hashKey &&
        entry->guidLow == guidLow &&
        entry->guidHigh == guidHigh)
    {
        void* resObj = entry->result;
        if (resObj && (uintptr_t)resObj > 0x10000 && (uintptr_t)resObj < 0x7FFE0000) {
            uint64_t expectedGuid = ((uint64_t)guidHigh << 32) | guidLow;
            if (ValidateCachedObject(resObj, expectedGuid)) {
                InterlockedIncrement(&g_cacheHits);
                CrashDumper::FeatureHit(g_featureToken);
                return resObj;
            }
        }
        entry->frameStamp = 0; // invalidate slot
    }

    void* result = orig_HashLookup(thisPtr, hashKey, guidPtr);

    entry->thisPtr = thisPtr;
    entry->hashKey = hashKey;
    entry->guidLow = guidLow;
    entry->guidHigh = guidHigh;
    entry->result = result;
    entry->frameStamp = currentFrame;

    InterlockedIncrement(&g_cacheMisses);
    return result;
}

__declspec(noinline) static void InvalidateEntry(const void* This) {
    __try {
        const uint32_t* j = (const uint32_t*)This;
        uint32_t hashKey = j[6];
        uint32_t guidLow = j[12];
        uint32_t guidHigh = j[13];
        
        // Invalidate the cache entry across all threads
        uint32_t idx = HashKey(hashKey, guidLow, guidHigh) & CACHE_MASK;
        for (int i = 0; i < MAX_THREADS; i++) {
            CacheEntry* entry = &g_cachePool[i].entries[idx];
            if (entry->hashKey == hashKey &&
                entry->guidLow == guidLow &&
                entry->guidHigh == guidHigh)
            {
                entry->frameStamp = 0; // invalidate slot
            }
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {}
}

// Public Invalidation API (called from Hooked_UnlinkNode in dllmain.cpp)
extern "C" void InvalidateObjVisCacheFor(void* This) {
    if (This && g_poolInitDone && (uintptr_t)This >= 0x10000 && (uintptr_t)This <= 0x7FFE0000) {
        InvalidateEntry(This);
    }
}

MH_STATUS WineSafe_CreateHook(void* target, void* detour, void** original);
MH_STATUS WO_EnableHook(void* target);

namespace ObjVisCache {

bool Init() {
    Log("[ObjVisCache] Init");

    void* target = (void*)0x4D4BB0;
    void* teardownTarget = (void*)0x5D9D90;

    if (!WowOpt_ClientPatchAllowed(target) || !WowOpt_ClientPatchAllowed(teardownTarget)) {
        Log("[ObjVisCache] Client patches disallowed by policy - not hooking");
        return false;
    }

    static const unsigned char kExp_HashLookup[8] = { 0x55, 0x8B, 0xEC, 0x8B, 0x41, 0x24, 0x83, 0xF8 };
    static const unsigned char kExp_Teardown[8]   = { 0x55, 0x8B, 0xEC, 0x51, 0x53, 0x56, 0x57, 0xBF };

    if (IsBadReadPtr(target, 8) || memcmp(target, kExp_HashLookup, 8) != 0) {
        Log("[ObjVisCache] 0x%08X bad prologue - not installing", (unsigned)target);
        return false;
    }
    if (IsBadReadPtr(teardownTarget, 8) || memcmp(teardownTarget, kExp_Teardown, 8) != 0) {
        Log("[ObjVisCache] 0x%08X bad prologue - not installing", (unsigned)teardownTarget);
        return false;
    }

    memset((void*)g_cachePool, 0, sizeof(g_cachePool));
    InterlockedExchange(&g_poolInitDone, 1);

    if (WineSafe_CreateHook(target, (void*)hooked_HashLookup, (void**)&orig_HashLookup) != MH_OK) {
        Log("[ObjVisCache] ERROR: WineSafe_CreateHook failed at 0x4D4BB0");
        return false;
    }
    if (WO_EnableHook(target) != MH_OK) {
        Log("[ObjVisCache] ERROR: WO_EnableHook failed");
        MH_RemoveHook(target);
        return false;
    }

    if (WineSafe_CreateHook(teardownTarget, (void*)hooked_sub_5D9D90, (void**)&orig_sub_5D9D90) == MH_OK) {
        WO_EnableHook(teardownTarget);
    }

    g_featureToken = CrashDumper::FeatureTokenForCounting("ObjVisCache");
    SamplingProfiler::RegisterSelfSymbol("objvis_lookup", (const void*)&hooked_HashLookup);
    Log("[ObjVisCache] [ OK ] Hooked lookup, static pool=%d threads (no alloc)", MAX_THREADS);
    return true;
}

// Printed from the periodic report, not only from Shutdown. The DLL exits
// through TerminateProcess to avoid deadlocking on background threads, so
// Shutdown does not run and these numbers reached no log at all - four tester
// sessions contain none of them.
void LogStats() {
    LONG hits = g_cacheHits;
    LONG misses = g_cacheMisses;
    LONG total = hits + misses;
    double rate = total > 0 ? (100.0 * hits / total) : 0.0;

    Log("[ObjVisCache] hits=%d, misses=%d, rate=%.1f%%, exhausted=%d",
        hits, misses, rate, g_poolExhausted);
}

void Shutdown() {
    LogStats();

    void* target = (void*)0x4D4BB0;
    void* teardownTarget = (void*)0x5D9D90;

    MH_DisableHook(target);
    MH_RemoveHook(target);

    MH_DisableHook(teardownTarget);
    MH_RemoveHook(teardownTarget);

    InterlockedExchange(&g_poolInitDone, 0);
}

void OnFrame() {
    InterlockedIncrement(&g_frameCounter);
}

} // namespace ObjVisCache

#else  // TEST_DISABLE_OBJ_VIS_CACHE

namespace ObjVisCache {
bool Init() { Log("[ObjVisCache] DISABLED (feature flag)"); return false; }
void Shutdown() {}
void LogStats() { Log("[ObjVisCache] compiled out - nothing measured"); }
void OnFrame() {}
} // namespace ObjVisCache

#endif
