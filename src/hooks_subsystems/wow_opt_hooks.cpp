// ============================================================================
// Module: wow_opt_hooks.cpp
// Description: Installs and manages target intercepts for subsystem `wow_opt_hooks.cpp`.
// Safety & Threading: Stack layouts and register conventions must match target function definitions exactly.
// ============================================================================

#include "wow_opt_hooks.h"
#include "MinHook.h"
#include "version.h"
#include <mimalloc.h>
#include <cstdint>
#include <cstring>
#include <intrin.h>
#include <emmintrin.h>

extern "C" void Log(const char* fmt, ...);

// ================================================================
// 20 Real WoW.exe Optimization Hooks
// Each hook directly intercepts a hot wow.exe function and optimizes it.
// ================================================================

// Plain, not Interlocked, for the same reason as in wow_perf_hooks.cpp: a
// locked read-modify-write per call on every hooked client function, for
// statistics that nothing reads for control flow.
static long g_w1Hits = 0, g_w1Calls = 0;
static long g_w2Hits = 0, g_w2Calls = 0;
static long g_w8Fast = 0, g_w8Calls = 0;
static long g_w13Deduped = 0, g_w13Calls = 0;

// ================================================================
// W1: sub_4CFBB0 - memcpy wrapper with prefetch (called by sub_4CFD20)
// Original does conditional memcpy(680). We add prefetch before copy.
// ================================================================
typedef void (__cdecl *Memcpy680_fn)(const void*, size_t, void*);
static Memcpy680_fn orig_Memcpy680 = nullptr;

static void __cdecl Hooked_Memcpy680(const void* src, size_t len, void* dst) {
    ++g_w1Calls;
    if (src && dst && len == 680) {
        _mm_prefetch((const char*)src, _MM_HINT_T0);
        _mm_prefetch((const char*)src + 64, _MM_HINT_T0);
        ++g_w1Hits;
    }
    orig_Memcpy680(src, len, dst);
}

// ================================================================
// W2: sub_422910 - Object cleanup/destroy (called everywhere)
// Add prefetch of next object in cleanup chain before destroy.
// In WoW 3.3.5a, this is SFileCloseFile which is __stdcall.
// ================================================================
typedef void (__stdcall *ObjDestroy_fn)(void*);
static ObjDestroy_fn orig_ObjDestroy = nullptr;

volatile void* g_w5LastPtr = nullptr;

static void __stdcall Hooked_ObjDestroy(void* obj) {
    ++g_w2Calls;
    if (obj) {
        // Invalidate a recycled file handle in the W5 cache. There used to be a
        // second check here against a W4 pointer that nothing ever assigned, so
        // it compared against a permanent null that the `if (obj)` above had
        // already excluded.
        if (obj == g_w5LastPtr) {
            g_w5LastPtr = nullptr;
        }
        __try {
            // Prefetch the object's vtable and first cache line
            _mm_prefetch((const char*)obj, _MM_HINT_NTA);
            ++g_w2Hits;
        } __except(EXCEPTION_EXECUTE_HANDLER) {}
    }
    orig_ObjDestroy(obj);
}

// ================================================================
// W3: sub_771870 - Error/assert handler (called on every error path)
// In production builds, skip non-critical error formatting.
// sub_771870 is __stdcall.
// ================================================================


// ================================================================
// W4: sub_424B50 - File read dispatcher (21 callers, texture/model loads)
// Cache last successful file read result to avoid re-reading same data.
// ================================================================

// W4 is gone. It hooked 0x00424B50 as FileReadDispatch(src, dst, a3, Block) and
// did nothing but count: the caching idea behind it was abandoned as unsafe and
// the comment saying so stayed for the life of the hook.
//
// The name was wrong as well. sub_424B50 is int __stdcall(void*, char* String1,
// int, void* Block) - the second argument is a filename, and the function's own
// assert strings are "SFile" and ".\\SFile2-Core.cpp". It is SFileOpenFileEx,
// an archive open, not a file read, so what W4 counted was never what it said.
//
// It also cost something. Two tester sessions report 0x00424B50 hooked by two of
// our own modules with only the first to install running, and the other one is
// the loading-screen census that measures where forty-five seconds of a loading
// screen go. A counter under a wrong name was standing in front of it.

// ================================================================
// W5: sub_4218C0 - Data size calculator (21 callers)
// Caches result for repeated size queries on same data pointer.
// ================================================================
volatile int g_w5LastSize = 0;


// ================================================================
// W6: sub_422530 - Memory block copy (texture/model data transfer)
// Add SSE2 prefetch for large copies (>256 bytes).
// ================================================================
typedef void (__cdecl *BlockCopy_fn)(void*, void*, int, void*, int, int);
static BlockCopy_fn orig_BlockCopy = nullptr;

