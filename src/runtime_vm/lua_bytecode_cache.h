#pragma once

// Caches the precompiled Lua chunk produced by luaL_loadbuffer keyed by
// FNV-1a(source). On hit, replays bytecode to skip the parser entirely.
// Cache cleared on lua_State swap (bytecode is VM-bound).

#include <windows.h>
#include <cstdint>

namespace LuaBytecodeCache {

bool Init();
void Shutdown();
void LogStats();
void OnLuaStateSwap();

struct Stats {
    bool     active;
    uint32_t entries;
    uint64_t hits;
    uint64_t misses;
    uint64_t bypasses;
    uint64_t loadFailures;
    uint64_t bytesCached;
};
void GetStats(Stats* out);

} // namespace LuaBytecodeCache
