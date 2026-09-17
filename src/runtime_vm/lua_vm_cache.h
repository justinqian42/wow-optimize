#pragma once

bool InstallLuaVMCache();
void ClearTableCache();
void GetTableCacheStats(long long* hits, long long* misses);
