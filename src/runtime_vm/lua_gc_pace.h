#pragma once

// The Lua collector's pacing, as a measured lever. See lua_gc_pace.cpp.
namespace LuaGcPace {
bool Init();
void OnFrame();
void Shutdown();
void LogStats();
}
