#pragma once

// Installs a Lua error diagnostic hook.
// NOTE: 0x84F610 is verified against disassembly as sub_84F610(size_t Size) = luaL_addvalue,
// NOT lua_error. The correct lua_error address must be found via disassembly before
// re-enabling this hook. Gated by TEST_DISABLE_LUA_ERROR_DIAG in version.h.
bool InstallLuaErrorDiag();
