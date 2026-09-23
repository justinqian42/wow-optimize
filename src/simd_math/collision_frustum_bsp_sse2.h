// ============================================================================
// Module: collision_frustum_bsp_sse2.h
//
// Iterative world collision BSP tree traversal accelerating sub_7CA440
// (frustum/cone collision queries).
// ============================================================================

#pragma once

namespace CollisionFrustumBsp {

bool Init();
void Shutdown();
void LogStats();

} // namespace CollisionFrustumBsp
