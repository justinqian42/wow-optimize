#include <windows.h>
#include <cstdint>
#include "MinHook.h"
#include "version.h"
#include "crash_dumper.h"

extern "C" void Log(const char* fmt, ...);

#define TAINT_CELL ( *(uint32_t**)0x00D4139C )
#define TAINT_FLAG 0x00D413A0

// luaL_unref at 0x84F7A0 — release reference from table[ref], return ref to freelist
// Fast path: inline the freelist-update chain for valid refs
// Falls back to original for negative refs or invalid states.
typedef int(__cdecl *unref_fn)(uintptr_t L, int t, int ref);
static unref_fn orig = nullptr;
static volatile long g_hits = 0, g_misses = 0;

static int __cdecl hook(uintptr_t L, int t, int ref) {
    if (L < 0x10000 || L > 0xFFE00000) { g_misses++; return orig(L, t, ref); }
    if (ref <= 0) { g_misses++; return orig(L, t, ref); }

    __try {
        // Save/restore taint for secure execution (engine clears it around the
        // freelist update so re-referencing never taints the registry).
        int32_t savedTaintFlag = *(int32_t*)TAINT_FLAG;
        int32_t savedTaintCell = *(int32_t*)0x00D4139C;
        *(int32_t*)TAINT_FLAG = 0;
        *(int32_t*)0x00D4139C = 0;

        // Normalize the table index ONCE to an absolute index, exactly as the
        // engine does — the stack grows by one between ops, so a relative index
        // would drift. Pseudo-indices (registry -10000) and positive indices are
        // left untouched; only [-9999..-1] and 0 are made absolute.
        typedef int(__cdecl *gettop_fn)(uintptr_t);
        int ti = t;
        if ((uint32_t)t >= 0xFFFFD8F1u || t == 0)
            ti = t + ((gettop_fn)0x0084DBD0)(L) + 1;

        // Engine freelist push: t[ref] = t[0]; t[0] = ref.
        typedef int(__cdecl *rawgeti_fn)(uintptr_t, int, int);
        typedef int(__cdecl *rawseti_fn)(uintptr_t, int, int);
        typedef void(__cdecl *pushint_fn)(uintptr_t, int);

        ((rawgeti_fn)0x0084E670)(L, ti, 0);     // push old freelist head t[0]
        ((rawseti_fn)0x0084EA00)(L, ti, ref);   // t[ref] = old head (pops it)
        ((pushint_fn)0x0084E2D0)(L, ref);       // push ref (lua_pushinteger)
        int result = ((rawseti_fn)0x0084EA00)(L, ti, 0); // t[0] = ref (pops it)

        // Restore taint
        *(int32_t*)0x00D4139C = savedTaintCell;
        *(int32_t*)TAINT_FLAG = savedTaintFlag;

        g_hits++;
        return result;
    } __except(EXCEPTION_EXECUTE_HANDLER) {}

    g_misses++;
    return orig(L, t, ref);
}

bool InstallLuaUnrefFast() {
    void* t = (void*)0x0084F7A0;
    if (MH_CreateHook(t, hook, (void**)&orig) != MH_OK) return false;
    WO_EnableHook(t);
    Log("[LUnref] ACTIVE — luaL_unref inline at 0x84F7A0");
    CrashDumper::RegisterFeature("LUnref");
    CrashDumper::FeatureSetActive("LUnref", true);
    return true;
}
