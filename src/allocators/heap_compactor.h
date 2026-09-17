#pragma once

#include "version.h"

#if TEST_DISABLE_HEAP_COMPACTOR == 0

bool HeapCompactor_Init();
void HeapCompactor_Shutdown();

// Diagnostic queries
//
// Every one of these hands back the monitor thread's last walk. None of them
// walks on the caller's thread, and that is deliberate: the walk is VirtualQuery
// over all of user address space, the field data has 6183 free regions in the
// low half alone, and a main-thread copy of it running once a second all session
// is the one self-inflicted stall this project has actually measured. Two entry
// points that did walk on the caller's thread used to sit here. Nothing called
// them and the header put no thread rule on them, so they stood as an invitation
// to repeat that; they are gone. A caller that needs a fresher figure has to say
// so by changing the monitor's interval, where the cost is visible.

// The largest free run below 2GB, with the age of the walk it came from. No
// VirtualQuery, so it is safe to call from inside a frame. Age 0 with a result
// of 0 means the monitor has not run yet, not that nothing is free.
extern "C" SIZE_T HeapCompactor_GetLastLowHalf(unsigned long* ageMsOut);

// The same two figures with no age, for the callers that only branch on them.
// Zero means the monitor has not walked yet - the same "not measured" the
// snapshot calls report with a false return, and not a claim that nothing is
// free. memory_pressure_governor and lua_optimize each wrote their own extern
// declaration for this instead of including the header, so a signature change
// would have reached them as a silent link match on the C name.
extern "C" SIZE_T HeapCompactor_GetCachedLargestBlock();
extern "C" SIZE_T HeapCompactor_GetCachedLowHalf();

// The largest free run below 2GB and the sum of all free space there, from the
// same cached walk. False when the monitor has not run yet, which is not the
// same as nothing being free.
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
// Compiled out, so nothing has walked and there is nothing to hand back. The
// callers already print "not measured" for a false return, which is the truth
// here as much as it is before the monitor thread's first pass.
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
// These two are defined on both sides of the #if in heap_compactor.cpp, so they
// are declared here rather than stubbed inline.
extern "C" SIZE_T HeapCompactor_GetCachedLargestBlock();
extern "C" SIZE_T HeapCompactor_GetCachedLowHalf();

#endif
