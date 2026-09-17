#include "version.h"
#include "MinHook.h"
#include "crash_dumper.h"
#include <windows.h>
#include <cstdint>
#include <cstring>

extern "C" void Log(const char* fmt, ...);

// sub_857250 signature: _DWORD* __cdecl luaV_gettable(int L, int* table, int* key, _DWORD* result)
typedef void* (__cdecl* luaV_gettable_fn)(int L, int* table, int* key, void* result);
static luaV_gettable_fn orig_luaV_gettable = nullptr;

// Cache entry: stores table ptr + key identity -> cached TValue result
static constexpr int GETTABLE_CACHE_SIZE = 4096;
static constexpr int GETTABLE_CACHE_MASK = GETTABLE_CACHE_SIZE - 1;

struct GetTableCacheEntry {
    uintptr_t tablePtr;      // Table* pointer
    uint64_t  keyIdentity;   // For strings: TString*, for numbers: bit-cast double
    int       keyType;       // TValue type tag (3=number, 4=string)
    uint32_t  resultData[4]; // Cached TValue (16 bytes)
    bool      valid;
};

static GetTableCacheEntry g_gettableCache[GETTABLE_CACHE_SIZE] = {};
static volatile LONG64 g_gettableHits = 0;
static volatile LONG64 g_gettableMisses = 0;

// Generation counter - incremented on lua_State swap to invalidate stale entries
static volatile LONG g_gettableGen = 0;

static inline uint64_t ComputeKeyIdentity(int* keyTValue) {
    int tt = keyTValue[2];
    if (tt == 4) {
        // String key: use TString* pointer as identity
        return (uint64_t)(uintptr_t)keyTValue[0];
    } else if (tt == 3) {
        // Number key: use bit-cast double
        uint64_t bits;
        memcpy(&bits, keyTValue, 8);
        return bits;
    }
    // Other types: don't cache
    return 0;
}

static inline uint32_t HashEntry(uintptr_t tablePtr, uint64_t keyIdentity) {
    uint64_t h = (uint64_t)tablePtr ^ keyIdentity;
    h *= 0xCBF29CE484222325ULL;
    h ^= h >> 33;
    return (uint32_t)(h & GETTABLE_CACHE_MASK);
}

static void* __cdecl Hooked_luaV_gettable(int L, int* table, int* key, void* result) {
    // Only cache string and number keys on table objects
    if (table && key && result) {
        __try {
            int tableTT = table[2];
            int keyTT = key[2];

            // Only cache lookups on actual tables (tt=5) with string/number keys
            if (tableTT == 5 && (keyTT == 4 || keyTT == 3)) {
                uintptr_t tablePtr = (uintptr_t)table[0];
                uint64_t keyId = ComputeKeyIdentity(key);

                if (tablePtr > 0x10000 && tablePtr < 0xFFE00000 && keyId != 0) {
                    uint32_t idx = HashEntry(tablePtr, keyId);
                    GetTableCacheEntry* e = &g_gettableCache[idx];

                    // Check cache hit
                    if (e->valid && e->tablePtr == tablePtr &&
                        e->keyIdentity == keyId && e->keyType == keyTT) {
                        // Validate cached result is still in valid memory range
                        uint32_t* dst = (uint32_t*)result;
                        dst[0] = e->resultData[0];
                        dst[1] = e->resultData[1];
                        dst[2] = e->resultData[2];
                        dst[3] = e->resultData[3];
                        ++g_gettableHits;
                        return result;
                    }

                    // Cache miss - call original and store result
                    void* ret = orig_luaV_gettable(L, table, key, result);

                    // Only cache non-nil results (nil means key not found, may change)
                    uint32_t* src = (uint32_t*)result;
                    if (src[2] != 0) {  // tt != nil
                        e->tablePtr = tablePtr;
                        e->keyIdentity = keyId;
                        e->keyType = keyTT;
                        e->resultData[0] = src[0];
                        e->resultData[1] = src[1];
                        e->resultData[2] = src[2];
                        e->resultData[3] = src[3];
                        e->valid = true;
                    }

                    ++g_gettableMisses;
                    return ret;
                }
            }
        } __except(EXCEPTION_EXECUTE_HANDLER) {
            // Fall through to original on any exception
        }
    }

    return orig_luaV_gettable(L, table, key, result);
}

// ================================================================
// luaV_settable hook - provides write-through cache invalidation
// When t[k] = v executes, invalidate any cached gettable entry
// for the same (table, key) pair so subsequent reads see the new value.
// ================================================================
typedef void* (__cdecl* luaV_settable_fn)(int L, int* table, int* key, int* val);
static luaV_settable_fn orig_luaV_settable = nullptr;

static void InvalidateGetTableCache(uintptr_t tablePtr, uint64_t keyId, int keyType) {
    // Direct-mapped: only one slot to check per (table,key) pair
    uint32_t idx = HashEntry(tablePtr, keyId);
    GetTableCacheEntry* e = &g_gettableCache[idx];
    if (e->valid && e->tablePtr == tablePtr &&
        e->keyIdentity == keyId && e->keyType == keyType) {
        e->valid = false;
    }
}

static void* __cdecl Hooked_luaV_settable(int L, int* table, int* key, int* val) {
    // Invalidate cache BEFORE the write so no stale reads possible
    if (table && key) {
        __try {
            int tableTT = table[2];
            int keyTT = key[2];
            if (tableTT == 5 && (keyTT == 4 || keyTT == 3)) {
                uintptr_t tablePtr = (uintptr_t)table[0];
                uint64_t keyId = ComputeKeyIdentity(key);
                if (tablePtr > 0x10000 && tablePtr < 0xFFE00000 && keyId != 0) {
                    InvalidateGetTableCache(tablePtr, keyId, keyTT);
                }
            }
        } __except(EXCEPTION_EXECUTE_HANDLER) {}
    }
    return orig_luaV_settable(L, table, key, val);
}

bool InstallLuaGetTableCache() {
    // DISABLED: Causes freeze on world entry. The gettable/settable cache
    // invalidation has a race condition with Lua GC or table rehashing
    // during the loading->world transition. Needs further investigation.
    Log("[GetTableCache] DISABLED (freezes on world entry - needs investigation)");
    CrashDumper::RegisterFeature("LuaGetTableCache");
    CrashDumper::FeatureSetActive("LuaGetTableCache", false);
    return false;
}

void ShutdownLuaGetTableCache() {
    // No-op: feature is disabled
}
