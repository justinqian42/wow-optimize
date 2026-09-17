#pragma once

#include <cstdint>

bool InstallLuaThisCache();
void UninstallLuaThisCache();
void GetLuaThisCacheStats(uint64_t* hits, uint64_t* total);
void LuaThisCache_LogStats();
