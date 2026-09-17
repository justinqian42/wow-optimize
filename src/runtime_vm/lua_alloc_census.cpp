// ============================================================================
// Description: Counts every allocation the Lua VM makes, by size.
// Safety & Threading: Runs on whichever thread the VM runs on - one, by contract.
// ============================================================================
// Every object Lua creates - strings, tables, closures, stacks, upvalues - is
// allocated through one function pointer. luaM_realloc_ at 0x0085D6F0 reads it:
//
//     G = *(lua_State + 20);                        // l_G
//     result = (*(G + 12))(*(G + 16), ptr, osize, nsize);
//     *(G + 68) += nsize - osize;                   // totalbytes
//
// So the whole VM's memory traffic passes through G+12, and it can be observed
// by writing a pointer rather than by patching code. The state itself is the
// known global at 0x00D3F78C.
//
// The reason to look is a concrete proposal: give Lua its own arena instead of
// the general-purpose allocator, on the grounds that it makes millions of small
// objects and pays thread-safety overhead it does not need, being single
// threaded. That may be right. It also may be that Lua's allocations are a small
// fraction of the client's, or mostly large, in which case an arena is a lot of
// risk for very little.
//
// This decides it. The census is a pass-through: it counts, then calls the
// original with the same arguments and returns the same value. Nothing about
// allocation behaviour changes.
//
// It is off by default. This sits on the hottest allocation path in the VM, and
// like the draw census it exists to answer a question in one session.

#include <windows.h>
#include <cstdint>
#include "lua_alloc_census.h"
#include "config.h"

extern "C" void Log(const char* fmt, ...);

namespace LuaAllocCensus {

typedef void* (__cdecl *lua_Alloc_fn)(void* ud, void* ptr, size_t osize, size_t nsize);

static const uintptr_t ADDR_GLOBAL_LUA_STATE = 0x00D3F78C;
static const int OFF_L_G        = 20;   // lua_State -> global_State
static const int OFF_G_FREALLOC = 12;   // global_State -> frealloc
static const int OFF_G_UD       = 16;   // global_State -> ud

static lua_Alloc_fn g_orig = nullptr;
static void*        g_origUd = nullptr;
static uintptr_t    g_installedOn = 0;   // the state we patched, for reload detection

// Powers of two from 16 bytes up, then an overflow bin. Non-atomic: the VM is
// single threaded, and an interlocked increment on every Lua allocation would
// cost more than the thing it is measuring.
static constexpr int BUCKETS = 16;
static long g_allocs[BUCKETS] = {};
static long g_frees   = 0;
static long g_reallocs = 0;
static long long g_bytesOut = 0;

static inline void Note(size_t nsize) {
    int b = 0;
    size_t v = nsize >> 4;
    while (v && b < BUCKETS - 1) { v >>= 1; ++b; }
    ++g_allocs[b];
}

static void* __cdecl Hooked_LuaAlloc(void* ud, void* ptr, size_t osize, size_t nsize) {
    if (nsize == 0)            ++g_frees;
    else if (ptr)              ++g_reallocs;
    else                       Note(nsize);

    g_bytesOut += (long long)nsize - (long long)osize;

    return g_orig(ud, ptr, osize, nsize);
}

// Called from the frame boundary. Cheap when already installed: one load and a
// compare. Reinstalls after a /reload, which builds a new state with the
// client's own allocator back in place.
void EnsureInstalled() {
    if (!Config::g_settings.OptLuaAllocCensus) return;

    uintptr_t L = *(uintptr_t*)ADDR_GLOBAL_LUA_STATE;
    if (L < 0x10000 || L >= 0xFFE00000) return;
    if (L == g_installedOn) return;

    __try {
        uintptr_t G = *(uintptr_t*)(L + OFF_L_G);
        if (G < 0x10000 || G >= 0xFFE00000) return;

        lua_Alloc_fn* slot = (lua_Alloc_fn*)(G + OFF_G_FREALLOC);
        lua_Alloc_fn  cur  = *slot;
        if (!cur || cur == Hooked_LuaAlloc) { g_installedOn = L; return; }

        g_orig   = cur;
        g_origUd = *(void**)(G + OFF_G_UD);
        *slot    = Hooked_LuaAlloc;
        g_installedOn = L;

        Log("[LuaAllocCensus] Counting VM allocations (state 0x%08X, original "
            "allocator 0x%08X)", (unsigned)L, (unsigned)(uintptr_t)cur);
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        // A state torn down mid-read; the next frame will try again.
    }
}

void LogStats() {
    if (!g_orig) {
        if (Config::g_settings.OptLuaAllocCensus)
            Log("[LuaAllocCensus] enabled but never installed - no Lua state was seen");
        return;
    }

    long total = 0;
    for (int i = 0; i < BUCKETS; i++) total += g_allocs[i];

    Log("[LuaAllocCensus] %ld new objects, %ld reallocations, %ld frees, "
        "%lld KB net", total, g_reallocs, g_frees, g_bytesOut / 1024);

    if (total <= 0) return;

    Log("[LuaAllocCensus]   sizes of new objects - what an arena would have to serve:");
    for (int i = 0; i < BUCKETS; i++) {
        if (!g_allocs[i]) continue;
        unsigned lo = (i == 0) ? 0u : (16u << i);
        unsigned hi = (32u << i) - 1u;
        if (i == BUCKETS - 1)
            Log("[LuaAllocCensus]     %7u+      %8ld (%5.1f%%)", lo,
                g_allocs[i], 100.0 * g_allocs[i] / total);
        else
            Log("[LuaAllocCensus]     %7u-%-7u %8ld (%5.1f%%)", lo, hi,
                g_allocs[i], 100.0 * g_allocs[i] / total);
    }
}

} // namespace LuaAllocCensus
