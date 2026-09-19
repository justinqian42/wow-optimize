#pragma once

// ============================================================================
// Module: aabb_transform_sse2
//
// Hardware double-precision SSE2 rewrite of bounding-box and coordinate math:
//   sub_7F9430: AABB_Transform       (4x4 matrix * AABB transformation, world/scene engine)
//   sub_7F93D0: AABB_Transform3x3    (3x3 matrix * AABB transformation, orientation only)
//   sub_984860: AABB_TransformAffine (4x4 matrix * AABB transformation, math library / M2 bounds)
//   sub_984930: AABB_FromVertices    (bounding box [min, max] calculation from vertex stream)
//   sub_715130: CAxisAlignedBox::Union (in-place bounding box union)
//   sub_714D10: Vec3_Min             (component-wise minimum of two 3D vectors)
//   sub_714D70: Vec3_Max             (component-wise maximum of two 3D vectors)
//
// Implements Jim Arvo's bounding box transformation algorithm using SSE2
// hardware double precision, and parallel 128-bit vector min/max operations,
// eliminating serialized x87 status-word roundtrips (fcom/fnstsw/test), nested
// stack buffers, and branch mispredictions.
//
// Dual-run verified against client output for bit-exact floating-point results.
// ============================================================================

namespace AabbTransform {

bool Init();
void Shutdown();
void LogStats();

}  // namespace AabbTransform
