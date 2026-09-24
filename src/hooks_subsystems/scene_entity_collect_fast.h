#pragma once

// ============================================================================
// Module: scene_entity_collect_fast.h
//
// Fast scene entity spatial hash cell traversal and candidate accumulation (sub_7A2760).
// In scene visibility, raycasting, and collision queries (sub_7A3570, sub_7A30D0),
// sub_7A2760 was sampled 251,081 times at 0x007A27F8 as a major traversal bottleneck.
// ============================================================================

namespace SceneEntityCollect {

bool Init();
void Shutdown();
void LogStats();

} // namespace SceneEntityCollect
