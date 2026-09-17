#pragma once

bool InstallLuaGetStrInline();
void UninstallLuaGetStrInline();
void InvalidateLuaGetStrInlineCache();  // zeroes the entire 16384-entry bucket cache
void LuaGetStrInline_LogStats(void);
