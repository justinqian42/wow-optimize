#pragma once

// Deliberately includes nothing, like the other patch modules pulled in near the
// top of dllmain.cpp.

namespace UiBatchFill {

bool Init();
void Shutdown();
void LogStats();

}  // namespace UiBatchFill
