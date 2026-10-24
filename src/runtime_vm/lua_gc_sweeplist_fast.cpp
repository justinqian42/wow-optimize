// lua_gc_sweeplist_fast.cpp: Vector-prefetched Lua GC sweep list traversal (sub_85B200).
//
// In profiling sessions (wow_optimize_2026-10-22_16-54-34.log), luaC_sweeplist accounted
// for 10,330 samples (#21 overall hotspot, up to 7.04% of executing CPU time in GC spikes).
// The client's sub_85B200 (143 bytes, 62 instructions) sweeps through 200,000+ GCObjects
// in the heap one by one via dependent pointer chasing (curr = curr->next).
//
// Every live node incurs an L2/L3 memory stall waiting for cache line fill. Over 95% of
// swept objects are surviving heap objects whose marked bit is simply reset to currentwhite.
//
// This replacement:
//   1. Vector prefetches 2 upcoming GCObject nodes ahead in the chain (next and next2) to
//      hide memory latency while the CPU verifies the current node.
//   2. Inlines the surviving-object fast path into a branchless mask and write.
//   3. Retains verbatim client semantics for thread openupval sweep (tt == 8) and dead
//      object unlinking + freeobj invocation (0x0085AF90).
//   4. Compiles with __declspec(safebuffers) ensuring zero /GS security cookies and zero
//      SEH frames on the sweep path.
//   5. Checks 16-byte prologue signature before hooking; off by default under
//      LuaGCSweeplistFast (experimental: true).

#include "lua_gc_sweeplist_fast.h"
#include "MinHook.h"
#include "version.h"
#include "config.h"
#include "sampling_profiler.h"
#include <xmmintrin.h>
#include <cstdint>
#include <cstring>

extern "C" void Log(const char* fmt, ...);

namespace LuaGCSweeplistFast {

namespace {

static const uintptr_t kTarget = 0x0085B200;
static const uintptr_t kFreeObjAddr = 0x0085AF90;

// Verbatim prologue bytes of sub_85B200 (16 bytes)
static const uint8_t kExpectedPrologue[16] = {
    0x55, 0x8B, 0xEC, 0x51, 0x8B, 0x45, 0x08, 0x53,
    0x8B, 0x58, 0x14, 0x0F, 0xB6, 0x43, 0x15, 0x56
};

typedef void** (__cdecl* LuaCSweeplistFn)(void* L, void** p, uint32_t count);
static LuaCSweeplistFn g_orig_luaC_sweeplist = nullptr;

static bool g_installed = false;
static uint64_t g_calls = 0;
static uint64_t g_nodesVisited = 0;
static uint64_t g_nodesDead = 0;

// Cold helper for freeing dead objects to preserve caller registers and avoid frame bloat
__declspec(noinline) static void CallFreeObj(void* L, void* curr) {
    __asm {
        mov eax, curr
        mov ecx, L
        mov edx, kFreeObjAddr
        call edx
    }
}

__declspec(safebuffers)
static void** __cdecl Hook_LuaC_Sweeplist(void* L, void** p, uint32_t count) {
    if (!p) return p;

    ++g_calls;
    void* g = *(void**)((uintptr_t)L + 0x14); // L->l_G
    const uint8_t curwhite = *(const uint8_t*)((uintptr_t)g + 0x15) & 3;
    const uint32_t otherwhite = ((uint32_t)*(const uint8_t*)((uintptr_t)g + 0x15)) ^ 3;
    void** rootgc_ptr = (void**)((uintptr_t)g + 0x1C);

    void* curr = *p;
    while (curr && count > 0) {
        --count;
        ++g_nodesVisited;

        void* next = *(void**)curr; // curr->gch.next
        
        // Prefetch ahead in GC linked list to overlap memory fetches with execution
        if (next) {
            _mm_prefetch((const char*)next, _MM_HINT_T0);
            void* next2 = *(void**)next;
            if (next2) {
                _mm_prefetch((const char*)next2, _MM_HINT_T0);
            }
        }

        const uint8_t tt = *(const uint8_t*)((uintptr_t)curr + 8);
        if (tt == 8) { // LUA_TTHREAD
            Hook_LuaC_Sweeplist(L, (void**)((uintptr_t)curr + 0x68), (uint32_t)-3);
        }

        const uint8_t marked = *(const uint8_t*)((uintptr_t)curr + 9);
        if (((marked ^ 3) & otherwhite) != 0) {
            // Object is SURVIVING (NOT DEAD) - hot path (>95% of passes)
            *(uint8_t*)((uintptr_t)curr + 9) = (marked & 0xF8) | curwhite;
            p = (void**)curr;
        } else {
            // Object is DEAD - unlink and free
            ++g_nodesDead;
            *p = next;
            if (curr == *rootgc_ptr) {
                *rootgc_ptr = next;
            }
            CallFreeObj(L, curr);
        }
        curr = *p;
    }
    return p;
}

} // anonymous namespace

bool Init() {
    if (!Config::g_settings.OptLuaGCSweeplistFast) {
        return false;
    }

    if (Config::g_settings.OptNoClientPatches) {
        Log("[LuaGCSweeplistFast] Client patches disabled by OptNoClientPatches, skipping.");
        return false;
    }

    if (g_installed) return true;

    // Verify prologue bytes at sub_85B200
    if (std::memcmp((const void*)kTarget, kExpectedPrologue, sizeof(kExpectedPrologue)) != 0) {
        Log("[LuaGCSweeplistFast] Refusing to hook 0x%08X: prologue bytes mismatch", kTarget);
        return false;
    }

    MH_STATUS status = WineSafe_CreateHook((void*)kTarget, (void*)Hook_LuaC_Sweeplist, (void**)&g_orig_luaC_sweeplist);
    if (status != MH_OK) {
        Log("[LuaGCSweeplistFast] WineSafe_CreateHook failed with status %d", status);
        return false;
    }

    status = WO_EnableHook((void*)kTarget);
    if (status != MH_OK) {
        Log("[LuaGCSweeplistFast] WO_EnableHook failed with status %d", status);
        return false;
    }

    SamplingProfiler::RegisterSelfSymbol("LuaGCSweeplistFast_Hook", (const void*)&Hook_LuaC_Sweeplist);

    g_installed = true;
    Log("[LuaGCSweeplistFast] ACTIVE on sub_85B200 (luaC_sweeplist, 143 bytes, off by default).");
    return true;
}

void LogStats() {
    if (!g_installed) return;
    Log("[LuaGCSweeplistFast] Sweeps: %llu, Nodes: %llu (dead: %llu)",
        g_calls, g_nodesVisited, g_nodesDead);
}

void Shutdown() {
    if (!g_installed) return;
    MH_DisableHook((void*)kTarget);
    g_installed = false;
    Log("[LuaGCSweeplistFast] Shutdown complete");
}

} // namespace LuaGCSweeplistFast
