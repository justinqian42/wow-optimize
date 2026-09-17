#include "lua_rawget_inline.h"
#include <windows.h>
#include <cstdint>
#include "MinHook.h"
#include "version.h"
#include "crash_dumper.h"
#include "lua_optimize.h"

extern "C" void Log(const char* fmt, ...);

// Statistics
// Whether the hook actually went in, so the report can tell a guard
// that never fired from one that was never installed.
static bool g_statsInstalled = false;

static volatile long g_rawgetCalls = 0;
static volatile long g_rawgetFast  = 0;

#define TAINT_CELL ( *(uint32_t**)0x00D4139C )

typedef int (__cdecl* lua_rawget_fn)(uintptr_t L, int idx);
static lua_rawget_fn orig_rawget = nullptr;

// The same one-off that kept lua_rawgeti out of its own fast path. Three field
// sessions: 61362728 calls with 576 taking it, 5557775 with 170, 8322978 with
// 187. Nought point nought per cent, every time.
//
// lua_rawget is reached as lua_rawget(L, LUA_GLOBALSINDEX) for a global read and
// as lua_rawget(L, LUA_REGISTRYINDEX) for a registry one, and both are below the
// `idx > -10000` floor the stack-slot resolution uses. So the fast path was
// available for exactly the indices nobody passes.
//
// Resolved out of the client's own index2adr at 0x0084D9C0, which is explicit:
//
//     case -10000:  return (_DWORD *)(a2[5] + 104);   // l_G + 104
//     case -10002:  return a2 + 18;                   // L + 72
//
// a2 is the lua_State as dwords, so a2[5] is L->l_G at 0x14. -10001 stays with
// the engine: that case writes four lua_State fields before returning.
//
// Checked rather than asserted. The first calls compare this against what the
// client's index2adr returns for the same arguments, through a naked thunk -
// that function is __usercall with the index in EAX and the state in ECX - and
// one disagreement retires the pseudo-index path for the session.
static const int kRegistryIndex = -10000;
static const int kGlobalsIndex  = -10002;

static const long kPseudoProve = 20000;
static long g_pseudoChecked = 0;
static long g_pseudoUsed    = 0;
static bool g_pseudoDead    = false;

static __declspec(naked) int* __cdecl ClientIndex2Adr(int /*idx*/, void* /*L*/) {
    __asm {
        mov  eax, [esp+4]
        mov  ecx, [esp+8]
        push ebx
        push esi
        push edi
        push ebp
        mov  ebx, 0x0084D9C0
        call ebx
        pop  ebp
        pop  edi
        pop  esi
        pop  ebx
        ret
    }
}

static __forceinline bool IsValidPtr(uintptr_t p) {
    return p > 0x10000 && p < 0xFFE00000;
}

