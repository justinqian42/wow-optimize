#pragma once

// Master M2 batch sort comparator (sub_81F0E0, 227 bytes / 0xE3).
// Dispatched by heapsort (sub_83DCF0) in M2_DrawBatchBuilder (0x0081FAE2).
// In profile logs (wow_optimize_2026-09-22_17-28-51.log), sub_81F0E0 was sampled
// 15,561 times at 0x0081F107 (~1.5% of executing time).
// Compares priority, batch types, and inlines sub_81CA80 bitfield extraction
// and pointer delta without function call overhead.

namespace M2BatchCmpTop {

bool Init();
void Shutdown();
void LogStats();

}  // namespace M2BatchCmpTop
