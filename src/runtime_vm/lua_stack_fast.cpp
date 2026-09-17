#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <cstdint>
#include "MinHook.h"
#include "version.h"
#include "lua_stack_fast.h"

extern "C" void Log(const char* fmt, ...);

// Shared: symbolic type constants and secure taint cells
#define LUA_TNIL              0
#define LUA_TBOOLEAN          1
#define LUA_TLIGHTUSERDATA    2
#define LUA_TNUMBER           3
#define LUA_TSTRING           4
#define LUA_TTABLE            5
#define LUA_TFUNCTION         6
#define LUA_TUSERDATA         7
#define LUA_TTHREAD           8

#define TAINT_CELL ( *(uint32_t*)0x00D4139C )
static const uint32_t TAINT_A0   = 0x00D413A0;
static const uint32_t TAINT_A4   = 0x00D413A4;

// Pseudo-index boundary
#define LUA_REGISTRYINDEX (-10000)

// nil sentinel (the object returned for invalid indices)
static const uintptr_t NIL_OBJECT = 0x00A46F78;

// Teardown guard: if the lua_State global is zero, the Lua VM is
// being torn down and stack pointers are stale.
#include "../allocators/loading_defrag.h"

extern "C" bool LuaOpt_IsTeardown();
static inline bool IsTeardownState() {
    if (LoadingDefrag::IsLoadingActive()) return true;
    uintptr_t gL = *(uintptr_t*)0x00D3F78C;
    return (gL < 0x10000 || gL > 0xFFE00000);
}

// Pointer sanity
static __forceinline bool IsValidPtr(uintptr_t p) {
    return p > 0x10000 && p < 0xFFE00000;
}

// Shared inline index resolution (positive/negative stack indices only)
static __forceinline uintptr_t ResolveTValue(
    uintptr_t L, int idx, bool* deferToOrig)
{
    *deferToOrig = false;
    if (idx > 8000 || idx < -8000) {
        *deferToOrig = true;
        return 0;
    }
    if (idx > 0) {
        uintptr_t base = *(uintptr_t*)(L + 0x10);
        if (!IsValidPtr(base)) {
            *deferToOrig = true;
            return 0;
        }
        uintptr_t tv = base + (uintptr_t)(idx - 1) * 16;
        uintptr_t top = *(uintptr_t*)(L + 0x0C);
        if (tv >= top) return 0; // Truly out of bounds
        return tv;
    }
    if (idx < 0 && idx > LUA_REGISTRYINDEX) {
        uintptr_t top = *(uintptr_t*)(L + 0x0C);
        if (!IsValidPtr(top)) {
            *deferToOrig = true;
            return 0;
        }
        uintptr_t tv = top + (uintptr_t)idx * 16;
        uintptr_t base = *(uintptr_t*)(L + 0x10);
        if (tv < base) return 0; // Truly out of bounds
        return tv;
    }
    // Pseudo-index or 0 — can't inline
    *deferToOrig = true;
    return 0;
}

// ================================================================
// 1. lua_pushnil — 0x84E280
// ================================================================
typedef int (__cdecl *pushnil_t)(uintptr_t L);
static pushnil_t orig_pushnil = nullptr;

static int __cdecl hook_pushnil(uintptr_t L) {
    if (IsTeardownState()) return orig_pushnil(L);
    __try {
        uintptr_t top = *(uintptr_t*)(L + 0x0C);
        if (!IsValidPtr(top)) return orig_pushnil(L);
        uint32_t taint = TAINT_CELL;
        *(uint32_t*)(top + 0) = 0;
        *(uint32_t*)(top + 4) = 0;
        *(uint32_t*)(top + 8) = 0;
        *(uint32_t*)(top + 12) = taint;
        *(uintptr_t*)(L + 0x0C) = top + 16;
        return (int)L;
    } __except(EXCEPTION_EXECUTE_HANDLER) {}
    return orig_pushnil(L);
}

// ================================================================
// 2. lua_pushinteger — 0x84E2D0
// ================================================================
typedef int (__cdecl *pushinteger_t)(uintptr_t L, int n);
static pushinteger_t orig_pushinteger = nullptr;

static int __cdecl hook_pushinteger(uintptr_t L, int n) {
    if (IsTeardownState()) return orig_pushinteger(L, n);
    __try {
        uintptr_t top = *(uintptr_t*)(L + 0x0C);
        if (!IsValidPtr(top)) return orig_pushinteger(L, n);
        uint32_t taint = TAINT_CELL;
        double dn = (double)n;
        *(double*)(top + 0) = dn;
        *(uint32_t*)(top + 8) = LUA_TNUMBER;
        *(uint32_t*)(top + 12) = taint;
        *(uintptr_t*)(L + 0x0C) = top + 16;
        return (int)top;
    } __except(EXCEPTION_EXECUTE_HANDLER) {}
    return orig_pushinteger(L, n);
}

