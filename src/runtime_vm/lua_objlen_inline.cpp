#include "lua_objlen_inline.h"
#include <windows.h>
#include <cstdint>
#include "MinHook.h"
#include "version.h"
#include "crash_dumper.h"

extern "C" void Log(const char* fmt, ...);

static volatile long g_objlenCalls = 0;
static volatile long g_objlenFast  = 0;

#define LUA_TNUMBER   3
#define LUA_TSTRING   4
#define LUA_TTABLE    5
#define LUA_TUSERDATA 7
#define NIL_OBJECT    0x00A46F78

static inline bool IsTeardownState() {
    uintptr_t gL = *(uintptr_t*)0x00D3F78C;
    return (gL < 0x10000);
}

static bool IsValidPtr(uintptr_t p) {
    return p > 0x10000;
}

static uintptr_t ResolveTValue(uintptr_t L, int idx, bool* defer) {
    *defer = false;
    if (idx > 0) {
        uintptr_t base = *(uintptr_t*)(L + 0x10);
        if (!IsValidPtr(base)) return 0;
        uintptr_t tv = base + (idx - 1) * 16;
        if (tv >= *(uintptr_t*)(L + 0x0C)) return 0;
        return tv;
    }
    if (idx < 0 && idx > -10000) {
        uintptr_t top = *(uintptr_t*)(L + 0x0C);
        if (!IsValidPtr(top)) return 0;
        uintptr_t tv = top + idx * 16;
        if (tv < *(uintptr_t*)(L + 0x10)) return 0;
        return tv;
    }
    *defer = true;
    return 0;
}

typedef int (__cdecl *lua_objlen_fn)(uintptr_t L, int idx);
static lua_objlen_fn orig_objlen = nullptr;

static int __cdecl Hooked_ObjLen(uintptr_t L, int idx) {
    ++g_objlenCalls;
    __try {
        if (IsTeardownState()) goto fallback;
        bool defer = false;
        uintptr_t tv = ResolveTValue(L, idx, &defer);
        if (!defer && tv && tv != NIL_OBJECT) {
            int tt = *(int*)(tv + 8);
            uintptr_t gc = *(uintptr_t*)(tv + 0);
            if (tt == LUA_TSTRING && IsValidPtr(gc))
                { ++g_objlenFast; return *(int*)(gc + 16); }
            if (tt == LUA_TUSERDATA && IsValidPtr(gc))
                { ++g_objlenFast; return *(int*)(gc + 20); }
            if (tt == LUA_TTABLE && IsValidPtr(gc)) {
                ++g_objlenFast;
                typedef unsigned (__cdecl *getn_fn)(uintptr_t);
                return ((getn_fn)0x0085C690)(gc);
            }
        }
    } __except(EXCEPTION_EXECUTE_HANDLER) {}
fallback:
    return orig_objlen(L, idx);
}

bool InstallLuaObjLenInline() {
    void* target = (void*)0x0084E150;
    if (*(unsigned char*)target != 0x55 || *((unsigned char*)target + 1) != 0x8B) {
        Log("[LuaObjLen] BAD PROLOGUE at 0x%08X", (uintptr_t)target);
        return false;
    }
    if (MH_CreateHook(target, Hooked_ObjLen, (void**)&orig_objlen) != MH_OK) {
        Log("[LuaObjLen] MH_CreateHook FAILED");
        return false;
    }
    MH_EnableHook(target);
    Log("[LuaObjLen] ACTIVE: lua_objlen at 0x84E150 (returns length in EAX)");
    CrashDumper::RegisterFeature("LuaObjLen");
    CrashDumper::FeatureSetActive("LuaObjLen", true);
    return true;
}

void UninstallLuaObjLenInline() {
    MH_DisableHook((void*)0x0084E150);
    MH_RemoveHook((void*)0x0084E150);
}
