#pragma once

#include <windows.h>

// Init/shutdown
bool InitAddonPreload();
void ShutdownAddonPreload();
void ClearAddonPreload();

// Called from hooked_CreateFileA/W to track addon file handles
void AddonPreload_OnCreateFile(HANDLE hFile, const char* filename);
void AddonPreload_OnWriteFile(const char* filename);
void AddonPreload_OnCloseHandle(HANDLE hFile);
bool AddonPreload_TryServe(HANDLE hFile, LPVOID lpBuffer, DWORD nBytes, LPDWORD lpBytesRead);

// Tells the cache whether the file hooks it depends on actually went in. Must be
// called before InitAddonPreload.
void AddonPreload_SetFileHooksInstalled(bool installed);
