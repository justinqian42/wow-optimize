#include <windows.h>
#include <cstdint>
#include "MinHook.h"
#include "version.h"
#include "lua_optimize.h"
#include "font_metrics_fast.h"

extern "C" void Log(const char* fmt, ...);

#define TAINT_CELL ( *(uint32_t**)0x00D4139C )

static inline bool IsTeardownState() {
    uintptr_t gL = *(uintptr_t*)0x00D3F78C;
    return (gL < 0x10000 || gL > 0xFFE00000);
}

static __forceinline bool IsValidPtr(uintptr_t p) {
    return p > 0x10000 && p < 0xFFE00000;
}

static __forceinline bool IsWowCodePtr(uintptr_t p) {
    return p >= 0x00401000 && p < 0x009DF000;
}

static __forceinline bool IsWowDataPtr(uintptr_t p) {
    return p >= 0x009DF000 && p < 0x00DD1000;
}

static __forceinline uintptr_t ResolveTValue(uintptr_t L, int idx, bool* deferToOrig) {
    *deferToOrig = false;
    if (idx > 0) {
        uintptr_t base = *(uintptr_t*)(L + 0x10);
        if (!IsValidPtr(base)) return 0;
        uintptr_t tv = base + (uintptr_t)(idx - 1) * 16;
        uintptr_t top = *(uintptr_t*)(L + 0x0C);
        if (tv >= top) return 0;
        return tv;
    }
    if (idx < 0 && idx > -10000) {
        uintptr_t top = *(uintptr_t*)(L + 0x0C);
        if (!IsValidPtr(top)) return 0;
        uintptr_t tv = top + (uintptr_t)idx * 16;
        uintptr_t base = *(uintptr_t*)(L + 0x10);
        if (tv < base) return 0;
        return tv;
    }
    *deferToOrig = true;
    return 0;
}

static __forceinline void* GetCFrameFromLuaTable(uintptr_t L, int idx) {
    bool defer = false;
    uintptr_t tv = ResolveTValue(L, idx, &defer);
    if (defer || !tv) return nullptr;

    if (*(int*)(tv + 8) != 5) return nullptr; // LUA_TTABLE
    uintptr_t t = *(uintptr_t*)(tv + 0);
    if (!IsValidPtr(t)) return nullptr;

    uintptr_t nodes = *(uintptr_t*)(t + 0x14); // t->node
    if (!IsValidPtr(nodes)) return nullptr;

    // lsizenode is at +11 and a Node is 40 bytes. Both were wrong here - +9 and
    // 32 - which is the stock Lua 5.1 layout rather than WoW's. The engine's own
    // luaH_getstr settles it: it reads the size byte from [table+0Bh] and indexes
    // the node array with lea eax,[eax+eax*4] / lea eax,[edx+eax*8], i.e. idx*40.
    // ui_accessor_fast.cpp does the identical walk with the right constants; this
    // copy of it did not.
    //
    // The effect is worse than a failed lookup. A wrong size gives a wrong loop
    // bound and a wrong stride reads every node after the first at a misaligned
    // address, so the scan can land on a userdata belonging to a different font
    // string, pass the type check, and return it. The caller then measures that
    // object instead - a real width, for the wrong text. Text laid out with
    // another string's width is text drawn on top of itself.
    int size = 1 << *(unsigned char*)(t + 11); // 1 << t->lsizenode
    for (int i = 0; i < size; i++) {
        uintptr_t node = nodes + i * 40; // sizeof(Node) is 40 in WoW's Lua
        int key_tt = *(int*)(node + 24); // TKey.tt
        if (key_tt == 3) { // LUA_TNUMBER
            double key_val = *(double*)(node + 16); // TKey.value
            if (key_val == 0.0) {
                int val_tt = *(int*)(node + 8); // TValue.tt
                if (val_tt == 7) { // LUA_TUSERDATA
                    uintptr_t udata = *(uintptr_t*)(node + 0); // TValue.value
                    if (IsValidPtr(udata)) {
                        return (void*)(udata + 24);
                    }
                }
                break;
            }
        }
    }
    return nullptr;
}

static __forceinline bool ValidateObjectType(void* obj, int typeId) {
    uintptr_t pObj = (uintptr_t)obj;
    if (pObj < 0x10000 || pObj > 0xFFE00000) return false;

    uintptr_t vtable = *(uintptr_t*)obj;
    if (!IsWowDataPtr(vtable)) return false;

    uintptr_t is_type_fn = *(uintptr_t*)(vtable + 16);
    if (!IsWowCodePtr(is_type_fn)) return false;

    return ((bool (__thiscall*)(void*, int))is_type_fn)(obj, typeId);
}

// Stats
static volatile long g_getstringwidth_calls = 0;
static volatile long g_getstringheight_calls = 0;

// TypeId pointer
static const uintptr_t ADDR_FONTSTRING_TYPEID = 0x00B4792C;

// Lock-free Font String Cache with frame-based lazy invalidation
struct FontStringCacheEntry {
    void* obj;
    const char* text; // Pointer to the internal text buffer (at obj+244)
    uint32_t textHash; // Contents of that buffer, not just where it lives
    double width;
    double height;
    uint32_t frame;
    bool hasWidth;
    bool hasHeight;
};

