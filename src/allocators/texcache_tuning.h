#pragma once

// Raises WoW's resident-texture cache budget so an area's working set of textures
// stays resident instead of being evicted and reloaded constantly. See the .cpp
// for the full binary rationale. Tick() re-asserts after device resets.
void InitTexCacheTuning();
void TexCacheTuning_Tick();

// Dynamic budget control for the memory-pressure governor.
// SetBudget(N) writes directly to 0xB49C9C (no bound check — caller is trusted).
// GetConfiguredTarget() returns the budget selected at init (128/96 MB).
void TexCache_SetBudget(int bytes);
int  TexCache_GetConfiguredTarget();
