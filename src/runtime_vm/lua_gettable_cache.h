#pragma once

// luaV_gettable Fast Path Cache
// Hooks sub_857250 to cache table+key lookups
bool InstallLuaGetTableCache();
void ShutdownLuaGetTableCache();
