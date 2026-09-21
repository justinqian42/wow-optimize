// ============================================================================
// Module: collision_reset_visited.h
//
// Fast reset for the collision visited-flags array in sub_7C7610.
// ============================================================================

#pragma once

namespace CollisionResetVisited {

bool Init();
void Shutdown();
void LogStats();

} // namespace CollisionResetVisited