// ================================================================
// 3. lua_pushboolean — 0x84E4D0
// ================================================================
typedef __int64 (__cdecl *pushboolean_t)(uintptr_t L, int b);
static pushboolean_t orig_pushboolean = nullptr;

static __int64 __cdecl hook_pushboolean(uintptr_t L, int b) {
    if (IsTeardownState()) return orig_pushboolean(L, b);
    __try {
        uintptr_t top = *(uintptr_t*)(L + 0x0C);
        if (!IsValidPtr(top)) return orig_pushboolean(L, b);
        uint32_t taint = TAINT_CELL;
        *(uint32_t*)(top + 0) = (b != 0) ? 1 : 0;
        *(uint32_t*)(top + 4) = 0;
        *(uint32_t*)(top + 8) = LUA_TBOOLEAN;
        *(uint32_t*)(top + 12) = taint;
        *(uintptr_t*)(L + 0x0C) = top + 16;
        
        uint64_t res = (uint64_t)(top) | ((uint64_t)(b != 0) << 32);
        return (__int64)res;
    } __except(EXCEPTION_EXECUTE_HANDLER) {}
    return orig_pushboolean(L, b);
}

// ================================================================
// 4. lua_pushlightuserdata — 0x84E500
// ================================================================
typedef int (__cdecl *pushlightuserdata_t)(uintptr_t L, uintptr_t p);
static pushlightuserdata_t orig_pushlightuserdata = nullptr;

static int __cdecl hook_pushlightuserdata(uintptr_t L, uintptr_t p) {
    if (IsTeardownState()) return orig_pushlightuserdata(L, p);
    __try {
        uintptr_t top = *(uintptr_t*)(L + 0x0C);
        if (!IsValidPtr(top)) return orig_pushlightuserdata(L, p);
        uint32_t taint = TAINT_CELL;
        *(uintptr_t*)(top + 0) = p;
        *(uint32_t*)(top + 8) = LUA_TLIGHTUSERDATA;
        *(uint32_t*)(top + 12) = taint;
        *(uintptr_t*)(L + 0x0C) = top + 16;
        return (int)top;
    } __except(EXCEPTION_EXECUTE_HANDLER) {}
    return orig_pushlightuserdata(L, p);
}

// ================================================================
// 5. lua_type — 0x84DEB0
// ================================================================
typedef int (__cdecl *lua_type_t)(uintptr_t L, int idx);
static lua_type_t orig_lua_type = nullptr;

static int __cdecl hook_lua_type(uintptr_t L, int idx) {
    if (IsTeardownState()) return orig_lua_type(L, idx);
    __try {
        bool defer = false;
        uintptr_t tv = ResolveTValue(L, idx, &defer);
        if (defer) return orig_lua_type(L, idx);
        if (tv) {
            if (tv == NIL_OBJECT) return -1;
            return *(int*)(tv + 8);
        }
    } __except(EXCEPTION_EXECUTE_HANDLER) {}
    return orig_lua_type(L, idx);
}

// ================================================================
// 6. lua_isfunction — 0x84DEF0
// ================================================================
typedef int (__cdecl *lua_isfunc_t)(uintptr_t L, int idx);
static lua_isfunc_t orig_lua_isfunction = nullptr;

static int __cdecl hook_lua_isfunction(uintptr_t L, int idx) {
    if (IsTeardownState()) return orig_lua_isfunction(L, idx);
    __try {
        bool defer = false;
        uintptr_t tv = ResolveTValue(L, idx, &defer);
        if (defer) return orig_lua_isfunction(L, idx);
        if (tv && tv != NIL_OBJECT && *(int*)(tv + 8) == LUA_TFUNCTION) {
            uintptr_t gc = *(uintptr_t*)(tv + 0);
            if (IsValidPtr(gc)) {
                return (*(uint8_t*)(gc + 10) != 0) ? 1 : 0;
            }
            return orig_lua_isfunction(L, idx);
        }
        return 0;
    } __except(EXCEPTION_EXECUTE_HANDLER) {}
    return orig_lua_isfunction(L, idx);
}

// ================================================================
// 7. lua_isstring — 0x84DF60
// ================================================================
typedef int (__cdecl *lua_isstring_t)(uintptr_t L, int idx);
static lua_isstring_t orig_lua_isstring = nullptr;

