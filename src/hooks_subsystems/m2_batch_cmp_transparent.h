#pragma once

// Transparent M2 batch sort comparator (sub_81EF30, 0x1A7 bytes).
// Replaces x87 FPU status-word stalls (fnstsw ax) with scalar pipeline comparisons.
// No arithmetic is performed; exact by construction.

namespace M2BatchCmpTransparent {

bool Init();
void Shutdown();
void LogStats();

}  // namespace M2BatchCmpTransparent
