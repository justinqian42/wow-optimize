// ============================================================================
// Description: Aligned memory pool cache to avoid heap fragmentation.
// ============================================================================

#include "aligned_alloc_cache.h"
#include <windows.h>
#include <MinHook.h>
#include <stdlib.h>
#include <string.h>

extern "C" void Log(const char* fmt, ...);

// ================================================================
// Aligned allocator cache - sub_76E540 (1764 callers)
//
// Original function signature (stdcall, cleans 16 bytes = 4 args):
//   void* __stdcall AlignedAlloc(size_t size, int param2, int exitCode, int flags)
//
// Logic:
//   aligned_size = (size + 7) & ~7   (8-byte alignment)
//   if (flags & 8) result = calloc(1, aligned_size)   (zero memory)
//   else           result = malloc(aligned_size)
//   if (!result)   call error_handler(size, param2, exitCode); return NULL
//   return result;
//
// Optimization: thread-local free lists for common sizes (32/64/128/256).
// ~80% of allocations are small and short-lived, so this eliminates
// malloc/free overhead for them (~100 cycles per call saved).
// ================================================================

// Bucket sizes (aligned to 8 bytes)
static const size_t kBucketSizes[] = { 32, 64, 128, 256 };
static const int kNumBuckets = 4;
static const int kMaxPerBucket = 16;

struct ThreadCache {
    void* free_lists[4];     // linked lists, stored in the first 4 bytes of each block
    int   counts[4];
    bool  initialized;
};

static __declspec(thread) ThreadCache* t_cache = nullptr;

static inline int GetBucket(size_t aligned_size) {
    // Map: 32->0, 64->1, 128->2, 256->3
    switch (aligned_size) {
        case 32:  return 0;
        case 64:  return 1;
        case 128: return 2;
        case 256: return 3;
        default:  return -1;
    }
}

static ThreadCache* GetThreadCache() {
    if (!t_cache) {
        t_cache = (ThreadCache*)calloc(1, sizeof(ThreadCache));
        if (t_cache) t_cache->initialized = true;
    }
    return t_cache;
}

// Original function pointer
using AlignedAllocFn = void* (__stdcall*)(size_t, int, int, int);
static AlignedAllocFn pOrigAlignedAlloc = nullptr;

// Our hook
static void* __stdcall HookedAlignedAlloc(size_t size, int param2, int exitCode, int flags) {
    size_t aligned = (size + 7) & ~7;
    int bucket = GetBucket(aligned);

    // Try thread-local cache
    if (bucket >= 0) {
        ThreadCache* tc = GetThreadCache();
        if (tc && tc->free_lists[bucket]) {
            void* block = tc->free_lists[bucket];
            // Pop from free list (next pointer stored at start of block)
            tc->free_lists[bucket] = *(void**)block;
            tc->counts[bucket]--;
            // Zero if requested
            if (flags & 8) memset(block, 0, aligned);
            return block;
        }
    }

    // Fall through to original
    return pOrigAlignedAlloc(size, param2, exitCode, flags);
}

// We also need to intercept free to recycle blocks back.
// But we don't know the size at free time! Strategy: check if pointer
// is aligned to 32/64/128/256 boundary and peek at _msize to determine bucket.
// Actually, we can't reliably know if a pointer came from us.
//
// Simpler approach: pre-allocate a batch on install, recycle on our own free hook.
// For safety, we'll just skip recycling - the pool is pre-warmed per thread.

bool InstallAlignedAllocCache() {
    // Disabled: the pool was pre-warmed with this DLL's static-CRT malloc and
    // handed to WoW, which frees those blocks through its own CRT (free at
    // 0x00412FC7) -> cross-heap free -> heap corruption. It also never refilled
    // (no safe way to recognize our blocks at free time) and was pre-warmed on
    // a single thread only, so after the first 64 allocations it added bucket
    // dispatch + TLS overhead to all 1764 callers for no benefit. Net-negative
    // and unsafe; let WoW's original aligned allocator run untouched.
    (void)&HookedAlignedAlloc;
    (void)&GetBucket;
    (void)kBucketSizes;
    (void)kMaxPerBucket;
    Log("[AlignedAllocCache] Disabled (cross-heap free hazard, no net benefit)");
    return false;
}

void UninstallAlignedAllocCache() {
    if (pOrigAlignedAlloc) {
        MH_DisableHook((void*)0x0076E540);
        MH_RemoveHook((void*)0x0076E540);
        pOrigAlignedAlloc = nullptr;
    }

    // Free thread-local cache for current thread
    if (t_cache) {
        for (int b = 0; b < kNumBuckets; b++) {
            void* block = t_cache->free_lists[b];
            while (block) {
                void* next = *(void**)block;
                free(block);
                block = next;
            }
        }
        free(t_cache);
        t_cache = nullptr;
    }
}
