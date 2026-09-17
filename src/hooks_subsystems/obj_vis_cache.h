#pragma once

#include <cstdint>
#include <windows.h>

namespace ObjVisCache {

bool Init();
void Shutdown();
void LogStats();
void OnFrame();  // Call once per frame on main thread to invalidate stale entries

} // namespace ObjVisCache