// W6 (block copy prefetch) was defined here and never appeared in the install
// table, so it has never run. Removed with the ten passthroughs rather than
// left to look like a feature.

// ================================================================
// W7 (REMOVED): sub_4C6A40, the sound play dispatcher.
//
// It used to drop a play whose sound id matched the previous one inside a 16 ms
// window, keeping a single global "last id" slot. A tester lost every boss voice
// line in raids and dungeons while every other sound kept working, and this is
// why.
//
// Three things were wrong with it, all visible in the target's own disassembly.
//
// sub_4C6A40 is PlaySoundKit out of SoundInterface2.cpp: a1 is a SoundEntries
// row id and the return value is an error code, where 0 means the sound started
// and 5, 6, 8..11, 14..17 and 20 each name a reason it did not. Coalescing
// returned 0 - success - for a sound that was never handed to the mixer, so
// nothing upstream could notice, retry or report it.
//
// The engine already suppresses duplicates, and it does so with information we
// do not have: sounds whose flags carry 0x20 get registered in the list at
// dword_B4A398 when they start, and a second play of one that is still in that
// list returns 15 near the top of the function. Sounds without that flag are
// meant to overlap. Our version applied one blanket rule to all of them.
//
// The window could not have worked either. GetTickCount advances in ~15.6 ms
// steps, so "less than 16 ms apart" was really "zero or one ticks apart" -
// somewhere between an instant and a frame, depending on where the plays landed
// against a timer we do not control. With the TimingFix switch on, GetTickCount
// was itself redirected to QPC, and the same code silently used a different
// window.
//
// Deleted rather than fixed. The engine's own rule is better than any rule we
// can reconstruct from outside it, and it is already running.
// ================================================================

// ================================================================
// W8: sub_4B9DE0 - Async file read destroy (17 xrefs, 16 callers)
// Fast-path null checks before expensive cleanup.
// ================================================================
typedef int (__cdecl *AsyncReadDestroy_fn)(int);
static AsyncReadDestroy_fn orig_AsyncReadDestroy = nullptr;

static int __cdecl Hooked_AsyncReadDestroy(int handle) {
    ++g_w8Calls;
    // Fast null check - avoid entering cleanup for empty handles
    if (!handle) {
        ++g_w8Fast;
        return 0;
    }
    return orig_AsyncReadDestroy(handle);
}

// ================================================================
// W9: sub_4B4F90 - SysMessage handler (9 callers)
// Skip redundant system message processing during loading.
// ================================================================
static volatile DWORD g_w9LastMsgTick = 0;
static volatile int g_w9LastMsg = 0;


// ================================================================
// W10: sub_513660 - Tooltip/item context getter (12 callers)
// Cache the global context pointer to avoid repeated reads.
// ================================================================
static volatile __int64 g_w10CachedContext = 0;
static volatile DWORD g_w10CacheTick = 0;


// ================================================================
// W11: sub_61E3A0 - Item name resolver (12 callers)
// Cache last resolved item name to avoid re-resolution on hover flicker.
// ================================================================
static volatile int g_w11LastItem = 0;
static volatile int g_w11LastName = 0;


// ================================================================
// W12: sub_47C240 - Memory allocator wrapper (called by SysMessage)
// Batch small allocations to reduce allocator overhead.
// ================================================================


// ================================================================
// W13: sub_47C0F0 - Buffer validity check (called before every buffer op)
// Inline the common case: buffer is valid (non-null, flag set).
// ================================================================
typedef int (__thiscall *BufferValid_fn)(void*);
static BufferValid_fn orig_BufferValid = nullptr;

static int __fastcall Hooked_BufferValid(void* This, void* unused) {
    ++g_w13Calls;
    if (This) {
        __try {
            int flags = *(int*)((char*)This + 12);
            // Common case: flags has bit 0 clear AND flags != 0 → valid
            if ((flags & 1) == 0 && flags != 0) {
                ++g_w13Deduped;
                return 1; // Valid
            }
        } __except(EXCEPTION_EXECUTE_HANDLER) {}
    }
    return orig_BufferValid(This);
}

// ================================================================
// W14: sub_4CFD20 already hooked in hot_patch N1.
// W14: sub_878760 - Sound volume lookup (called by sub_4C6A40)
// Cache volume values to avoid repeated CVAR lookups.
// ================================================================
static volatile float g_w14Cache[16] = {};
static volatile uintptr_t g_w14Keys[16] = {};


// ================================================================
// W15: sub_8799E0 - Sound channel deallocator (called by sub_4C6A40)
// SKIPPED/DISABLED: This is a deallocator; skipping it causes channel leaks.
// ================================================================