static int __cdecl hook_lua_isstring(uintptr_t L, int idx) {
    if (IsTeardownState()) return orig_lua_isstring(L, idx);
    __try {
        bool defer = false;
        uintptr_t tv = ResolveTValue(L, idx, &defer);
        if (defer) return orig_lua_isstring(L, idx);
        if (tv && tv != NIL_OBJECT) {
            int tt = *(int*)(tv + 8);
            if (tt == LUA_TSTRING || tt == LUA_TNUMBER)
                return 1;
        }
        return 0;
    } __except(EXCEPTION_EXECUTE_HANDLER) {}
    return orig_lua_isstring(L, idx);
}

// ================================================================
// 8. lua_tothread — 0x84E1F0
// ================================================================
typedef int (__cdecl *lua_tothread_t)(uintptr_t L, int idx);
static lua_tothread_t orig_lua_tothread = nullptr;

static int __cdecl hook_lua_tothread(uintptr_t L, int idx) {
    if (IsTeardownState()) return orig_lua_tothread(L, idx);
    __try {
        bool defer = false;
        uintptr_t tv = ResolveTValue(L, idx, &defer);
        if (defer) return orig_lua_tothread(L, idx);
        if (tv && tv != NIL_OBJECT && *(int*)(tv + 8) == LUA_TTHREAD) {
            return *(int*)(tv + 0);
        }
        return 0;
    } __except(EXCEPTION_EXECUTE_HANDLER) {}
    return orig_lua_tothread(L, idx);
}

// ================================================================
// 9. lua_remove — 0x0084DC50
// ================================================================
typedef int (__cdecl* lua_remove_t)(uintptr_t L, int idx);
static lua_remove_t orig_lua_remove = nullptr;

// `processed` is set before the first write, not after the last. These three
// functions shift live stack slots in place; if a fault lands mid-shift and the
// handler falls through to the original, the original shifts an already-shifted
// stack and the damage doubles. Once a single slot has been touched, the only
// safe exit is our own.
static int __cdecl hook_lua_remove(uintptr_t L, int idx) {
    if (IsTeardownState()) return orig_lua_remove(L, idx);
    bool processed = false;
    __try {
        uintptr_t top = *(uintptr_t*)(L + 0x0C);
        uintptr_t base = *(uintptr_t*)(L + 0x10);
        if (IsValidPtr(top) && IsValidPtr(base)) {
            bool defer = false;
            uintptr_t target = ResolveTValue(L, idx, &defer);
            if (!defer && target && target >= base && target < top) {
                uint32_t a0 = *(uint32_t*)TAINT_A0;
                uint32_t a4 = *(uint32_t*)TAINT_A4;
                uint32_t current_taint = TAINT_CELL;

                processed = true;
                uintptr_t curr = target + 16;
                while (curr < top) {
                    uint32_t val0 = *(uint32_t*)(curr + 0);
                    uint32_t val1 = *(uint32_t*)(curr + 4);
                    uint32_t val2 = *(uint32_t*)(curr + 8);
                    uint32_t val3 = *(uint32_t*)(curr + 12);

                    *(uint32_t*)(curr - 16) = val0;
                    *(uint32_t*)(curr - 12) = val1;
                    *(uint32_t*)(curr - 8) = val2;

                    if (val3 != 0) {
                        *(uint32_t*)(curr - 4) = val3;
                        if (a0 && !a4) {
                            current_taint = val3;
                        }
                    } else {
                        *(uint32_t*)(curr - 4) = current_taint;
                    }
                    curr += 16;
                }

                if (a0 && !a4) {
                    *(uint32_t*)0x00D4139C = current_taint;
                }

                *(uintptr_t*)(L + 0x0C) = top - 16;
                return (int)top;
            }
        }
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        if (processed) return 0;   // stack already moved - never re-run the original
    }
    return orig_lua_remove(L, idx);
}

// ================================================================
// 10. lua_insert — 0x0084DCC0
// ================================================================
typedef int (__cdecl* lua_insert_t)(uintptr_t L, int idx);
static lua_insert_t orig_lua_insert = nullptr;

