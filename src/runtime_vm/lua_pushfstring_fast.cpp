#include <windows.h>
#include <cstdint>
#include "MinHook.h"
#include "version.h"
#include "crash_dumper.h"

extern "C" void Log(const char* fmt, ...);

// lua_pushfstring at 0x84E3D0 — format string push.
// Fast path: "%s" single-string arg — skip sprintf, directly push the string
// via lua_pushstring. Common under ElvUI/WeakAuras for wrapping single strings.

typedef const char*(__cdecl *pushfstring_fn)(uintptr_t L, const char* fmt, ...);
static pushfstring_fn orig = nullptr;
static volatile long g_hits = 0, g_misses = 0;

static const char* __cdecl hook(
    uintptr_t L, const char* fmt,
    uintptr_t a3, uintptr_t a4, uintptr_t a5, uintptr_t a6, uintptr_t a7, uintptr_t a8
) {
    if (L < 0x10000 || L > 0xFFE00000) {
        g_misses++;
        return orig(L, fmt, a3, a4, a5, a6, a7, a8);
    }

    if (fmt && fmt[0] == '%' && fmt[1] == 's' && fmt[2] == '\0') {
        const char* s1 = (const char*)a3;
        if (s1 && (uintptr_t)s1 > 0x10000) {
            __try {
                // lua_pushstring (0x84E350) interns s1 and returns the new TString*.
                // (The previous code called lua_pushlstring at 0x84E300, which needs
                // an explicit length — passing only two args left len as stack
                // garbage.) lua_pushfstring must return the interned char* data,
                // which lives at TString+20.
                typedef uintptr_t(__cdecl *pushstr_fn)(uintptr_t, const char*);
                uintptr_t ts = ((pushstr_fn)0x0084E350)(L, s1);
                if (ts > 0x10000) {
                    g_hits++;
                    return (const char*)(ts + 20);
                }
            } __except (EXCEPTION_EXECUTE_HANDLER) {}
        }
    }

    g_misses++;
    return orig(L, fmt, a3, a4, a5, a6, a7, a8);
}

bool InstallLuaPushfstringFast() {
    void* t = (void*)0x0084E3D0;
    if (MH_CreateHook(t, hook, (void**)&orig) != MH_OK) return false;
    WO_EnableHook(t);
    Log("[PushFStr] ACTIVE — lua_pushfstring inline at 0x84E3D0");
    CrashDumper::RegisterFeature("PushFStr");
    CrashDumper::FeatureSetActive("PushFStr", true);
    return true;
}
