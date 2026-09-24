// ============================================================================
// Module: terrain_chunk_sort_sse2.h
//
// Terrain map chunk frustum culling, plane equation evaluation, and draw bucket
// insertion (sub_7C3E70).
// ============================================================================

#pragma once

namespace TerrainChunkSort {

bool Init();
void Shutdown();
void LogStats();

} // namespace TerrainChunkSort