static int __cdecl hook_lua_insert(uintptr_t L, int idx) {
    if (IsTeardownState()) return orig_lua_insert(L, idx);
    bool processed = false;
    uintptr_t target = 0;
    __try {
        uintptr_t top = *(uintptr_t*)(L + 0x0C);
        uintptr_t base = *(uintptr_t*)(L + 0x10);
        if (IsValidPtr(top) && IsValidPtr(base)) {
            bool defer = false;
            target = ResolveTValue(L, idx, &defer);
            if (!defer && target && target >= base && target < top) {
                uint32_t a0 = *(uint32_t*)TAINT_A0;
                uint32_t a4 = *(uint32_t*)TAINT_A4;
                uint32_t current_taint = TAINT_CELL;

                processed = true;
                // Copy original top element (now at top - 16) to temp
                uintptr_t src_top = top - 16;
                uint32_t t_val0 = *(uint32_t*)(src_top + 0);
                uint32_t t_val1 = *(uint32_t*)(src_top + 4);
                uint32_t t_val2 = *(uint32_t*)(src_top + 8);
                uint32_t t_val3 = *(uint32_t*)(src_top + 12);

                // Shift elements up from target to top - 32
                uintptr_t curr = top - 16;
                while (curr > target) {
                    uintptr_t src = curr - 16;
                    uint32_t val0 = *(uint32_t*)(src + 0);
                    uint32_t val1 = *(uint32_t*)(src + 4);
                    uint32_t val2 = *(uint32_t*)(src + 8);
                    uint32_t val3 = *(uint32_t*)(src + 12);

                    *(uint32_t*)(curr + 0) = val0;
                    *(uint32_t*)(curr + 4) = val1;
                    *(uint32_t*)(curr + 8) = val2;

                    if (val3 != 0) {
                        *(uint32_t*)(curr + 12) = val3;
                        if (a0 && !a4) {
                            current_taint = val3;
                        }
                    } else {
                        *(uint32_t*)(curr + 12) = current_taint;
                    }
                    curr -= 16;
                }

                *(uint32_t*)(target + 0) = t_val0;
                *(uint32_t*)(target + 4) = t_val1;
                *(uint32_t*)(target + 8) = t_val2;

                if (t_val3 != 0) {
                    *(uint32_t*)(target + 12) = t_val3;
                    if (a0 && !a4) {
                        current_taint = t_val3;
                    }
                } else {
                    *(uint32_t*)(target + 12) = current_taint;
                }

                if (a0 && !a4) {
                    *(uint32_t*)0x00D4139C = current_taint;
                }

                return (int)target;
            }
        }
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        if (processed) return (int)target;   // stack already moved
    }
    return orig_lua_insert(L, idx);
}

// ================================================================
// 11. lua_replace — 0x0084DD70
// ================================================================
typedef int (__cdecl* lua_replace_t)(uintptr_t L, int idx);
static lua_replace_t orig_lua_replace = nullptr;

static int __cdecl hook_lua_replace(uintptr_t L, int idx) {
    if (IsTeardownState()) return orig_lua_replace(L, idx);
    bool processed = false;
    uintptr_t target = 0;
    __try {
        uintptr_t top = *(uintptr_t*)(L + 0x0C);
        uintptr_t base = *(uintptr_t*)(L + 0x10);
        if (IsValidPtr(top) && IsValidPtr(base)) {
            bool defer = false;
            target = ResolveTValue(L, idx, &defer);
            if (!defer && target && target >= base && target < top && idx >= -10002) {
                processed = true;
                uint32_t a0 = *(uint32_t*)TAINT_A0;
                uint32_t a4 = *(uint32_t*)TAINT_A4;

                uintptr_t src = top - 16;
                uint32_t val0 = *(uint32_t*)(src + 0);
                uint32_t val1 = *(uint32_t*)(src + 4);
                uint32_t val2 = *(uint32_t*)(src + 8);
                uint32_t val3 = *(uint32_t*)(src + 12);

                *(uint32_t*)(target + 0) = val0;
                *(uint32_t*)(target + 4) = val1;
                *(uint32_t*)(target + 8) = val2;

                *(uint32_t*)(target + 12) = val3;
                if (val3 != 0) {
                    if (a0 && !a4) {
                        *(uint32_t*)0x00D4139C = val3;
                    }
                }

                *(uintptr_t*)(L + 0x0C) = top - 16;
                return (int)target;
            }
        }
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        if (processed) return (int)target;   // stack already moved
    }
    return orig_lua_replace(L, idx);
}

// ================================================================
// 12. lua_touserdata — 0x0084E1C0
// ================================================================
typedef void* (__cdecl *lua_touserdata_fn)(uintptr_t L, int idx);
static lua_touserdata_fn orig_lua_touserdata = nullptr;

