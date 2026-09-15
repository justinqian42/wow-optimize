#pragma once

// ============================================================================
// Module: lock_tuning.h
// ============================================================================










// Reduces lock-contention stalls by giving WoW's critical sections a userspace
// spin count. WoW's static MSVC CRT created its locks (heap, stdio, errno, ...)
// with InitializeCriticalSection -- spin count 0 -- so every contended acquisition
// is a kernel wait + context switch. On a many-core CPU a brief spin is far
// cheaper. Semantics are unchanged; only the spin-before-block behaviour differs.
// retrofit tunes the client's own locks; hookInitCS installs the process-wide
// InitializeCriticalSection hook, which every module goes through.
bool InstallLockTuning(bool retrofit, bool hookInitCS);
// Periodic report: what each half did, or that it did not run.
void LogLockTuningStats();
