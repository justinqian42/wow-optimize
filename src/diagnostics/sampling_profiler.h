#pragma once

// A background thread samples the main thread's EIP about every 1 ms through
// SuspendThread/GetThreadContext/ResumeThread, buckets each sample by nearest
// known function, and dumps the top of the profile to the log. Read-only: no
// hooks into client code, no writes to client memory.

#include <windows.h>
#include <cstdint>

namespace SamplingProfiler {

// Initialize the profiler. Stores the main thread handle for sampling.
// Call after the main thread ID is known (post-injection delay).
bool Init(HANDLE mainThread);

// Stop the sampling thread and dump results to the log.
// Call during DLL shutdown before closing handles.
void Shutdown();

// Returns true if the profiler is actively sampling.
bool IsActive();

// Get total number of samples collected (for diagnostics).
uint64_t GetSampleCount();
// Share of main-thread samples executing rather than blocked, from the last
// report. False when none has run. A frame-time comparison in a session with a
// low share cannot show a CPU saving.
bool GetExecutingShare(double* pct, unsigned long long* samples);

// Dump the current top-50 hot functions to the log without stopping sampling.
// Called from the periodic stats dump so the profile is captured even when the
// fast process-exit path skips Shutdown().
void DumpNow();

// Names one of our own functions, so the profile says which of this DLL's hooks
// costs time instead of printing an offset that only matches one build's .map.
// Call at install time with the detour's own address; resolution happens only
// when the profile is printed.
void RegisterSelfSymbol(const char* name, const void* addr);

// What share of the profile sits inside [lo, hi), measured over the same window
// the periodic dump uses and never over the lifetime total: a windowed count
// divided by a lifetime total understates by several times.
//
// False when there is nothing to answer with - profiler off, or fewer than
// minSamples in the window. A caller that gets false must say it could not see
// this rather than print a zero.
bool ShareForRange(uintptr_t lo, uintptr_t hi, unsigned long minSamples,
                   double* outPercent, unsigned long* outSamples,
                   unsigned long* outWindow);

// Where a single loading screen went. MarkLoadWindowStart copies the histogram,
// ReportLoadWindow prints the difference, so the answer is about that one load
// rather than every load of the session.
void MarkLoadWindowStart();
void ReportLoadWindow();

} // namespace SamplingProfiler