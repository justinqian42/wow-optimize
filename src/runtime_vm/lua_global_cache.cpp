#include "lua_global_cache.h"
#include <windows.h>

extern "C" void Log(const char* fmt, ...);

bool InstallLuaGlobalCache() {
    // Disabled - function signature mismatch
    Log("[LuaGlobalCache] Disabled - prototype mismatch (3 args, not 2)");
    return false;
}

void UninstallLuaGlobalCache() {}