static void* __cdecl hook_lua_touserdata(uintptr_t L, int idx) {
    if (IsTeardownState()) return orig_lua_touserdata(L, idx);
    __try {
        bool defer = false;
        uintptr_t tv = ResolveTValue(L, idx, &defer);
        if (defer) return orig_lua_touserdata(L, idx);
        if (tv && tv != NIL_OBJECT) {
            int tt = *(int*)(tv + 8);
            if (tt == LUA_TLIGHTUSERDATA) {
                return *(void**)(tv + 0);
            }
            if (tt == LUA_TUSERDATA) {
                uintptr_t udata = *(uintptr_t*)(tv + 0);
                if (IsValidPtr(udata)) {
                    return (void*)(udata + 24);
                }
                return orig_lua_touserdata(L, idx);
            }
        }
        return nullptr;
    } __except(EXCEPTION_EXECUTE_HANDLER) {}
    return orig_lua_touserdata(L, idx);
}

// ================================================================
// 13. lua_getmetatable — 0x0084E730
// ================================================================
typedef int (__cdecl *lua_getmetatable_fn)(uintptr_t L, int idx);
static lua_getmetatable_fn orig_lua_getmetatable = nullptr;

static int __cdecl hook_lua_getmetatable(uintptr_t L, int idx) {
    if (IsTeardownState()) return orig_lua_getmetatable(L, idx);
    __try {
        bool defer = false;
        uintptr_t tv = ResolveTValue(L, idx, &defer);
        if (defer) return orig_lua_getmetatable(L, idx);
        if (tv && tv != NIL_OBJECT) {
            int tt = *(int*)(tv + 8);
            uintptr_t mt = 0;
            if (tt == LUA_TTABLE || tt == LUA_TUSERDATA) {
                uintptr_t obj = *(uintptr_t*)(tv + 0);
                if (IsValidPtr(obj)) {
                    mt = *(uintptr_t*)(obj + 12);
                } else {
                    return orig_lua_getmetatable(L, idx);
                }
            } else {
                uintptr_t g = *(uintptr_t*)(L + 0x14);
                if (IsValidPtr(g)) {
                    mt = *(uintptr_t*)(g + 4 * tt + 160);
                } else {
                    return orig_lua_getmetatable(L, idx);
                }
            }

            if (mt) {
                uintptr_t top = *(uintptr_t*)(L + 0x0C);
                if (IsValidPtr(top)) {
                    *(uintptr_t*)(top + 0) = mt;
                    *(int*)(top + 8) = LUA_TTABLE;
                    *(uint32_t*)(top + 12) = TAINT_CELL;
                    *(uintptr_t*)(L + 0x0C) = top + 16;
                    return 1;
                } else {
                    return orig_lua_getmetatable(L, idx);
                }
            }
            return 0;
        }
    } __except(EXCEPTION_EXECUTE_HANDLER) {}
    return orig_lua_getmetatable(L, idx);
}

// ================================================================
// 14. lua_setmetatable — 0x0084EA90
// ================================================================
typedef int (__cdecl* lua_setmetatable_fn)(uintptr_t L, int idx);
static lua_setmetatable_fn orig_lua_setmetatable = nullptr;

static int __cdecl hook_lua_setmetatable(uintptr_t L, int idx) {
    if (IsTeardownState()) return orig_lua_setmetatable(L, idx);
    __try {
        uintptr_t top = *(uintptr_t*)(L + 0x0C);
        uintptr_t base = *(uintptr_t*)(L + 0x10);
        if (IsValidPtr(top) && IsValidPtr(base)) {
            bool defer = false;
            uintptr_t tv = ResolveTValue(L, idx, &defer);
            if (!defer && tv && tv != NIL_OBJECT) {
                uintptr_t mt_tv = top - 16;
                uintptr_t mt = 0;
                if (*(int*)(mt_tv + 8) != LUA_TNIL) {
                    mt = *(uintptr_t*)(mt_tv + 0);
                }

                int tt = *(int*)(tv + 8);
                if (tt == LUA_TTABLE) {
                    uintptr_t tbl = *(uintptr_t*)(tv + 0);
                    if (IsValidPtr(tbl)) {
                        if (mt && (*(uint8_t*)(mt + 9) & 3) != 0 && (*(uint8_t*)(tbl + 9) & 4) != 0) {
                            goto fallback;
                        }
                        *(uintptr_t*)(tbl + 12) = mt;
                    } else {
                        goto fallback;
                    }
                } else if (tt == LUA_TUSERDATA) {
                    uintptr_t udata = *(uintptr_t*)(tv + 0);
                    if (IsValidPtr(udata)) {
                        if (mt && (*(uint8_t*)(mt + 9) & 3) != 0 && (*(uint8_t*)(udata + 9) & 4) != 0) {
                            goto fallback;
                        }
                        *(uintptr_t*)(udata + 12) = mt;
                    } else {
                        goto fallback;
                    }
                } else {
                    uintptr_t g = *(uintptr_t*)(L + 0x14);
                    if (IsValidPtr(g)) {
                        *(uintptr_t*)(g + 4 * tt + 160) = mt;
                    } else {
                        goto fallback;
                    }
                }

                *(uintptr_t*)(L + 0x0C) = top - 16;
                return 1;
            }
        }
    } __except(EXCEPTION_EXECUTE_HANDLER) {}

fallback:
    return orig_lua_setmetatable(L, idx);
}

