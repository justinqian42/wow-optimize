// ============================================================================
// Module: high_tables
// Description: Puts this DLL's large lookup tables in the top half of the
//              address space instead of in its own image.
// Safety & Threading: Called from init, before the tables are used.
// ============================================================================
//
// A 32-bit client with the large-address-aware flag gets three gigabytes, and
// the two halves are not interchangeable. The client's own allocator works out
// of the low two, and that is the half which has run out on tester machines:
// one of them sat at a one-megabyte largest free block for fifteen minutes and
// wrote a SavedVariables file under a garbage name because of it. The top
// gigabyte, meanwhile, is mostly empty.
//
// This DLL is loaded into the low half and everything static in it is mapped
// there. Measured on the 3.19.2 build, .data is 12.4 MB of a 13.4 MB image, and
// the largest contributors are lookup tables that could live anywhere:
//
//     api_cache            3616 KB
//     dllmain              2238 KB
//     regex_cache          2182 KB
//     lua_vm_engine        1024 KB
//     sampling_profiler     831 KB
//     hooks_memory          513 KB
//     addon_dispatcher      448 KB
//
// Reserving a table through here instead moves it out of the image and into
// whatever VirtualAlloc gives back for MEM_TOP_DOWN, which on a three-gigabyte
// process is the top of the range. Nothing about the table changes - the
// pointer indexes the same way the array did, and VirtualAlloc hands back
// zeroed pages, which is what a zero-initialised static was relied on for.
//
// Where a table actually landed is reported rather than assumed. MEM_TOP_DOWN
// is a hint: it can be ignored, and a process without the large-address flag
// has no top half to be placed in. A table that came back below the boundary
// is counted separately from one that was never allocated at all, because a
// caller refusing to install is a different thing from a caller that installed
// and gained nothing.
// ============================================================================

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <cstdint>
#include <cstring>

#include "high_tables.h"

extern "C" void Log(const char* fmt, ...);

namespace HighTables {

namespace {

// The line between the half the client allocates from and the half it does not.
constexpr uintptr_t kHalf = 0x80000000u;

constexpr int kMaxTables = 32;

struct Table {
    const char* name;
    void*       base;
    size_t      bytes;
};

Table g_table[kMaxTables];
int   g_count   = 0;
int   g_refused = 0;      // VirtualAlloc failed both ways
size_t g_refusedBytes = 0;

}  // namespace

void* Reserve(const char* name, size_t bytes) {
    if (bytes == 0) return nullptr;

    void* p = VirtualAlloc(nullptr, bytes, MEM_COMMIT | MEM_RESERVE | MEM_TOP_DOWN,
                           PAGE_READWRITE);
    if (!p) {
        // A table in the wrong half still works, and the caller wanting one is
        // a stronger claim than this module's preference about where it sits.
        p = VirtualAlloc(nullptr, bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    }
    if (!p) {
        ++g_refused;
        g_refusedBytes += bytes;
        Log("[HighTables] %s: %u KB could not be allocated at all, so whoever "
            "asked for it has to stand down.",
            name, (unsigned)(bytes / 1024));
        return nullptr;
    }

    if (g_count < kMaxTables) {
        Table& t = g_table[g_count++];
        t.name = name;
        t.base = p;
        t.bytes = bytes;
    }
    return p;
}

void LogStats() {
    if (g_count == 0 && g_refused == 0) {
        Log("[HighTables] no table asked to be moved. Everything static in this "
            "DLL is still in the half the client allocates from.");
        return;
    }

    size_t high = 0, low = 0;
    int highN = 0, lowN = 0;
    for (int i = 0; i < g_count; ++i) {
        if ((uintptr_t)g_table[i].base >= kHalf) { high += g_table[i].bytes; ++highN; }
        else                                     { low  += g_table[i].bytes; ++lowN; }
    }

    Log("[HighTables] %d table(s), %u KB, moved out of this DLL's image. %d of "
        "them (%u KB) landed above 2GB, which is %u KB returned to the half the "
        "client allocates from.",
        g_count, (unsigned)((high + low) / 1024), highN, (unsigned)(high / 1024),
        (unsigned)(high / 1024));

    if (lowN > 0) {
        Log("[HighTables]   %d of them (%u KB) came back below 2GB anyway. "
            "MEM_TOP_DOWN is a hint, and a process without the large address "
            "flag has no other half to be placed in; those tables work, they "
            "just did not move.", lowN, (unsigned)(low / 1024));
        for (int i = 0; i < g_count; ++i)
            if ((uintptr_t)g_table[i].base < kHalf)
                Log("[HighTables]     %-20s %6u KB at 0x%08X",
                    g_table[i].name, (unsigned)(g_table[i].bytes / 1024),
                    (unsigned)(uintptr_t)g_table[i].base);
    }
    if (g_refused > 0)
        Log("[Wrong] [HighTables] %d table(s), %u KB, could not be allocated at "
            "all. The features that own them are switched off in this session.",
            g_refused, (unsigned)(g_refusedBytes / 1024));
}

}  // namespace HighTables