// ================================================================
// W16: sub_878610 - Sound mix/update (called every frame by sound system)
// Skip mix update when no sounds are playing.
// ================================================================
static volatile float g_w16Cache[16] = {};
static volatile uintptr_t g_w16Keys[16] = {};


// ================================================================
// W17: sub_879390 - Sound stop/fadeout (called on zone transitions)
// Batch stop calls to reduce per-sound overhead.
// ================================================================
typedef int (__fastcall *SoundStop_fn)(void*, void*, float, float);
static SoundStop_fn orig_SoundStop = nullptr;


// ================================================================
// W18: sub_87F7A0 - Ambient sound manager (called every frame)
// Skip ambient update when player is indoors/loading.
// ================================================================
static volatile DWORD g_w18LastAmbientTick = 0;


// ================================================================
// W19: sub_4CB580 - Music track selector (called by sub_4C6A40)
// Cache selected music track to avoid re-selection on volume changes.
// sub_4CB580 is __cdecl.
// ================================================================
static volatile int g_w19LastSelf = 0;
static volatile int g_w19LastZone = 0;
static volatile int g_w19LastTrack = 0;


// ================================================================
// W20: sub_4C5990 - Sound effect priority calculator (called by sub_4C6A40)
// DISABLED: 0x4C5990 is a camera/audio properties constructor with a signature of
// BOOL __thiscall sub_4C5990(float *this), not an SFX priority calculator.
// Hooking it as a cdecl function causes registers (ECX) to be clobbered.
// ================================================================

// ================================================================
// Installation / Shutdown / Stats
// ================================================================
namespace WowOptHooks {
    // How many of the four went in. A report of zeroes from a hook that never
    // installed reads exactly like one from a hook nothing called.
    static int g_installedHooks = 0;

    bool InstallAll() {
        int installed = 0;

        struct HookDef {
            void* addr; void* hook; void** orig; const char* name;
        };

        HookDef hooks[] = {
            {(void*)0x004CFBB0, (void*)Hooked_Memcpy680,      (void**)&orig_Memcpy680,      "W1 memcpy680 prefetch"},
            {(void*)0x00422910, (void*)Hooked_ObjDestroy,      (void**)&orig_ObjDestroy,      "W2 obj destroy prefetch"},
            {(void*)0x004B9DE0, (void*)Hooked_AsyncReadDestroy,(void**)&orig_AsyncReadDestroy,"W8 async destroy fast"},
            {(void*)0x0047C0F0, (void*)Hooked_BufferValid,     (void**)&orig_BufferValid,     "W13 buffer valid inline"},
            // W15 skipped - channel deallocator function (skipping it causes channel leak)
            // W20 skipped - 0x004C5990 is camera constructor, not SFX priority
        };

        for (auto& h : hooks) {
            if (WineSafe_CreateHook(h.addr, h.hook, h.orig) == MH_OK) {
                if (MH_EnableHook(h.addr) == MH_OK) {
                    Log("[WowOpt] %s: ACTIVE @ 0x%08X", h.name, (uintptr_t)h.addr);
                    installed++;
                }
            }
        }

        // Against the table, not against a number that stopped being true.
        // It read "/20" while the table held fourteen, and now holds four.
        Log("[WowOpt] %d/%d WoW.exe optimization hooks installed, and every one "
            "of them does work - ten that only counted and called the original "
            "were removed.",
            installed, (int)(sizeof(hooks) / sizeof(hooks[0])));
        g_installedHooks = installed;
        return installed > 0;
    }

    void ShutdownAll() {
        DumpStats();
    }

    void DumpStats() {
        // Four hooks, and only the four that do something.
        //
        // Ten more used to be installed here and every one of them had been
        // reduced to a counter and a call to the original after its
        // optimisation was found unsafe - the reasons are kept above, where
        // each function used to be. They still patched five bytes of client
        // code, still paid a detour on every call, and still reported counters
        // whose names described work that no longer happened: g_w9Skipped
        // counted successful calls rather than skips, g_w14Fast counted
        // successes rather than fast paths, g_w19Cached counted successes
        // rather than cache hits. A reader of this report saw a high skip rate
        // for a hook that skipped nothing.
        if (g_installedHooks == 0) {
            Log("[WowOpt] not measured: none of the four hooks on wow.exe went in. "
                "The counters below would all read zero because nothing ran, which "
                "is not the same as nothing happening.");
            return;
        }
        Log("[WowOpt] hits/calls below are plain counters on %d hooked client "
            "function(s) and are lower bounds.", g_installedHooks);
        Log("[WowOpt] Memcpy680: %d/%d | ObjDestroy: %d/%d | AsyncDest: %d/%d "
            "| BufValid: %d/%d",
            g_w1Hits, g_w1Calls, g_w2Hits, g_w2Calls,
            g_w8Fast, g_w8Calls, g_w13Deduped, g_w13Calls);
    }
}