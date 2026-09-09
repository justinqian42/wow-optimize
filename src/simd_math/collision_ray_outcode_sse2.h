// ============================================================================
// Module: collision_ray_outcode_sse2.h
// Description: SSE2 AABB outcode classification for the collision ray cast.
// Safety & Threading: Main thread, same as the function it patches.
// ============================================================================

#pragma once

namespace CollisionRayOutcode {

bool Init();
void Shutdown();
void LogStats();

} // namespace CollisionRayOutcode
