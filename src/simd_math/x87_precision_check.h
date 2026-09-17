// ============================================================================
// Description: Measures the floating point control state every SIMD
//              replacement in this project assumes.
// Safety & Threading: Reads two registers.
// ============================================================================

#pragma once

namespace X87Precision {

// Records the control state at a named moment. A moment is sampled once, so
// this is safe to call from the frame path.
void Sample(const char* when);

// True when the last sample said 53-bit, which is what the SSE2 replacements
// here were written against.
bool IsDouble();

void LogStats();

} // namespace X87Precision
