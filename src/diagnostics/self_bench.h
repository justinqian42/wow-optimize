// ============================================================================
// Module: self_bench.h
// Description: Times a replacement against the client on the same input, using
//              the pairs the verification phase already produces.
// Safety & Threading: Main thread, inside a verification path.
// ============================================================================

#pragma once

#include <cstdint>

namespace SelfBench {

// Once, at init. Returns a slot id, or -1 when there is no room.
int Register(const char* name);

// One pair: the same input through both implementations, in cycles.
void Pair(int id, uint64_t oursCycles, uint64_t theirsCycles);

// The clock both halves are timed with.
uint64_t Now();

void LogStats();

} // namespace SelfBench
