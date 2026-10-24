#pragma once

// ============================================================================
// Module: scene_node_active_fast.h
//
// Fastpath for SceneNode::SetActive (sub_823F10, 119 bytes).
// In field profiles (wow_optimize_2026-10-12_08-56-28.log), sub_823F10 was a
// top rendering hotspot (5,343 samples at 0x00823F1A, 0.53% of executing CPU time),
// invoked by 29 callers across the engine whenever doodads, terrain map chunks,
// M2 models, spell visuals, missiles, or game objects toggle visibility/active state.
// ============================================================================

namespace SceneNodeActiveFast {

bool Init();
void Shutdown();
void LogStats();

} // namespace SceneNodeActiveFast
