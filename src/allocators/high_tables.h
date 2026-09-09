// ============================================================================
// Module: high_tables.h
// Description: Allocates a large lookup table outside this DLL's image, as
//              high in the address space as Windows will place it.
// Safety & Threading: Called from init, before the table is used.
// ============================================================================

#pragma once

#include <cstddef>

namespace HighTables {

// Zeroed on return, like the static array it replaces. Null means the caller
// has no table and must stand down; the refusal is logged and reported.
void* Reserve(const char* name, size_t bytes);

void LogStats();

} // namespace HighTables
