#include <windows.h>
#include <cstdint>
#include "MinHook.h"
#include "version.h"
#include "crash_dumper.h"

extern "C" void Log(const char* fmt, ...);

#define TAINT_CELL ( *(uint32_t**)0x00D4139C )
#define TAINT_FLAG 0x00D413A0

// luaL_ref at 0x84F6C0 — store value in table[t], return reference key
// Fast path: inline the common "add reference" case (freelist empty, objlen+1)
// Falls back to original for free (nil value) and freelist-nonzero paths.
typedef int(__cdecl *ref_fn)(uintptr_t L, int t);
static ref_fn orig = nullptr;
static volatile long g_hits = 0, g_misses = 0;

static int __cdecl hook(uintptr_t L, int t) {
    if (L < 0x10000 || L > 0xFFE00000) { g_misses++; return orig(L, t); }

    __try {
        uintptr_t top = *(uintptr_t*)(L + 0x0C);
        // If the value to reference is nil, the engine returns LUA_REFNIL — defer.
        uint32_t topTT = *(uint32_t*)(top - 8);
        if (!topTT) { g_misses++; return orig(L, t); }

        // Read the freelist head t[0]. rawgeti pushes it; we leave the stack as we
        // found it before either fast-pathing or deferring.
        typedef int(__cdecl *rawgeti_fn)(uintptr_t, int, int);
        ((rawgeti_fn)0x0084E670)(L, t, 0);
        uintptr_t newTop = *(uintptr_t*)(L + 0x0C);
        uint32_t tt2 = *(uint32_t*)(newTop - 8);
        int head = (tt2 == 3) ? (int)*(double*)(newTop - 16) : 0;
        *(uintptr_t*)(L + 0x0C) = newTop - 16;   // pop the freelist head

        // Non-empty freelist requires unlinking t[0] = t[head] — defer that to the
        // engine. With the head popped, the stack is back to its original shape, so
        // the original runs correctly. Only the "fresh ref" (objlen+1) path is
        // inlined here.
        if (head != 0) { g_misses++; return orig(L, t); }

        typedef int(__cdecl *objlen_fn)(uintptr_t, int);
        int ref = ((objlen_fn)0x0084E150)(L, t) + 1;

        // Engine clears taint around the registry write so referencing never taints.
        int32_t savedTaintFlag = *(int32_t*)TAINT_FLAG;
        int32_t savedTaintCell = *(int32_t*)0x00D4139C;
        *(int32_t*)TAINT_FLAG = 0;
        *(int32_t*)0x00D4139C = 0;

        typedef int(__cdecl *rawseti_fn)(uintptr_t, int, int);
        ((rawseti_fn)0x0084EA00)(L, t, ref);     // t[ref] = value (pops it)

        *(int32_t*)0x00D4139C = savedTaintCell;
        *(int32_t*)TAINT_FLAG = savedTaintFlag;

        g_hits++;
        return ref;
    } __except(EXCEPTION_EXECUTE_HANDLER) {}

    g_misses++;
    return orig(L, t);
}

bool InstallLuaRefFast() {
    void* t = (void*)0x0084F6C0;
    if (MH_CreateHook(t, hook, (void**)&orig) != MH_OK) return false;
    WO_EnableHook(t);
    Log("[LRef] ACTIVE — luaL_ref inline at 0x84F6C0");
    CrashDumper::RegisterFeature("LRef");
    CrashDumper::FeatureSetActive("LRef", true);
    return true;
}
