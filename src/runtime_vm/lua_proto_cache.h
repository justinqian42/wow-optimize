// ============================================================================
// Description: Skips luaY_parser for source the client has already compiled.
// ============================================================================

#pragma once

namespace LuaProtoCache {

bool Init();
void LogStats();

// Called when another module observes the client swapping its lua_State. Do not
// detect the swap here by comparing l_G: the client's Lua pool routinely hands
// the new global state the old one's address, so the comparison misses it and
// every kept Proto becomes a pointer into freed memory.
void OnLuaStateSwapped();

} // namespace LuaProtoCache
