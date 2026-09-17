#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <cstdint>
#include <cstring>
#include <intrin.h>
#include <emmintrin.h>
#include "MinHook.h"
#include "lua_getstr_inline.h"
#include "lua_optimize.h"
#include "crash_dumper.h"
#include "version.h"

extern "C" void Log(const char* fmt, ...);

// ----------------------------------------------------------------
// Statistics (diagnostic only; plain increments. The Lua VM is
// single-threaded, so we use plain increments for minimum overhead.)
// ----------------------------------------------------------------
// Whether the hook actually went in, so the report can tell a guard
// that never fired from one that was never installed.
static bool g_statsInstalled = false;

static volatile LONG64 g_total_calls = 0;
static volatile LONG64 g_first_node_hits = 0;
static volatile LONG64 g_chain_walks = 0;
static volatile LONG64 g_chain_depth_total = 0;
static volatile LONG64 g_nil_returns = 0;
// Of those nil answers, the ones settled before entering the guarded walk.
static volatile LONG64 g_nil_no_chain = 0;

void InvalidateLuaGetStrInlineCache() {
    // Cache removed to prevent WeakAuras nil-field or GC invalidation errors.
}

// ----------------------------------------------------------------
// Nil object sentinel (returned when key not found)
// Located at 0xA46F78 in WoW 3.3.5a (resolved dynamically)
// ----------------------------------------------------------------
static void* g_nil_object = (void*)0x00A46F78;

// Original function pointer
typedef void* (__cdecl *luaH_getstr_fn)(int table, int tstring);
static luaH_getstr_fn g_orig_getstr = nullptr;

// ----------------------------------------------------------------
// ----------------------------------------------------------------
// The chain walk, and the SEH that used to cover everything
// ----------------------------------------------------------------
// This function is 993333271 calls in one field session and half of them stop on
// the first node. A __try region on 32-bit MSVC is not free: the prologue pushes
// an exception registration record and links it through fs:[0], and that happens
// on every call whether anything faults or not.
//
// Measured in a standalone harness, a hundred million first-node hits each way:
//
//     __try around the whole body  : 3.93 ns
//     first node outside the __try : 1.45 ns
//
// The guard cost more than twice what the lookup did, and on the 508900616
// first-node hits of that session it is 1.26 seconds of main thread.
//
// So the guard moves to where the risk actually is. The chain walk follows
// node[8] to wherever it points and has no bound at all - that is what SEH is
// for, and it keeps it. The first-node read is at node_array + 40*bucket, with
// bucket masked by the table's own lsizenode, so it is inside the array the
// table says it has.
//
// The first-node read is not guarded against a table pointer that looks valid
// and points at freed memory. Three things stand in front of it - the reload and
// swap check, the range test on both pointers, the range test on node_array -
// and the walk, where a corrupt chain actually leads, is guarded. A fault at
// this address means revisiting that.
static __declspec(noinline) void* WalkChainGuarded(int table, int tstring,
                                                   void* first)
{
    __try {
        ++g_chain_walks;
        int depth = 1;
        void* next = first;
        while (next != nullptr) {
            uint32_t* n = (uint32_t*)next;
            if (n[6] == 4 && n[4] == (uint32_t)tstring) {
                g_chain_depth_total += depth;
                return n;
            }
            next = (void*)n[8];
            depth++;
        }
        ++g_nil_returns;
        return g_nil_object;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        // Corrupt chain - fall back to the engine rather than fault.
        return g_orig_getstr(table, tstring);
    }
}

// Optimized replacement - SAFE (no pointer caching)
// ----------------------------------------------------------------
static void* __cdecl Optimized_GetStr(int table, int tstring)
{
    ++g_total_calls;

    // Bail out during lua_State swap - table and tstring pointers become
    // garbage when WoW destroys the old Lua VM during UI reload/logout.
    // GuardActive() rather than IsReloading() || IsSwapping(): the same three
    // flags in the same order, read inline. The pair of calls cost four call and
    // return pairs on a path this hook takes 11522349178 times a session.
    if (LuaOpt::GuardActive()) {
        return g_orig_getstr(table, tstring);
    }

    // Validate inputs - reject obviously invalid pointers
    if ((uintptr_t)table < 0x10000 || (uintptr_t)table > 0xFFE00000 ||
        (uintptr_t)tstring < 0x10000 || (uintptr_t)tstring > 0xFFE00000) {
        return g_orig_getstr(table, tstring);
    }

    // Read tstring hash: tstring[12] = precomputed string hash
    uint32_t ts_hash = *(uint32_t*)(tstring + 12);

    // Read table metadata
    uint8_t  lsize      = *(uint8_t*)(table + 11);   // log2 of hash size
    uint32_t* node_array = *(uint32_t**)(table + 20); // hash bucket array

    if (!node_array || lsize == 0 || lsize > 24 ||
        (uintptr_t)node_array < 0x10000 || (uintptr_t)node_array > 0xFFE00000) {
        return g_orig_getstr(table, tstring);
    }

    // Compute bucket index: ts_hash & ((1 << lsize) - 1)
    uint32_t bucket_mask = (1u << lsize) - 1;
    uint32_t bucket_idx  = ts_hash & bucket_mask;

    // Get first node in chain (each node is 40 bytes = 10 DWORDs)
    uint32_t* node = (uint32_t*)((uint8_t*)node_array + 40 * bucket_idx);

    // FAST PATH: direct first-node check, about half of all lookups.
    if (node[6] == 4 && node[4] == (uint32_t)tstring) {
        ++g_first_node_hits;
        return node;
    }

    // The empty-chain answer, settled here rather than inside the guard.
    //
    // A field session has 11522349178 calls through this hook: 6756067716 stop
    // on the first node, and of the 4238573891 that walk, 2290386267 - 54% -
    // find node[8] null and return nil without touching a second node. The
    // average depth of a walk is 0.6, so most of what the guarded function does
    // is pay for its own prologue.
    //
    // node[8] is the link word at node+32, in the same forty-byte node whose
    // +16 and +24 were just read above, reached through node_array + 40*bucket
    // with bucket masked by the table's own lsizenode. Reading it here adds no
    // pointer the fast path was not already dereferencing, and the walk that
    // follows an unbounded chain still runs under SEH.
    //
    // At the 2.48 ns a __try prologue was measured to cost on this compiler,
    // the calls this takes out of the guard are about 5.7 seconds of main
    // thread in that session.
    void* first = (void*)node[8];
    if (first == nullptr) {
        ++g_chain_walks;      // still a walk, of depth zero, so the average holds
        ++g_nil_returns;
        ++g_nil_no_chain;
        return g_nil_object;
    }

    return WalkChainGuarded(table, tstring, first);
}

