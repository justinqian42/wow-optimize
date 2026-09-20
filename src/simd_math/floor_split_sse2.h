#pragma once

// The client's float split into an integer part and a remainder, sub_5FE800.
// See floor_split_sse2.cpp.
namespace FloorSplit {
bool Init();
void Shutdown();
void LogStats();
}
