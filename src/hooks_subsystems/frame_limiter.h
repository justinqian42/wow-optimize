#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

// ============================================================================
// Description: Custom high-precision hybrid frame rate limiter.
// Safety & Threading: Thread-safe. Integrates with the main render thread.
// ============================================================================

namespace FrameLimiter {

// Initialize the high-precision frame rate limiter hooks
bool Init();

// Shutdown the module and release hooks
void Shutdown();

// Thread-local flag to bypass original Sleep calls during limiter loops
extern thread_local bool g_bypassSleep;

extern LARGE_INTEGER g_frameStartQpc;
extern double g_ticksPerSec;
double GetActiveFrameElapsedTime();

} // namespace FrameLimiter
