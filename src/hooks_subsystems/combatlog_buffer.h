#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <cstdint>

namespace CombatLogBuffer {

struct Stats {
    int64_t totalEvents;
    int64_t droppedEvents;
    int64_t forcedFlushes;
    int64_t recycledEntries;
    int64_t allocatedEntries;
    int32_t currentPending;
    int32_t peakPending;
    int32_t ringBufferSize;
    int32_t ringBufferInUse;
};

bool Init();
void OnFrame(DWORD mainThreadId);
void Shutdown();
void LogStats();
Stats GetStats();

} // namespace CombatLogBuffer
