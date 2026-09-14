#pragma once

// Includes nothing, like the other allocator headers pulled in near the top of
// dllmain.cpp.

namespace HighPlacement {

// Installs the NtAllocateVirtualMemory and NtFreeVirtualMemory hooks when any of
// VaCensus, HighPlacementModules or HighPlacementClient is on. Call after
// MH_Initialize and before whatever should be seen reserving.
bool Init();

// Rebuilds the module list callers are attributed against. It takes the loader
// lock, so it must never run on the hooked path; the heap monitor thread calls
// it, and the periodic report does when that thread is not running.
void RefreshModules();

// Live private reservations by the module that made them. compareWith names a
// figure the caller has just printed that the below-2GB total should roughly
// match, or is null when there is none.
void LogLiveByCaller(bool lowHalfOnly, const char* compareWith);

void LogStats();

}  // namespace HighPlacement
