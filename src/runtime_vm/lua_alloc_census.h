#pragma once

// ============================================================================
// Description: Counts Lua VM allocations by size, through G->frealloc.
// ============================================================================

namespace LuaAllocCensus {

// Installs the pass-through counter on the current lua_State, or reinstalls it
// after a reload. Cheap to call every frame once installed.
void EnsureInstalled();

// Prints how many objects the VM allocated and their size distribution - the
// number that decides whether a dedicated Lua arena is worth building.
void LogStats();

}
