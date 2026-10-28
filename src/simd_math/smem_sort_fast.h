// ============================================================================
// Module: smem_sort_fast.h
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

#pragma once

#include <cstdint>

namespace SMemSortFast {

typedef int (__cdecl *CmpFn)(const void* a, const void* b, void* ctx);

bool Init();
void Shutdown();
void LogStats();

// High-speed introsort for 32-bit elements (pointers or indices)
void Sort(CmpFn cmp, int32_t* arr, uint32_t n, void* ctx);

// Query active state
bool IsActive();

} // namespace SMemSortFast

