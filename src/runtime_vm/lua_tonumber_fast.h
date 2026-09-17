#pragma once

#include <cstdint>

// Lua tonumber fast path optimization
// Hooks lua_tonumber (0x0084E0E0) to skip type conversion for numbers
bool InstallLuaToNumberFast();

// Statistics
void GetLuaToNumberStats(uint64_t* fast_path, uint64_t* slow_path);
