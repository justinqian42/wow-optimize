#pragma once

// Includes nothing on purpose: version.h defines the MinHook helpers only after
// MinHook.h has been seen, and this header is included near the top of
// dllmain.cpp.

namespace MimallocHighArena {

bool Init();
// Hands over another block before the allocator runs out of what it has. Call
// from a background thread; it reserves address space.
void Grow();
void LogStats();

// How much of the handed-over address space is committed, from a VirtualQuery
// walk of those blocks alone, and how much was handed over. False when nothing
// has been handed over, so a caller can say it did not measure rather than
// print zero.
bool CommittedInArena(unsigned long long* committed, unsigned long long* handed);

// Whether an address lies inside a block this module handed over.
bool Contains(const void* addr);

}  // namespace MimallocHighArena
