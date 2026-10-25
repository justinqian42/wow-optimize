#pragma once

// ============================================================================
// Module: detail_doodad_vertices_sse2.h
//
// Vectorized vertex generation and rotation for ground clutter, grass, and
// detail doodads in CDetailDoodadInstBatch::FillVertices (sub_7B1B50).
// ============================================================================

namespace DetailDoodadVertices {
bool Init();
void Shutdown();
void LogStats();
}
