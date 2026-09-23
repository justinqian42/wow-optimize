// ============================================================================
// Module: scene_light_grid_sse2.h
//
// Scene dynamic light 64x64 grid traversal and bounds derivation (sub_81E400).
// ============================================================================

#pragma once

namespace SceneLightGrid {

bool Init();
void Shutdown();
void LogStats();

} // namespace SceneLightGrid