// Hooked lua_rawget (0x84E600): reads table at idx, lookup using key at L->top - 1,
// replaces the key at L->top - 1 with the retrieved value. Stack height doesn't change.
static int __cdecl Hooked_RawGet(uintptr_t L, int idx) {
    ++g_rawgetCalls;

    // GuardActive() rather than IsReloading() || IsSwapping(): the same three
    // flags in the same order, read inline. The pair of calls cost four call and
    // return pairs on a path this hook takes 578000000 times a session.
    if (LuaOpt::GuardActive()) {
        return orig_rawget(L, idx);
    }

    if (L < 0x10000 || L > 0xFFE00000) {
        return orig_rawget(L, idx);
    }

    __try {
        int* L_base = *(int**)(L + 0x10);  // L->base
        int* L_top  = *(int**)(L + 0x0C);  // L->top

        // Validate stack pointers
        if ((uintptr_t)L_base < 0x10000 || (uintptr_t)L_base > 0xFFE00000 ||
            (uintptr_t)L_top < 0x10000 || (uintptr_t)L_top > 0xFFE00000) {
            return orig_rawget(L, idx);
        }

        // Fast inline stack lookup
        int* tableSlot = nullptr;
        if (idx > 0) {
            int* targetSlot = L_base + (idx - 1) * 4;
            if (targetSlot < L_top) {
                tableSlot = targetSlot;
            }
        } else if (idx < 0 && idx > -10000) {
            int* targetSlot = L_top + idx * 4;
            if (targetSlot >= L_base) {
                tableSlot = targetSlot;
            }
        } else if ((idx == kRegistryIndex || idx == kGlobalsIndex) && !g_pseudoDead) {
            int* slot = nullptr;
            if (idx == kGlobalsIndex) {
                slot = (int*)(L + 72);
            } else {
                uintptr_t g = *(uintptr_t*)(L + 0x14);   // L->l_G
                if (IsValidPtr(g)) slot = (int*)(g + 104);
            }
            if (slot && g_pseudoChecked < kPseudoProve) {
                int* theirs = ClientIndex2Adr(idx, (void*)L);
                ++g_pseudoChecked;
                if (theirs != slot) {
                    g_pseudoDead = true;
                    Log("[RawGet] pseudo-index path retired: index %d resolved "
                        "0x%08X here and 0x%08X in the client's own index2adr. "
                        "Nothing was read from ours, so this call and the "
                        "session are unaffected.",
                        idx, (unsigned)(uintptr_t)slot,
                        (unsigned)(uintptr_t)theirs);
                    slot = nullptr;
                }
            }
            if (slot) { tableSlot = slot; ++g_pseudoUsed; }
        }

        if (tableSlot && IsValidPtr((uintptr_t)tableSlot) && tableSlot[2] == 5) { // LUA_TTABLE
            uintptr_t top = *(uintptr_t*)(L + 0x0C);
            uintptr_t key = top - 16;
            uintptr_t base = *(uintptr_t*)(L + 0x10);
            if (IsValidPtr(top) && IsValidPtr(base) && key >= base) {
                uintptr_t tab = *(uintptr_t*)(tableSlot + 0);
                if (IsValidPtr(tab)) {
                    // Call the engine's luaH_get (0x0085C470)
                    typedef uintptr_t (__cdecl *luaH_get_t)(uintptr_t t, uintptr_t key);
                    luaH_get_t luaH_get = (luaH_get_t)0x0085C470;
                    uintptr_t val = luaH_get(tab, key);
                    if (IsValidPtr(val)) {
                        // Replace key at stack top with value
                        *(uint64_t*)(key + 0) = *(uint64_t*)(val + 0);
                        *(int*)(key + 8) = *(int*)(val + 8);
                        
                        // Handle taint — engine checks val+12 (taint), not val+8 (tt)
                        uint32_t found_taint = *(uint32_t*)(val + 12);
                        if (found_taint == 0) {
                            uint32_t gt = *(uint32_t*)0x00D4139C;
                            *(uint32_t*)(key + 12) = gt;
                            ++g_rawgetFast;
                            return (int)gt;
                        } else {
                            if (*(uint32_t*)0x00D413A0 && !*(uint32_t*)0x00D413A4) {
                                *(uint32_t*)0x00D4139C = found_taint;
                            }
                            *(uint32_t*)(key + 12) = found_taint;
                            ++g_rawgetFast;
                            return (int)found_taint;
                        }
                    }
                }
            }
        }
    } __except(EXCEPTION_EXECUTE_HANDLER) {}

    return orig_rawget(L, idx);
}

bool InstallLuaRawGetInline() {
    void* target = (void*)0x0084E600;
    unsigned char* p = (unsigned char*)target;
    if (p[0] != 0x55 || p[1] != 0x8B) {
        Log("[LuaRawGet] BAD PROLOGUE at 0x%08X (expected 55 8B)", (uintptr_t)target);
        return false;
    }
    if (MH_CreateHook(target, (void*)Hooked_RawGet, (void**)&orig_rawget) != MH_OK) {
        Log("[LuaRawGet] MH_CreateHook FAILED");
        return false;
    }
    if (MH_EnableHook(target) != MH_OK) {
        Log("[LuaRawGet] MH_EnableHook FAILED");
        return false;
    }
    Log("[LuaRawGet] ACTIVE: inline lua_rawget (0x84E600)");
    CrashDumper::RegisterFeature("LuaRawGet");
    CrashDumper::FeatureSetActive("LuaRawGet", true);
    g_statsInstalled = true;
    return true;
}

// Printed from the periodic report. The counters used to be printed only
// from the uninstall path, which nothing calls: the DLL leaves through
// TerminateProcess, and the linker had dropped the function outright.
void LuaRawGetInline_LogStats(void) {
    if (!g_statsInstalled) {
        Log("[LuaRawGet] not measured: the hook is not installed.");
        return;
    }
    const LONG64 total = g_rawgetCalls, fast = g_rawgetFast;
    if (total == 0) {
        Log("[LuaRawGet] measured and zero: no raw read reached it.");
        return;
    }
    Log("[LuaRawGet] %lld calls, %lld inline (%.1f%%).",
        (long long)total, (long long)fast,
        100.0 * (double)fast / (double)total);
    if (g_pseudoUsed || g_pseudoDead)
        Log("[LuaRawGet]   the globals and registry indices resolved here rather "
            "than deferring: %ld call(s), %ld checked against the client's own "
            "index2adr%s. Those two are how a global read and a registry read "
            "arrive, and they used to fall through by one.",
            g_pseudoUsed, g_pseudoChecked,
            g_pseudoDead ? " - RETIRED, one disagreed" : "");
}

void UninstallLuaRawGetInline() {
    MH_DisableHook((void*)0x0084E600);
    MH_RemoveHook((void*)0x0084E600);
    CrashDumper::FeatureSetActive("LuaRawGet", false);
    LONG64 total = g_rawgetCalls;
    LONG64 fast  = g_rawgetFast;
    if (total > 0) {
        Log("[LuaRawGet] Stats: %lld calls, %lld inline (%.1f%%)",
            (long long)total, (long long)fast,
            100.0 * (double)fast / (double)total);
    }
}
