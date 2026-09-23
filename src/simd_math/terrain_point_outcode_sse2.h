// ============================================================================
// Module: terrain_point_outcode_sse2.h
//
// Vectorized 3D point-vs-AABB Cohen-Sutherland outcode for terrain chunk
// culling and polygon generation (sub_7A61D0).
// ============================================================================

#pragma once

namespace TerrainPointOutcode {

bool Init();
void Shutdown();
void LogStats();

} // namespace TerrainPointOutcode
