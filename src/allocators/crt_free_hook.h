#pragma once

// ============================================================================
// Description: Removes the discarded _msize call from WoW's CRT free wrapper.
// ============================================================================

#include <cstdint>

bool InstallCrtFreeHook();

// Same defect on the allocation wrapper. Separate switch - see the note in the
// .cpp on why these are not one setting.
bool InstallCrtAllocHook();
void UninstallCrtFreeHook();

// Prints how many deallocations went through the replacement, or says plainly
// that it was never installed - those are different facts and the log has to
// keep them apart.
void ReportCrtFreeStats();

void GetCrtFreeStats(uint64_t* hits, uint64_t* total);