// ================================================================
// 15. lua_setfenv — 0x0084EB40
// ================================================================
typedef int (__cdecl* lua_setfenv_fn)(uintptr_t L, int idx);
static lua_setfenv_fn orig_lua_setfenv = nullptr;

static int __cdecl hook_lua_setfenv(uintptr_t L, int idx) {
    if (IsTeardownState()) return orig_lua_setfenv(L, idx);
    __try {
        uintptr_t top = *(uintptr_t*)(L + 0x0C);
        uintptr_t base = *(uintptr_t*)(L + 0x10);
        if (IsValidPtr(top) && IsValidPtr(base)) {
            bool defer = false;
            uintptr_t tv = ResolveTValue(L, idx, &defer);
            if (!defer && tv && tv != NIL_OBJECT) {
                uintptr_t env_tv = top - 16;
                uintptr_t env = 0;
                if (*(int*)(env_tv + 8) == LUA_TTABLE) {
                    env = *(uintptr_t*)(env_tv + 0);
                } else {
                    goto fallback;
                }

                int tt = *(int*)(tv + 8);
                int result = 1;
                if (tt == LUA_TFUNCTION) {
                    uintptr_t cl = *(uintptr_t*)(tv + 0);
                    if (IsValidPtr(cl)) {
                        if (env && (*(uint8_t*)(env + 9) & 3) != 0 && (*(uint8_t*)(cl + 9) & 4) != 0) {
                            goto fallback;
                        }
                        *(uintptr_t*)(cl + 16) = env;
                        if (!*(uint8_t*)(cl + 10) && !*(uintptr_t*)(cl + 4)) {
                            *(uintptr_t*)(cl + 4) = TAINT_CELL;
                        }
                    } else {
                        goto fallback;
                    }
                } else if (tt == LUA_TUSERDATA) {
                    uintptr_t udata = *(uintptr_t*)(tv + 0);
                    if (IsValidPtr(udata)) {
                        if (env && (*(uint8_t*)(env + 9) & 3) != 0 && (*(uint8_t*)(udata + 9) & 4) != 0) {
                            goto fallback;
                        }
                        *(uintptr_t*)(udata + 16) = env;
                    } else {
                        goto fallback;
                    }
                } else if (tt == LUA_TTHREAD) {
                    uintptr_t th = *(uintptr_t*)(tv + 0);
                    if (IsValidPtr(th)) {
                        if (env && (*(uint8_t*)(env + 9) & 3) != 0 && (*(uint8_t*)(th + 9) & 4) != 0) {
                            goto fallback;
                        }
                        uintptr_t th_env = th + 72;
                        *(uintptr_t*)(th_env + 12) = TAINT_CELL;
                        *(uintptr_t*)(th_env + 0) = env;
                        *(int*)(th_env + 8) = LUA_TTABLE;
                    } else {
                        goto fallback;
                    }
                } else {
                    result = 0;
                }

                *(uintptr_t*)(L + 0x0C) = top - 16;
                return result;
            }
        }
    } __except(EXCEPTION_EXECUTE_HANDLER) {}

fallback:
    return orig_lua_setfenv(L, idx);
}

// ================================================================
// 16. lua_getfenv — 0x0084E7A0
// ================================================================
typedef int (__cdecl* lua_getfenv_fn)(uintptr_t L, int idx);
static lua_getfenv_fn orig_lua_getfenv = nullptr;

