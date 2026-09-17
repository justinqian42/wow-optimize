#pragma once
// ============================================================================
// Description: C-level combat log event pre-parser and aggregator API
// ============================================================================

// Process and aggregate combat log event from the Lua stack
void CombatLogParser_ProcessEvent(void* L, int fieldCount);

// Lua C API bindings for getting and resetting stats
extern "C" int LUABOOST_GetCombatStats(void* L);
extern "C" int LUABOOST_ResetCombatStats(void* L);

// Install/initialize the parser
bool InstallCombatLogParser();

// Shutdown and reset
void ShutdownCombatLogParser();