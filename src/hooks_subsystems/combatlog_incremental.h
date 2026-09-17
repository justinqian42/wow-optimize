#pragma once

// ============================================================================
// Description: Batching and incremental parsing of combat log events.
// Safety & Threading: Thread-safe queue, invoked on main thread OnFrame.
// ============================================================================

namespace CombatLogIncremental {

bool Init();
void Shutdown();
void OnFrame(int luaState);
bool ShouldDefer(void* This, int luaState, void* orig_func);

} // namespace CombatLogIncremental