static int __cdecl hook_lua_getfenv(uintptr_t L, int idx) {
    if (IsTeardownState()) return orig_lua_getfenv(L, idx);
    __try {
        bool defer = false;
        uintptr_t tv = ResolveTValue(L, idx, &defer);
        if (!defer && tv && tv != NIL_OBJECT) {
            uintptr_t top = *(uintptr_t*)(L + 0x0C);
            if (IsValidPtr(top)) {
                int tt = *(int*)(tv + 8);
                uintptr_t env = 0;
                uint32_t taint = TAINT_CELL;
                uint32_t push_taint = taint;
                int result = 0;

                uint32_t a0 = *(uint32_t*)TAINT_A0;
                uint32_t a4 = *(uint32_t*)TAINT_A4;

                if (tt == LUA_TFUNCTION) {
                    uintptr_t cl = *(uintptr_t*)(tv + 0);
                    if (IsValidPtr(cl)) {
                        env = *(uintptr_t*)(cl + 16);
                        if (*(uint8_t*)(cl + 10)) { // isC
                            push_taint = taint;
                        } else {
                            uint32_t cl_taint = *(uint32_t*)(cl + 4);
                            if (cl_taint) {
                                push_taint = cl_taint;
                                if (a0 && !a4) {
                                    *(uint32_t*)0x00D4139C = cl_taint;
                                }
                            }
                        }
                        *(uintptr_t*)(top + 0) = env;
                        *(int*)(top + 8) = LUA_TTABLE;
                        *(uint32_t*)(top + 12) = push_taint;
                        result = (int)cl;
                    } else {
                        return orig_lua_getfenv(L, idx);
                    }
                } else if (tt == LUA_TUSERDATA) {
                    uintptr_t udata = *(uintptr_t*)(tv + 0);
                    if (IsValidPtr(udata)) {
                        env = *(uintptr_t*)(udata + 16);
                        *(uintptr_t*)(top + 0) = env;
                        *(int*)(top + 8) = LUA_TTABLE;
                        *(uint32_t*)(top + 12) = taint;
                        result = (int)udata;
                    } else {
                        return orig_lua_getfenv(L, idx);
                    }
                } else if (tt == LUA_TTHREAD) {
                    uintptr_t th = *(uintptr_t*)(tv + 0);
                    if (IsValidPtr(th)) {
                        uintptr_t th_env = th + 72;
                        uint32_t th_val0 = *(uint32_t*)(th_env + 0);
                        uint32_t th_val1 = *(uint32_t*)(th_env + 4);
                        uint32_t th_val2 = *(uint32_t*)(th_env + 8);
                        uint32_t th_val3 = *(uint32_t*)(th_env + 12);

                        *(uint32_t*)(top + 0) = th_val0;
                        *(uint32_t*)(top + 4) = th_val1;
                        *(uint32_t*)(top + 8) = th_val2;

                        if (th_val3) {
                            *(uint32_t*)(top + 12) = th_val3;
                            if (a0 && !a4) {
                                *(uint32_t*)0x00D4139C = th_val3;
                            }
                            result = th_val3;
                        } else {
                            *(uint32_t*)(top + 12) = taint;
                            result = taint;
                        }
                    } else {
                        return orig_lua_getfenv(L, idx);
                    }
                } else {
                    *(uintptr_t*)(top + 0) = 0;
                    *(uintptr_t*)(top + 4) = 0;
                    *(int*)(top + 8) = LUA_TNIL;
                    *(uint32_t*)(top + 12) = taint;
                    result = (int)top;
                }

                *(uintptr_t*)(L + 0x0C) = top + 16;
                return result;
            }
        }
    } __except(EXCEPTION_EXECUTE_HANDLER) {}
    return orig_lua_getfenv(L, idx);
}

// Install / Shutdown

static void* const ADDR_PUSHNIL           = (void*)0x0084E280;
static void* const ADDR_PUSHINTEGER       = (void*)0x0084E2D0;
static void* const ADDR_PUSHBOOLEAN       = (void*)0x0084E4D0;
static void* const ADDR_PUSHLIGHTUSERDATA = (void*)0x0084E500;
static void* const ADDR_LUA_TYPE          = (void*)0x0084DEB0;
static void* const ADDR_LUA_ISFUNCTION    = (void*)0x0084DEF0;
static void* const ADDR_LUA_ISSTRING      = (void*)0x0084DF60;
static void* const ADDR_LUA_TOTHREAD      = (void*)0x0084E1F0;
static void* const ADDR_LUA_REMOVE        = (void*)0x0084DC50;
static void* const ADDR_LUA_INSERT        = (void*)0x0084DCC0;
static void* const ADDR_LUA_REPLACE       = (void*)0x0084DD70;
static void* const ADDR_LUA_TOUSERDATA    = (void*)0x0084E1C0;
static void* const ADDR_LUA_GETMETATABLE  = (void*)0x0084E730;
static void* const ADDR_LUA_SETMETATABLE  = (void*)0x0084EA90;
static void* const ADDR_LUA_SETFENV       = (void*)0x0084EB40;
static void* const ADDR_LUA_GETFENV       = (void*)0x0084E7A0;

