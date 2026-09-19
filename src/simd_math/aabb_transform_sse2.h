#pragma once

// ============================================================================
// Module: aabb_transform_sse2
//
// Hardware double-precision SSE2 rewrite of the bounding-box transformation
// routines:
//   sub_7F9430: 4x4 matrix * AABB transformation (including translation)
//   sub_7F93D0: 3x3 matrix * AABB transformation (orientation only)
//
// Implements Jim Arvo's bounding box transformation algorithm using SSE2
// hardware double precision, eliminating the serialized x87 status-word
// roundtrips (fcom/fnstsw/test) and branch mispredictions per bounding box.
//
// Dual-run verified against client output for bit-exact floating-point results.
// ============================================================================

namespace AabbTransform {

bool Init();
void Shutdown();
void LogStats();

}  // namespace AabbTransform
