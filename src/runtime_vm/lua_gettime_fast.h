// ============================================================================
// Description: Declarations for accelerating Lua GetTime via frame caching.
// ============================================================================

#pragma once

bool InstallLuaGetTimeFast();
void LuaGetTimeFast_NewFrame();