bool InstallLuaStackFast() {
    int installed = 0;
    MH_STATUS st;

    #define INSTALL(type, orig_ptr, detour, addr, name) \
        st = WineSafe_CreateHook(addr, (void*)detour, (void**)&orig_ptr); \
        if (st == MH_OK) { WO_EnableHook(addr); installed++; \
            Log("[LuaStackFast] %s ACTIVE (%p)", name, addr); } \
        else { Log("[LuaStackFast] %s FAILED (status %d)", name, (int)st); }

    INSTALL(pushnil_t,           orig_pushnil,           hook_pushnil,           ADDR_PUSHNIL,           "lua_pushnil");
    INSTALL(pushinteger_t,       orig_pushinteger,       hook_pushinteger,       ADDR_PUSHINTEGER,       "lua_pushinteger");
    INSTALL(pushboolean_t,       orig_pushboolean,       hook_pushboolean,       ADDR_PUSHBOOLEAN,       "ADDR_PUSHBOOLEAN");
    INSTALL(pushlightuserdata_t, orig_pushlightuserdata, hook_pushlightuserdata, ADDR_PUSHLIGHTUSERDATA, "lua_pushlightuserdata");
    INSTALL(lua_type_t,         orig_lua_type,          hook_lua_type,          ADDR_LUA_TYPE,          "lua_type");
    INSTALL(lua_isfunc_t,       orig_lua_isfunction,    hook_lua_isfunction,    ADDR_LUA_ISFUNCTION,    "lua_isfunction");
    INSTALL(lua_isstring_t,     orig_lua_isstring,      hook_lua_isstring,      ADDR_LUA_ISSTRING,      "lua_isstring");
    INSTALL(lua_tothread_t,     orig_lua_tothread,      hook_lua_tothread,      ADDR_LUA_TOTHREAD,      "lua_tothread");
    INSTALL(lua_remove_t,       orig_lua_remove,        hook_lua_remove,        ADDR_LUA_REMOVE,        "lua_remove");
    INSTALL(lua_insert_t,       orig_lua_insert,        hook_lua_insert,        ADDR_LUA_INSERT,        "lua_insert");
    INSTALL(lua_replace_t,      orig_lua_replace,       hook_lua_replace,       ADDR_LUA_REPLACE,       "lua_replace");
    INSTALL(lua_touserdata_fn,  orig_lua_touserdata,    hook_lua_touserdata,    ADDR_LUA_TOUSERDATA,    "lua_touserdata");
    INSTALL(lua_getmetatable_fn,orig_lua_getmetatable,  hook_lua_getmetatable,  ADDR_LUA_GETMETATABLE,  "lua_getmetatable");
    INSTALL(lua_setmetatable_fn,orig_lua_setmetatable,  hook_lua_setmetatable,  ADDR_LUA_SETMETATABLE,  "lua_setmetatable");
    INSTALL(lua_setfenv_fn,     orig_lua_setfenv,       hook_lua_setfenv,       ADDR_LUA_SETFENV,       "lua_setfenv");
    INSTALL(lua_getfenv_fn,     orig_lua_getfenv,       hook_lua_getfenv,       ADDR_LUA_GETFENV,       "lua_getfenv");

    #undef INSTALL

    Log("[LuaStackFast] %d/16 hooks installed", installed);
    return installed > 0;
}

void ShutdownLuaStackFast() {
    MH_DisableHook(ADDR_PUSHNIL);
    MH_DisableHook(ADDR_PUSHINTEGER);
    MH_DisableHook(ADDR_PUSHBOOLEAN);
    MH_DisableHook(ADDR_PUSHLIGHTUSERDATA);
    MH_DisableHook(ADDR_LUA_TYPE);
    MH_DisableHook(ADDR_LUA_ISFUNCTION);
    MH_DisableHook(ADDR_LUA_ISSTRING);
    MH_DisableHook(ADDR_LUA_TOTHREAD);
    MH_DisableHook(ADDR_LUA_REMOVE);
    MH_DisableHook(ADDR_LUA_INSERT);
    MH_DisableHook(ADDR_LUA_REPLACE);
    MH_DisableHook(ADDR_LUA_TOUSERDATA);
    MH_DisableHook(ADDR_LUA_GETMETATABLE);
    MH_DisableHook(ADDR_LUA_SETMETATABLE);
    MH_DisableHook(ADDR_LUA_SETFENV);
    MH_DisableHook(ADDR_LUA_GETFENV);
}