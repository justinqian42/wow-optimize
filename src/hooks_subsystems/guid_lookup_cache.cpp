// ============================================================================
// Description: Lock-free GUID to Object pointer lookup cache.
// Safety & Threading: Thread-safe, avoids global locks using atomic cache slots.
// ============================================================================

#include "guid_lookup_cache.h"
#include "MinHook.h"
#include "version.h"
#include <windows.h>
#include <atomic>

extern "C" void Log(const char* fmt, ...);

namespace GuidLookupCache {

struct CacheEntry {
    std::atomic<unsigned __int64> guid;
    std::atomic<void*>            obj;
};

static constexpr int CACHE_SIZE = 1024;
static constexpr int CACHE_MASK = CACHE_SIZE - 1;
static CacheEntry g_cache[CACHE_SIZE];

typedef void* (__cdecl *GetObject_fn)(unsigned __int64 guid, int typemask);
static GetObject_fn orig_GetObject = nullptr;

static inline unsigned int HashGuid(unsigned __int64 guid) {
    unsigned int h = (unsigned int)(guid ^ (guid >> 32));
    h ^= h >> 16;
    h *= 0x85ebca6b;
    h ^= h >> 13;
    h *= 0xc2b2ae35;
    h ^= h >> 16;
    return h & CACHE_MASK;
}

static inline bool IsValidPtr(uintptr_t p) {
    return p > 0x10000 && p < 0xFFE00000;
}

static void* __cdecl Hooked_GetObject(unsigned __int64 guid, int typemask) {
    return nullptr;
}

void Invalidate(unsigned __int64 guid) {
    // Disabled for stability
}

bool Init() {
    Log("[GuidLookupCache] Disabled for stability (prevents exit/logout "
        "ACCESS_VIOLATION at 0x5D9DD1). Hooked_GetObject returns null and "
        "nothing installs it, so no lookup has ever been cached.");
    // Returns false: it did not install. Returning true put it in the
    // feature summary as a working feature, which is the same lie the
    // summary was fixed for telling in the other direction.
    return false;
}

void Shutdown() {
    // No-op
}

} // namespace GuidLookupCache
