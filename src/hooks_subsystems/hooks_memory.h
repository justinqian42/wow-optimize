#pragma once

bool InstallMemoryHooks(void);
void ShutdownMemoryHooks(void);
void ClearGuidHashTable(void);  // zeroes the entire 16384-entry GUID cache
