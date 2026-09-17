#include <windows.h>
#include <intrin.h>
#include <cstdint>
#include "MinHook.h"
#include "version.h"
#include "crash_dumper.h"
#include "lua_optimize.h"

extern "C" void Log(const char* fmt, ...);

#define CACHE_SIZE  1024
#define CACHE_MASK  (CACHE_SIZE - 1)
#define TAINT_CELL ( *(uint32_t**)0x00D4139C )

struct Entry { uintptr_t closure; int nups; uint64_t hits; };
static Entry g_cache[CACHE_SIZE];
static volatile long g_hits = 0, g_misses = 0, g_inline = 0;
static ULONGLONG g_startTime = 0;

typedef int(__cdecl *precall_t)(uintptr_t,uintptr_t,int,uint64_t,uint64_t*);
static precall_t orig = nullptr;

void PrecallCache_Invalidate() { memset(g_cache, 0, sizeof(g_cache)); }

static bool CanInline() {
    if (g_startTime == 0) g_startTime = GetTickCount64();
    return !LuaOpt::IsLoadingMode() && (GetTickCount64() - g_startTime) > 30000;
}

static int __cdecl hook(uintptr_t L, uintptr_t tv, int nres, uint64_t tin, uint64_t* tout)
{
    uintptr_t obj = *(uintptr_t*)(tv + 0);
    if (*(int*)(tv + 8) != 6 || obj < 0x10000) return orig(L,tv,nres,tin,tout);
    if (*(unsigned char*)(obj + 10) & 1) return orig(L,tv,nres,tin,tout);

    uint32_t s = ((uint32_t)(obj >> 4) * 2654435761u) & CACHE_MASK;

    if (g_cache[s].closure == obj) {
        g_hits++;
        g_cache[s].hits++;
        int nups = g_cache[s].nups;

        if (CanInline()) {
            uintptr_t proto = *(uintptr_t*)(obj + 24);
            if (*(unsigned char*)(proto + 78) & 1) goto fallback2;
            
            uintptr_t base = tv + 16;
            
            // Truncate L->top if caller passed more arguments than numparams
            uintptr_t expected_top = base + 16 * *(unsigned char*)(proto + 77);
            uintptr_t orig_top = *(uintptr_t*)(L + 0x0C);
            if (orig_top > expected_top) {
                orig_top = expected_top;
                *(uintptr_t*)(L + 0x0C) = orig_top;
            }
            
            uintptr_t newtop = base + 16 * nups;
            if (*(uintptr_t*)(L + 0x20) <= newtop) goto fallback2;
            
            for (uintptr_t p = orig_top; p < newtop; p += 16) {
                *(uint32_t*)(p + 8) = 0;
                *(uint32_t*)(p + 12) = *(uint32_t*)0x00D4139C;
            }
            
            *(uintptr_t*)(L + 0x0C) = newtop;
            *(uintptr_t*)(L + 0x10) = base;  // CRITICAL: Update L->base
            
            uintptr_t ci = *(uintptr_t*)(L + 0x18);
            
            // CRITICAL: Save L->savedpc to caller's ci->savedpc before pushing new ci!
            // If we don't do this, luaD_poscall restores garbage, causing infinite loops/freezes.
            *(uintptr_t*)(ci + 12) = *(uintptr_t*)(L + 0x1C);
            
            if (ci == *(uintptr_t*)(L + 0x28)) goto fallback2;
            ci += 24;
            
            *(uintptr_t*)(L + 0x18) = ci;
            *(uintptr_t*)(ci + 0) = base;   // ci->base
            *(uintptr_t*)(ci + 4) = tv;     // ci->func
            *(uintptr_t*)(ci + 8) = newtop; // ci->top
            *(uintptr_t*)(ci + 12) = 0;     // ci->savedpc
            *(uint32_t*)(ci + 16) = nres;   // ci->nresults
            *(uint32_t*)(ci + 20) = 0;      // ci->tailcalls
            
            *(uintptr_t*)(L + 0x1C) = *(uintptr_t*)(proto + 16);
            g_inline++;
            return 0;
        }
    } else {
        g_misses++;
        g_cache[s].closure = obj;
        g_cache[s].nups = *(unsigned char*)(*(uintptr_t*)(obj + 24) + 79);
        g_cache[s].hits = 1;
    }

fallback2:
    return orig(L,tv,nres,tin,tout);
}

bool InstallLuaPrecallCache() {
    Log("[PrecallCache] DISABLED — high risk stack/profiler modifications");
    return false;
}
