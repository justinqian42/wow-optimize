#include <windows.h>
#include <cstdint>
#include <cstring>
#include "MinHook.h"
#include "version.h"
#include "crash_dumper.h"

extern "C" void Log(const char* fmt, ...);

// luaL_loadstring at 0x8547B0 — compile a Lua string chunk.
// Engine: checklstring(arg1) → optlstring(arg2,default=s) → luaL_loadbuffer.
// Fast path: single-entry compile cache keyed by source string hash.
// Falls back to original on cache miss, different string, or complex paths.

typedef int (__cdecl *loadstring_fn)(uintptr_t L);
static loadstring_fn orig = nullptr;
static volatile long g_hits = 0;
static volatile long g_misses = 0;

static uintptr_t g_cachedL = 0;
static uint32_t g_cachedHash = 0;
static uintptr_t g_cachedClosure = 0;
static char g_cachedSource[256] = {0};

static uint32_t hash_str(const char* s) {
    uint32_t h = 5381;
    while (*s) h = ((h << 5) + h) + (unsigned char)*s++;
    return h;
}

typedef const char* (__cdecl *checklstr_fn)(uintptr_t L, int idx, unsigned int* len);
typedef const char* (__cdecl *optlstr_fn)(uintptr_t L, int idx, const char* def, unsigned int* len);

static int __cdecl hook(uintptr_t L) {
    if (L < 0x10000 || L > 0xFFE00000)
        return orig(L);
    __try {
        checklstr_fn checklstr = (checklstr_fn)0x0084F9F0;
        optlstr_fn optlstr = (optlstr_fn)0x0084FA50;
        typedef int (__cdecl *loadbuffer_fn)(uintptr_t L, const char* buf, unsigned int sz, const char* name);
        loadbuffer_fn loadbuffer = (loadbuffer_fn)0x0084F860;
        typedef void (__cdecl *push_result_fn)(uintptr_t L);
        push_result_fn push_result = (push_result_fn)0x0084E280;

        unsigned int len = 0;
        const char* s = checklstr(L, 1, &len);
        if (!s || len == 0 || len >= sizeof(g_cachedSource) - 1) goto fallback;

        const char* name = optlstr(L, 2, s, nullptr);
        uint32_t h = hash_str(s);

        if (L == g_cachedL && h == g_cachedHash && g_cachedSource[0] &&
            len < sizeof(g_cachedSource) - 1 &&
            memcmp(s, g_cachedSource, len) == 0 &&
            g_cachedSource[len] == 0) {
            // Cache disabled: raw Closure* pointer cached without GC validation
            // can become stale (mimalloc recycling). Defer to original.
            goto fallback;
        }

        g_cachedHash = h;
        memcpy(g_cachedSource, s, len);
        g_cachedSource[len] = 0;

        g_misses++;
        int r = orig(L);
        if (r == 1) {
            g_cachedL = L;
            g_cachedClosure = *(uintptr_t*)(*(uintptr_t*)(L + 0x0C) - 16);
        } else {
            g_cachedSource[0] = 0;
        }
        return r;
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
fallback:
    return orig(L);
}

bool InstallLuaLoadStringFast() {
    void* t = (void*)0x008547B0;
    if (MH_CreateHook(t, hook, (void**)&orig) != MH_OK) return false;
    WO_EnableHook(t);
    Log("[LoadStr] ACTIVE — luaL_loadstring inline at 0x8547B0");
    CrashDumper::RegisterFeature("LoadStr");
    CrashDumper::FeatureSetActive("LoadStr", true);
    return true;
}
