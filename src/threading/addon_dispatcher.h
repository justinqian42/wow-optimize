#pragma once

// Multithreaded Addon Update Dispatcher
// 

#ifndef ADDON_DISPATCHER_H
#define ADDON_DISPATCHER_H

#include <windows.h>
#include <cstdint>

namespace AddonDispatcher {

// Statistics structure
struct Stats {
    volatile LONG callbacksQueued;      // Total callbacks queued
    volatile LONG callbacksProcessed;   // Total callbacks processed
    volatile LONG callbacksDropped;     // Callbacks dropped (queue full)
    volatile LONG batchesProcessed;     // Total batches processed
    volatile LONG queueDepth;           // Current queue depth
    double totalProcessTimeMs;          // Total processing time in milliseconds
    double avgBatchSize;                // Average batch size
};

// Initialize the multithreaded addon dispatcher
bool Init();

// Shutdown and cleanup
void Shutdown();

// Called from main thread on each frame (for batch processing)
void OnFrame(DWORD mainThreadId);

// Get current statistics
Stats GetStats();

} // namespace AddonDispatcher

#endif // ADDON_DISPATCHER_H
