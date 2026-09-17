#pragma once

bool InstallLuaRawGetIInline();
void UninstallLuaRawGetIInline();
void ClearRawGetIInlineCache();  // zeroes the entire 8192-entry bucket cache
void LuaRawGetIInline_LogStats(void);
