#pragma once

// ============================================================================
// Description: Coalescing network/field updates to reduce redundant UI redraws.
// Safety & Threading: Thread-safe, queues updates and flushes on main thread.
// ============================================================================

namespace WorldStateCoalesce {

bool Init();
void Shutdown();
void OnFrame();
bool ProcessFieldUpdate(void* unit, int fieldId, int value, void* orig_func);

} // namespace WorldStateCoalesce
