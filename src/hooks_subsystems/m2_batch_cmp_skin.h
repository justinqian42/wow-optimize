#pragma once

// M2 model skin/submesh batch sort comparator (sub_824B70, 234 bytes).
// In profile wow_optimize_2026-09-22_15-28-41.log, sub_824B70 was sampled
// 3,864 times (1.40% of executing time) at 0x00824C1F.
// Invoked as the ordering predicate in quicksort / intro-sort (sub_82E840,
// sub_82DC10, sub_82BC20) when sorting model skin submesh batches.
// Pure comparison function returning true if a1 < a2, false otherwise.

namespace M2BatchCmpSkin {

bool Init();
void Shutdown();
void LogStats();

}  // namespace M2BatchCmpSkin
