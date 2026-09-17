#pragma once

#include "version.h"

#if TEST_DISABLE_HEAP_COMPACTOR == 0

bool HeapCompactor_Init();
void HeapCompactor_Shutdown();

// Each of these returns the monitor thread's last walk and never walks on the
// caller's thread: that walk is VirtualQuery over all of user address space.
// For a fresher figure, change the monitor's interval.

// Largest free run below 2GB, and the age of the walk it came from. Age 0 with
// a result of 0 means the monitor has not run yet.
extern "C" SIZE_T HeapCompactor_GetLastLowHalf(unsigned long* ageMsOut);

// The same two figures with no age. Zero means the monitor has not walked yet.
extern "C" SIZE_T HeapCompactor_GetCachedLargestBlock();
extern "C" SIZE_T HeapCompactor_GetCachedLowHalf();

// Largest free run below 2GB and the total free there, from the same walk.
// False when the monitor has not run yet.
extern "C" bool HeapCompactor_GetLowHalfSnapshot(SIZE_T* largestOut,
                                                 SIZE_T* totalOut,
                                                 unsigned long* ageMsOut);

// The largest free run over all of user address space, from the same walk.
extern "C" bool HeapCompactor_GetLastLargestFree(SIZE_T* largestOut,
                                                 unsigned long* ageMsOut);
extern "C" void HeapCompactor_GetStats(uint64_t* checks, uint64_t* compactions,
                                        SIZE_T* lastBlock, SIZE_T* minBlock, SIZE_T* maxBlock);

// Runs any compaction the background monitor thread requested. Must be called
// from the main thread only (see heap_compactor.cpp for why: HeapCompact()/
// mi_collect() must never run off an unsynchronized background thread).
extern "C" void HeapCompactor_RunPendingWork();
extern "C" void HeapCompactor_LogStats();

#else

inline bool HeapCompactor_Init() { return true; }
inline void HeapCompactor_Shutdown() {}
inline void HeapCompactor_RunPendingWork() {}
inline void HeapCompactor_LogStats() {}
// Compiled out: nothing has walked, so there is nothing to hand back.
inline SIZE_T HeapCompactor_GetLastLowHalf(unsigned long* ageMsOut) {
    if (ageMsOut) *ageMsOut = 0;
    return 0;
}
inline bool HeapCompactor_GetLowHalfSnapshot(SIZE_T*, SIZE_T*, unsigned long*) {
    return false;
}
inline bool HeapCompactor_GetLastLargestFree(SIZE_T*, unsigned long*) {
    return false;
}
// Defined on both sides of the #if in heap_compactor.cpp, so declared not stubbed.
extern "C" SIZE_T HeapCompactor_GetCachedLargestBlock();
extern "C" SIZE_T HeapCompactor_GetCachedLowHalf();

#endif
