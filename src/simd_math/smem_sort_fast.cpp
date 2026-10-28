// ============================================================================
// Module: smem_sort_fast.cpp
//
// High-performance hybrid Introsort (sub_83DCF0 acceleration).
// Replaces the client's generic heapsort across all engine render sorting loops:
//   - 0x0081DF81: M2 ray hit intersection sorting (cmp = 0x0081CBC0)
//   - 0x0081E215: M2 ray hit distance sorting (cmp = 0x0081CBC0)
//   - 0x0081FAE7: M2 master draw batch sorting (cmp = 0x0081F0E0)
//   - 0x00822E0A: Scene node / mesh draw sorting (cmp = 0x0081EA90)
//   - 0x00822FDD: M2 DrawBatchBuilder solid batches (cmp = 0x0081EEA0)
//   - 0x00822FF0: M2 DrawBatchBuilder transparent batches (cmp = 0x0081EF30)
//   - 0x00823003: M2 DrawBatchBuilder alpha batches (cmp = 0x0081EF30)
//
// In field profile logs, sub_83DCF0 was sampled over 17,000 times inside its
// sift-down loop (wow!0x0083DE09, wow!0x0083DE28, wow!0x0083DE00) due to
// poor cache locality and high comparator invocation counts.
//
// FastSort uses:
//   - Zero-allocation Insertion Sort for N <= 24 (branch-predictable, L1 cache hit)
//   - Median-of-3 QuickSort with Hoare sentinel partitioning for N > 24
//   - Depth-bounded Heapsort fallback (depth <= 2 * floor(log2(N))) to guarantee
//     strict O(N log N) worst-case time complexity.
// Dual-run verified against comparator monotonicity.
// ============================================================================

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <intrin.h>
#include <cstdint>
#include <cstring>

#include "smem_sort_fast.h"
#include "config.h"
#include "session_verdict.h"
#include "sampling_profiler.h"

extern "C" void Log(const char* fmt, ...);

