#pragma once

// The polygon-against-plane clip in the client's Collide.cpp, sub_75B710.
// See collision_poly_clip_sse2.cpp.
namespace CollisionPolyClip {
bool Init();
void Shutdown();
void LogStats();
}
