#pragma once

// A transcription of the client's Lua interpreter, sub_857CA0, with the string
// key lookup inlined. See lua_vm_fast.cpp.
namespace LuaVmFast {
bool Init();
void Shutdown();
void LogStats();
}