namespace SMemSortFast {

namespace {

constexpr uint32_t kLearnSorts = 1024;
constexpr uint32_t kResampleMask = 127;
constexpr uint32_t kInsertionCutoff = 24;
constexpr uint32_t kPartitionCutoff = 16;

bool g_active = false;
bool g_dead = false;

// Statistics
unsigned long long g_totalSorts = 0;
unsigned long long g_smallSorts = 0;
unsigned long long g_largeSorts = 0;
unsigned long long g_heapFallbacks = 0;
unsigned long long g_verifiedSorts = 0;
unsigned long long g_verifiedCompares = 0;
uint32_t g_largestN = 0;
unsigned g_mismatches = 0;

// Cycle timing
unsigned long long g_fastCycles = 0;
unsigned long long g_fastSortCount = 0;

__forceinline void SiftDown(int32_t* arr, uint32_t root, uint32_t count, CmpFn cmp, void* ctx) {
    int32_t val = arr[root];
    uint32_t curr = root;
    while (true) {
        uint32_t child = 2 * curr + 1;
        if (child >= count) break;
        if (child + 1 < count && cmp((const void*)arr[child + 1], (const void*)arr[child], ctx) > 0) {
            child++;
        }
        if (cmp((const void*)arr[child], (const void*)val, ctx) > 0) {
            arr[curr] = arr[child];
            curr = child;
        } else {
            break;
        }
    }
    arr[curr] = val;
}

// Fallback heapsort for pathological recursion depth limits
__declspec(safebuffers) static void HeapSortSub(int32_t* arr, uint32_t n, CmpFn cmp, void* ctx) {
    if (n <= 1) return;
    for (int32_t i = (int32_t)(n / 2) - 1; i >= 0; --i) {
        SiftDown(arr, (uint32_t)i, n, cmp, ctx);
    }
    for (int32_t i = (int32_t)n - 1; i > 0; --i) {
        int32_t tmp = arr[0];
        arr[0] = arr[i];
        arr[i] = tmp;
        SiftDown(arr, 0, (uint32_t)i, cmp, ctx);
    }
}

// Inlined insertion sort for small subarrays
__declspec(safebuffers) static inline void InsertionSort(int32_t* arr, uint32_t low, uint32_t high, CmpFn cmp, void* ctx) {
    for (uint32_t i = low + 1; i <= high; ++i) {
        int32_t val = arr[i];
        uint32_t j = i;
        while (j > low && cmp((const void*)arr[j - 1], (const void*)val, ctx) > 0) {
            arr[j] = arr[j - 1];
            --j;
        }
        arr[j] = val;
    }
}

// Median-of-3 pivot selection and ordering
__forceinline void Median3(int32_t* arr, uint32_t a, uint32_t b, uint32_t c, CmpFn cmp, void* ctx) {
    if (cmp((const void*)arr[a], (const void*)arr[b], ctx) > 0) {
        int32_t t = arr[a]; arr[a] = arr[b]; arr[b] = t;
    }
    if (cmp((const void*)arr[b], (const void*)arr[c], ctx) > 0) {
        int32_t t = arr[b]; arr[b] = arr[c]; arr[c] = t;
        if (cmp((const void*)arr[a], (const void*)arr[b], ctx) > 0) {
            int32_t t2 = arr[a]; arr[a] = arr[b]; arr[b] = t2;
        }
    }
}

// Tail-recursive introsort loop
__declspec(safebuffers) static void IntrosortLoop(int32_t* arr, uint32_t low, uint32_t high, uint32_t depth_limit, CmpFn cmp, void* ctx) {
    while (high - low > kPartitionCutoff) {
        if (depth_limit == 0) {
            ++g_heapFallbacks;
            HeapSortSub(arr + low, high - low + 1, cmp, ctx);
            return;
        }
        --depth_limit;
        uint32_t mid = low + ((high - low) >> 1);
        Median3(arr, low, mid, high, cmp, ctx);

        // Place pivot at high - 1 (sentinel high is >= pivot)
        int32_t tmp = arr[mid];
        arr[mid] = arr[high - 1];
        arr[high - 1] = tmp;
        int32_t pivot = arr[high - 1];

        // Hoare partition with sentinels
        uint32_t i = low;
        uint32_t j = high - 1;
        while (true) {
            while (cmp((const void*)pivot, (const void*)arr[++i], ctx) > 0);
            while (cmp((const void*)arr[--j], (const void*)pivot, ctx) > 0);
            if (i >= j) break;
            int32_t t = arr[i]; arr[i] = arr[j]; arr[j] = t;
        }
        arr[high - 1] = arr[i];
        arr[i] = pivot;

        // Tail-recurse into the smaller partition to bound stack depth
        if (i - low < high - i) {
            if (i > low + 1) IntrosortLoop(arr, low, i - 1, depth_limit, cmp, ctx);
            low = i + 1;
        } else {
            if (high > i + 1) IntrosortLoop(arr, i + 1, high, depth_limit, cmp, ctx);
            if (i == 0) break;
            high = i - 1;
        }
    }
}

__declspec(noinline) static void Retire(CmpFn cmp, uint32_t idx, int32_t a, int32_t b) {
    ++g_mismatches;
    g_dead = true;
    Log("[SMemSortFast] RETIRED: Comparator 0x%08X violation at index %u: "
        "element %d preceded smaller element %d. Switched off for this session.",
        (unsigned)(uintptr_t)cmp, idx, a, b);
    Verdict::Add(Verdict::Bad, "SMemSortFast encountered a comparator order discrepancy "
                 "and retired itself for this session");
}

} // namespace

bool Init() {
    if (!Config::g_settings.OptSMemSortFast) {
        return false;
    }
    g_active = true;
    SamplingProfiler::RegisterSelfSymbol("SMemSortFast_Sort", (const void*)&Sort);
    Log("[SMemSortFast] ACTIVE: hybrid Introsort (Insertion Sort for N <= %u, "
        "Median-of-3 QuickSort for N > %u with bounded Heapsort fallback). "
        "Accelerates all 7 engine render sorting loops. First %u sorts verified.",
        kInsertionCutoff, kInsertionCutoff, kLearnSorts);
    return true;
}

void Shutdown() {
    g_active = false;
}

bool IsActive() {
    return g_active && !g_dead;
}

__declspec(safebuffers) void Sort(CmpFn cmp, int32_t* arr, uint32_t n, void* ctx) {
    if (n <= 1) return;
    if (g_dead) {
        HeapSortSub(arr, n, cmp, ctx);
        return;
    }

    ++g_totalSorts;
    if (n > g_largestN) g_largestN = n;

    const bool verify = (g_verifiedSorts < kLearnSorts) || ((g_totalSorts & kResampleMask) == 0);
    const unsigned long long t0 = __rdtsc();

    if (n <= kInsertionCutoff) {
        ++g_smallSorts;
        InsertionSort(arr, 0, n - 1, cmp, ctx);
    } else {
        ++g_largeSorts;
        // Compute depth limit = 2 * floor(log2(n))
        unsigned long idx = 0;
        uint32_t depth = 32;
        if (_BitScanReverse(&idx, n)) {
            depth = (uint32_t)(idx << 1);
        }
        IntrosortLoop(arr, 0, n - 1, depth, cmp, ctx);
        InsertionSort(arr, 0, n - 1, cmp, ctx);
    }

    g_fastCycles += (__rdtsc() - t0);
    ++g_fastSortCount;

    if (verify) {
        // Monotonicity verification pass
        for (uint32_t i = 1; i < n; ++i) {
            if (cmp((const void*)arr[i - 1], (const void*)arr[i], ctx) > 0) {
                Retire(cmp, i, arr[i - 1], arr[i]);
                break;
            }
        }
        if (!g_dead) {
            ++g_verifiedSorts;
            g_verifiedCompares += (n - 1);
        }
    }
}

void LogStats() {
    if (!Config::g_settings.OptSMemSortFast) return;
    if (!g_active) {
        Log("[SMemSortFast] not active (disabled in config)");
        return;
    }
    if (g_totalSorts == 0) {
        Log("[SMemSortFast] active, 0 sorts recorded.");
        return;
    }
    Log("[SMemSortFast] %llu total sort(s): %llu small (N <= %u), %llu large (N > %u), "
        "%llu heapsort fallback(s). Largest N = %u.",
        g_totalSorts, g_smallSorts, kInsertionCutoff, g_largeSorts, kInsertionCutoff,
        g_heapFallbacks, g_largestN);
    if (g_mismatches) {
        Log("[SMemSortFast]   DISABLED after comparator discrepancy.");
    } else {
        Log("[SMemSortFast]   %llu sort(s) verified (%llu comparisons), zero order discrepancies.",
            g_verifiedSorts, g_verifiedCompares);
    }
    if (g_fastSortCount > 0) {
        double avgCycles = (double)g_fastCycles / (double)g_fastSortCount;
        Log("[SMemSortFast]   average execution: %.1f cycles per sort over %llu measured sorts.",
            avgCycles, g_fastSortCount);
    }
}

} // namespace SMemSortFast