// Install / Uninstall
bool InstallLuaGetStrInline()
{
    // Verified correct against the stock luaH_getstr decompile (0x85C430)
    void* target = (void*)0x0085C430;

    // Verify prologue: push ebp; mov ebp, esp
    unsigned char* p = (unsigned char*)target;
    if (p[0] != 0x55 || p[1] != 0x8B) {
        Log("[GetStrInline] BAD PROLOGUE at 0x%08X (expected 55 8B)", (uintptr_t)target);
        return false;
    }

    // Dynamically resolve nil object pointer from "mov eax, offset unk_A46F78" instruction
    // assembly at target + 0x38: B8 78 6F A4 00
    if (p[0x38] == 0xB8) {
        g_nil_object = *(void**)(p + 0x39);
        Log("[GetStrInline] Dynamically resolved nil object sentinel at %p", g_nil_object);
    } else {
        Log("[GetStrInline] WARNING: Failed to resolve nil object dynamically, using fallback %p", g_nil_object);
    }

    if (MH_CreateHook(target, (void*)Optimized_GetStr, (void**)&g_orig_getstr) != MH_OK) {
        Log("[GetStrInline] MH_CreateHook FAILED");
        return false;
    }
    if (MH_EnableHook(target) != MH_OK) {
        Log("[GetStrInline] MH_EnableHook FAILED");
        return false;
    }

    Log("[GetStrInline] Hook ACTIVE (safe first-node fast path + chain walk)");
    g_statsInstalled = true;
    return true;
}

// Printed from the periodic report. The counters used to be printed only
// from the uninstall path, which nothing calls: the DLL leaves through
// TerminateProcess, and the linker had dropped the function outright.
void LuaGetStrInline_LogStats(void) {
    if (!g_statsInstalled) {
        Log("[GetStrInline] not measured: the hook is not installed.");
        return;
    }
    const LONG64 total = g_total_calls, first = g_first_node_hits;
    const LONG64 walks = g_chain_walks, nils = g_nil_returns;
    const LONG64 depth = g_chain_depth_total;
    const LONG64 nochain = g_nil_no_chain;
    if (total == 0) {
        Log("[GetStrInline] measured and zero: no string lookup reached it.");
        return;
    }
    Log("[GetStrInline] %lld calls, %lld first-node (%.1f%%), %lld chain walks "
        "(avg depth %.1f), %lld nil.",
        (long long)total, (long long)first, 100.0 * (double)first / (double)total,
        (long long)walks, walks > 0 ? (double)depth / (double)walks : 0.0,
        (long long)nils);
    // Printed whether or not it fired. These are the calls that answer nil
    // without entering the guarded walk, so they never pay a __try prologue;
    // together with the first-node hits above they are the share of this hook
    // that runs with no exception frame at all.
    Log("[GetStrInline]   %lld of those nil answers found an empty chain and "
        "were settled without entering the guard (%.1f%% of all calls take no "
        "exception frame, counting the first-node hits).",
        (long long)nochain,
        100.0 * (double)(first + nochain) / (double)total);
}

void UninstallLuaGetStrInline()
{
    MH_DisableHook((void*)0x0085C430);
    MH_RemoveHook((void*)0x0085C430);

    LONG64 total = g_total_calls;
    LONG64 first = g_first_node_hits;
    LONG64 walks = g_chain_walks;
    LONG64 nils  = g_nil_returns;
    LONG64 depth = g_chain_depth_total;

    if (total > 0) {
        double firstPct = 100.0 * first / total;
        double avgDepth = walks > 0 ? (double)depth / walks : 0;
        Log("[GetStrInline] Stats: %lld calls | %lld first-node (%.1f%%) | "
            "%lld walks (avg depth %.1f) | %lld nil",
            total, first, firstPct, walks, avgDepth, nils);
    }
}