// The entry used to be validated by comparing the text POINTER. WoW reuses a
// font string's buffer and rewrites it in place, so the pointer stays identical
// across a SetText and the comparison passes for a string that no longer says
// the same thing - handing back the previous string's width. A wrong width is
// exactly how text ends up drawn on top of itself.
//
// Hashing the first 64 bytes plus the length costs a fraction of the real
// measurement call this cache exists to skip.
static inline uint32_t HashText(const char* s) {
    if (!s) return 0;
    uint32_t h = 2166136261u;
    int i = 0;
    for (; i < 64 && s[i]; i++) {
        h ^= (unsigned char)s[i];
        h *= 16777619u;
    }
    while (s[i]) i++;              // length matters: "abc" vs "abcabc..."
    h ^= (uint32_t)i;
    h *= 16777619u;
    return h;
}

static constexpr int FONT_CACHE_SIZE = 2048;
static constexpr int FONT_CACHE_MASK = FONT_CACHE_SIZE - 1;
static FontStringCacheEntry g_fontStringCache[FONT_CACHE_SIZE] = {};
static uint32_t g_fontCacheFrame = 0;

void FontMetrics_OnFrame() {
    g_fontCacheFrame++;
}

static inline uint32_t HashObj(void* obj) {
    uintptr_t val = (uintptr_t)obj;
    return (uint32_t)((val ^ (val >> 12)) & FONT_CACHE_MASK);
}

static void InvalidateFontStringCache(void* obj) {
    uint32_t idx = HashObj(obj);
    if (g_fontStringCache[idx].obj == obj) {
        g_fontStringCache[idx].obj = nullptr;
        g_fontStringCache[idx].text = nullptr;
        g_fontStringCache[idx].textHash = 0;
        g_fontStringCache[idx].hasWidth = false;
        g_fontStringCache[idx].hasHeight = false;
        g_fontStringCache[idx].frame = 0;
    }
}

typedef int (__fastcall *InvalidateMetrics_fn)(void* ecx, int edx);
static InvalidateMetrics_fn orig_InvalidateMetrics = nullptr;

static int __fastcall Hooked_InvalidateMetrics(void* ecx, int edx) {
    #if !TEST_DISABLE_FONT_METRICS_LOCK_FREE
    InvalidateFontStringCache(ecx);
    #endif
    return orig_InvalidateMetrics(ecx, 0);
}

// Functions
static const uintptr_t ADDR_FONT_GETWIDTH  = 0x00483E80;
static const uintptr_t ADDR_FONT_GETHEIGHT = 0x00483FA0;

typedef double (__thiscall* GetWidth_t)(void* obj);
static const GetWidth_t orig_GetWidth = (GetWidth_t)ADDR_FONT_GETWIDTH;

typedef double (__thiscall* GetHeight_t)(void* obj);
static const GetHeight_t orig_GetHeight = (GetHeight_t)ADDR_FONT_GETHEIGHT;

typedef double (*sub_47BFE0_t)();
static const sub_47BFE0_t orig_sub_47BFE0 = (sub_47BFE0_t)0x0047BFE0;

typedef double (*sub_47C050_t)(float a1);
static const sub_47C050_t orig_sub_47C050 = (sub_47C050_t)0x0047C050;

typedef int (__cdecl* lua_pushnumber_t)(uintptr_t L, double n);
static const lua_pushnumber_t pushnumber_fn = (lua_pushnumber_t)0x0084E2A0;

// 1. GetStringWidth — 0x0048DE90
typedef int (__cdecl* GetStringWidth_t)(uintptr_t L);
static GetStringWidth_t orig_GetStringWidth = nullptr;

static int __cdecl hook_GetStringWidth(uintptr_t L) {
    if (IsTeardownState() || LuaOpt::IsReloading() || LuaOpt::IsSwapping() || LuaOpt::IsLoadingMode()) 
        return orig_GetStringWidth(L);

    ++g_getstringwidth_calls;
    __try {
        int typeId = *(int*)ADDR_FONTSTRING_TYPEID;
        if (typeId != 0) {
            void* obj = GetCFrameFromLuaTable(L, 1);
            if (obj && ValidateObjectType(obj, typeId)) {
                const char* text = *(const char**)((char*)obj + 244);
                #if !TEST_DISABLE_FONT_METRICS_LOCK_FREE
                uint32_t idx = HashObj(obj);
                uint32_t th = HashText(text);
                if (g_fontStringCache[idx].obj == obj && 
                    g_fontStringCache[idx].text == text &&
                    g_fontStringCache[idx].textHash == th &&
                    g_fontStringCache[idx].frame == g_fontCacheFrame && 
                    g_fontStringCache[idx].hasWidth) {
                    pushnumber_fn(L, g_fontStringCache[idx].width);
                    return 1;
                }
                #endif

                double w = orig_GetWidth(obj);
                double scale = orig_sub_47BFE0();
                double v3 = scale * 1024.0 * w;
                double final_w = orig_sub_47C050((float)v3);

                #if !TEST_DISABLE_FONT_METRICS_LOCK_FREE
                g_fontStringCache[idx].obj = obj;
                g_fontStringCache[idx].text = text;
                g_fontStringCache[idx].textHash = th;
                g_fontStringCache[idx].width = final_w;
                g_fontStringCache[idx].frame = g_fontCacheFrame;
                g_fontStringCache[idx].hasWidth = true;
                #endif

                pushnumber_fn(L, final_w);
                return 1;
            }
        }
    } __except(EXCEPTION_EXECUTE_HANDLER) {}
    return orig_GetStringWidth(L);
}

