// ============================================================================
// Module: freeze_catcher.h
// Description: Samples the main thread only while a frame is already long.
// Safety & Threading: A watchdog thread; the main thread is touched only during
//                     a frame that has already overrun.
// ============================================================================

#pragma once

#include <windows.h>

namespace FreezeCatcher {

bool Init(HANDLE mainThread);
void OnFrame();
void Shutdown();
void LogStats();

} // namespace FreezeCatcher