// 2. GetStringHeight — 0x0048DF00
typedef int (__cdecl* GetStringHeight_t)(uintptr_t L);
static GetStringHeight_t orig_GetStringHeight = nullptr;

static int __cdecl hook_GetStringHeight(uintptr_t L) {
    if (IsTeardownState() || LuaOpt::IsReloading() || LuaOpt::IsSwapping() || LuaOpt::IsLoadingMode()) 
        return orig_GetStringHeight(L);

    ++g_getstringheight_calls;
    __try {
        int typeId = *(int*)ADDR_FONTSTRING_TYPEID;
        if (typeId != 0) {
            void* obj = GetCFrameFromLuaTable(L, 1);
            if (obj && ValidateObjectType(obj, typeId)) {
                const char* text = *(const char**)((char*)obj + 244);
                #if !TEST_DISABLE_FONT_METRICS_LOCK_FREE
                uint32_t idx = HashObj(obj);
                uint32_t th = HashText(text);
                if (g_fontStringCache[idx].obj == obj && 
                    g_fontStringCache[idx].text == text &&
                    g_fontStringCache[idx].textHash == th &&
                    g_fontStringCache[idx].frame == g_fontCacheFrame && 
                    g_fontStringCache[idx].hasHeight) {
                    pushnumber_fn(L, g_fontStringCache[idx].height);
                    return 1;
                }
                #endif

                double h = orig_GetHeight(obj);
                double scale = orig_sub_47BFE0();
                double v5 = scale * 1024.0 * h;
                double final_h = orig_sub_47C050((float)v5);

                #if !TEST_DISABLE_FONT_METRICS_LOCK_FREE
                g_fontStringCache[idx].obj = obj;
                g_fontStringCache[idx].text = text;
                g_fontStringCache[idx].textHash = th;
                g_fontStringCache[idx].height = final_h;
                g_fontStringCache[idx].frame = g_fontCacheFrame;
                g_fontStringCache[idx].hasHeight = true;
                #endif

                pushnumber_fn(L, final_h);
                return 1;
            }
        }
    } __except(EXCEPTION_EXECUTE_HANDLER) {}
    return orig_GetStringHeight(L);
}

// Install / Shutdown
bool InstallFontMetricsFast() {
    #if TEST_DISABLE_FONT_METRICS_FAST
    Log("[FontMetricsFast] DISABLED via TEST_DISABLE_FONT_METRICS_FAST");
    return false;
    #else
    int installed = 0;
    
    #define INSTALL(fn_t, orig_ptr, hook_ptr, addr, name) \
        if (WineSafe_CreateHook((void*)(addr), (void*)(hook_ptr), (void**)&(orig_ptr)) == MH_OK) { \
            if (WO_EnableHook((void*)(addr)) == MH_OK) { \
                installed++; \
                Log("[FontMetricsFast] Hooked " name " at 0x%08X", (DWORD)(addr)); \
            } else { \
                Log("[FontMetricsFast] Enable " name " FAILED"); \
            } \
        } else { \
            Log("[FontMetricsFast] Create " name " FAILED"); \
        }

    INSTALL(GetStringWidth_t,  orig_GetStringWidth,  hook_GetStringWidth,  0x0048DE90, "GetStringWidth");
    INSTALL(GetStringHeight_t, orig_GetStringHeight, hook_GetStringHeight, 0x0048DF00, "GetStringHeight");
    
    #if !TEST_DISABLE_FONT_METRICS_LOCK_FREE
    INSTALL(InvalidateMetrics_fn, orig_InvalidateMetrics, Hooked_InvalidateMetrics, 0x00482E10, "InvalidateMetrics");
    #endif

    #undef INSTALL

    Log("[FontMetricsFast] %d/2 hooks installed", installed);
    return installed > 0;
    #endif
}

void ShutdownFontMetricsFast() {
    #if !TEST_DISABLE_FONT_METRICS_FAST
    MH_DisableHook((void*)0x0048DE90);
    MH_DisableHook((void*)0x0048DF00);
    Log("[FontMetricsFast] Stats: GetStringWidth=%ld GetStringHeight=%ld",
        g_getstringwidth_calls, g_getstringheight_calls);
    #endif
}

extern "C" void FontMetrics_GetStats(long* widthCalls, long* heightCalls) {
    if (widthCalls) *widthCalls = g_getstringwidth_calls;
    if (heightCalls) *heightCalls = g_getstringheight_calls;
}
