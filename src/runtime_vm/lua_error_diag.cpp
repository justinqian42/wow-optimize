#include <windows.h>
#include <cstdint>
#include "MinHook.h"
#include "version.h"
#include "crash_dumper.h"

extern "C" void Log(const char* fmt, ...);
extern void LogFlushImmediate();

// lua_error: 0x84EF30 = sub_84EF30. Disassembly-verified: __cdecl __noreturn sub_84EF30(_DWORD *a1)
// { sub_850830(a1); } — directly calls luaD_throw. 12 bytes, prologue: 55 8B EC.
// The old address 0x84F610 was sub_84F610(size_t Size) = luaL_addvalue, NOT lua_error.
// Using 0x84F610 caused all error reads to show <unable to read> (arg is size_t, not L).

// Max unhandled errors to log before stopping (prevents recursive error flooding)
#define MAX_DIAG_ERRORS 50

// Max unhandled errors that additionally carry the full feature/hook-state snapshot
#define MAX_FULL_STATE_DUMPS 3

typedef int(__cdecl *lua_error_t)(uintptr_t L);
static lua_error_t s_origLuaError = nullptr;
static volatile LONG g_errorCount = 0;

static const char* ReadLuaErrorString(uintptr_t L) {
    __try {
        uintptr_t top = *(uintptr_t*)(L + 0x0C);
        if (top < 0x10000 || top > 0xFFE00000) return nullptr;
        uintptr_t tv = top - 16;
        if (tv < 0x10000 || tv > 0xFFE00000) return nullptr;
        uint32_t tt = *(uint32_t*)(tv + 8);
        if (tt != 4) return nullptr;
        uintptr_t ts = *(uintptr_t*)tv;
        if (ts < 0x10000 || ts > 0xFFE00000) return nullptr;
        size_t len = *(size_t*)(ts + 16);
        if (len > 4000) len = 4000;
        return (const char*)(ts + 20);
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}

typedef int (__cdecl *lua_getstack_fn)(uintptr_t L, int level, void* ar);
typedef int (__cdecl *lua_getinfo_fn)(uintptr_t L, const char* what, void* ar);

// The innermost Lua frame that has a source, as "file:line". That is what names
// the addon behind a caught error, and it is the one thing the message itself
// never says.
static void DescribeTop(uintptr_t L, char* out, int outSize) {
    out[0] = 0;
    if (!L) return;
    __try {
        lua_getstack_fn getstack = (lua_getstack_fn)0x0084FE40;
        lua_getinfo_fn  getinfo  = (lua_getinfo_fn)0x00850A90;
        char ar[100];
        for (int level = 0; level < 4; ++level) {
            memset(ar, 0, sizeof(ar));
            if (getstack(L, level, ar) != 1) return;
            getinfo(L, "Sl", ar);
            const char* shortSrc = (const char*)(ar + 36);
            const int line = *(int*)(ar + 20);
            if (shortSrc[0] && shortSrc[0] != '?' && line > 0) {
                wsprintfA(out, "%.*s:%d", outSize - 12, shortSrc, line);
                return;
            }
        }
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        out[0] = 0;
    }
}

// Caught errors, kept one line each however often they repeat.
//
// A caught error is one a pcall around it handles, so the game carries on. They
// are almost all an addon asking whether an API exists by calling it: a tester
// asked about "Usage: GetPlayerInfoByGUID(\"playerGUID\")" arriving over and
// over, and the same logs carry "CreateFrame: Unknown frame type 'TextureBase'"
// and "Couldn't find CVar named 'HelpTipEnabled'" - calls into a later client's
// API that this one does not have.
//
// Nothing is wrong, but a line per occurrence buries the rest of the log and
// gives the reader nothing to act on. So the first sighting of each message
// carries the Lua file and line that raised it, which names the addon, and
// every repeat after that is counted and printed once in the periodic report.
struct CaughtError {
    uint32_t hash;
    uint32_t count;
    char     msg[112];
    char     where[72];
};
#define MAX_CAUGHT_KINDS 48
static CaughtError g_caught[MAX_CAUGHT_KINDS];
static int  g_caughtKinds = 0;
static unsigned long g_caughtTotal = 0;
static unsigned long g_caughtDropped = 0;

static uint32_t HashMsg(const char* s) {
    uint32_t h = 2166136261u;
    for (int i = 0; s[i] && i < 160; ++i) { h ^= (unsigned char)s[i]; h *= 16777619u; }
    return h;
}

static void LogLuaTraceback(uintptr_t L) {
    if (!L) return;
    
    auto getstack = (lua_getstack_fn)0x0084FE40;
    auto getinfo = (lua_getinfo_fn)0x00850A90;

    char ar[100];
    memset(ar, 0, sizeof(ar));
    int level = 0;
    
    LogEx(LOG_LEVEL_ERROR, "LUA", "  Traceback:");
    while (getstack(L, level, ar) == 1) {
        getinfo(L, "nSl", ar);
        
        const char* name = *(const char**)(ar + 4);
        const char* namewhat = *(const char**)(ar + 8);
        const char* what = *(const char**)(ar + 12);
        const char* source = *(const char**)(ar + 16);
        int currentline = *(int*)(ar + 20);
        const char* short_src = (const char*)(ar + 36);
        
        if (!name) name = "?";
        if (!short_src) short_src = "?";
        if (!what) what = "?";
        
        LogEx(LOG_LEVEL_ERROR, "LUA", "    [%d] %s:%d in function '%s' (%s)",
            level, short_src, currentline, name, what);
            
        level++;
        if (level > 20) {
            LogEx(LOG_LEVEL_ERROR, "LUA", "    ... (truncated)");
            break;
        }
    }
}

static int __cdecl DiagLuaError(uintptr_t L) {
    if (g_errorCount >= MAX_DIAG_ERRORS) return s_origLuaError(L);

    uintptr_t useL = L;
    if (L < 0x10000 || L > 0xFFE00000) {
        useL = *(uintptr_t*)0x00D3F78C;
        if (useL < 0x10000 || useL > 0xFFE00000) useL = 0;
    }

    bool isCaught = false;
    if (useL) {
        __try {
            // L->errorJmp lives at +0x70 in WoW's Lua 5.1 lua_State layout.
            // Disassembly-verified against luaD_throw (sub_8562E0):
            //   v2 = *(_DWORD *)(L + 112); if (v2) { *(v2 + 68) = status; longjmp(v2 + 4, 1); }
            // The previous offset 0x74 was L->errfunc, read by luaG_errormsg
            // (sub_850830) as `a1[29]`. errfunc is 0 for every ordinary pcall, so
            // every *caught* error was misclassified as unhandled and triggered the
            // full multi-line diagnostic dump below.
            uintptr_t errorJmp = *(uintptr_t*)(useL + 0x70);
            if (errorJmp >= 0x10000 && errorJmp <= 0xFFE00000) {
                isCaught = true;
            }
        } __except(EXCEPTION_EXECUTE_HANDLER) {}
    }

    const char* errMsg = nullptr;
    if (useL) {
        errMsg = ReadLuaErrorString(useL);
    }
    if (!errMsg) errMsg = "<unable to read>";

    if (isCaught) {
        // Handled by the surrounding pcall - the game recovers on its own. One
        // INFO line per distinct message, with no synchronous disk flush, so an
        // addon probing the API in a loop cannot stall the main thread.
        ++g_caughtTotal;
        const uint32_t h = HashMsg(errMsg);
        for (int i = 0; i < g_caughtKinds; ++i) {
            if (g_caught[i].hash == h) { ++g_caught[i].count; return s_origLuaError(L); }
        }
        if (g_caughtKinds >= MAX_CAUGHT_KINDS) {
            ++g_caughtDropped;
            return s_origLuaError(L);
        }
        CaughtError& e = g_caught[g_caughtKinds++];
        e.hash = h;
        e.count = 1;
        lstrcpynA(e.msg, errMsg, sizeof(e.msg));
        e.where[0] = 0;
        // Only on a first sighting, so a message arriving thousands of times
        // costs one walk and not thousands.
        DescribeTop(useL, e.where, sizeof(e.where));
        LogEx(LOG_LEVEL_INFO, "LUA",
              "[LuaError] Caught exception: %s%s%s. A pcall around it handled it, so "
              "nothing broke; later occurrences of this one are counted and reported "
              "rather than repeated here.",
              e.msg, e.where[0] ? " - raised from " : "", e.where);
        return s_origLuaError(L);
    }

    LONG errNum = InterlockedIncrement(&g_errorCount);
    if (errNum > MAX_DIAG_ERRORS) return s_origLuaError(L);

    LogEx(LOG_LEVEL_ERROR, "LUA", "=== UNHANDLED LUA ERROR #%d ===", (int)errNum);
    LogEx(LOG_LEVEL_ERROR, "LUA", "  lua_State param: 0x%08X  global: 0x%08X", (unsigned)L, (unsigned)useL);
    LogEx(LOG_LEVEL_ERROR, "LUA", "  Message: %s", errMsg);

    if (useL) {
        LogLuaTraceback(useL);
    }

    CrashDumper::Trace("LUA error: %.80s", errMsg);

    // Only the first few errors carry the state snapshot. Every ERROR line is
    // written through to disk, so repeating the snapshot per error costs real
    // main-thread time, and consecutive errors report near-identical state.
    if (errNum > MAX_FULL_STATE_DUMPS) {
        LogEx(LOG_LEVEL_ERROR, "LUA", "=== END LUA ERROR #%d (state snapshot omitted) ===", (int)errNum);
        return s_origLuaError(L);
    }

    // What ran before the error, in order. This replaces a full dump of the
    // feature table: FeatureCall() is wired into almost nothing, so that table
    // printed ~70 lines of "calls=0" that said nothing about the failure. Only
    // features that actually recorded activity are worth a line.
    LogEx(LOG_LEVEL_ERROR, "LUA", "  Recent events:");
    CrashDumper::DumpTrace(16);

    LogEx(LOG_LEVEL_ERROR, "LUA", "  Features with recorded activity:");
    FeatureState features[MAX_TRACKED_FEATURES];
    int fcount = CrashDumper::GetFeatureStates(features, MAX_TRACKED_FEATURES);
    int reported = 0;
    for (int i = 0; i < fcount; i++) {
        if (features[i].callCount == 0 && features[i].errorCount == 0) continue;
        LogEx(LOG_LEVEL_ERROR, "LUA", "    %-28s active=%d calls=%lld errors=%lld",
            features[i].name ? features[i].name : "(null)",
            features[i].active ? 1 : 0,
            features[i].callCount,
            features[i].errorCount);
        reported++;
    }
    if (reported == 0) {
        LogEx(LOG_LEVEL_ERROR, "LUA", "    (none)");
    }

    LogEx(LOG_LEVEL_ERROR, "LUA", "  Last 32 hook calls:");
    extern void CrashDumper_DumpHookTrace(int count);
    CrashDumper_DumpHookTrace(32);

    LogEx(LOG_LEVEL_ERROR, "LUA", "=== END LUA ERROR #%d ===", (int)errNum);

    return s_origLuaError(L);
}

// Printed from the periodic report, so a session that never reaches Shutdown
// still says what was caught and how often.
void LuaErrorDiagLogStats() {
    if (!s_origLuaError) {
        Log("[LuaError] not measured: the hook is not installed.");
        return;
    }
    if (g_caughtTotal == 0) {
        Log("[LuaError] measured and zero: no Lua error was caught by a pcall this "
            "session.");
        return;
    }
    Log("[LuaError] %lu error(s) caught by a pcall, of %d distinct message(s). These "
        "are handled where they are raised; the game carries on.",
        g_caughtTotal, g_caughtKinds);
    for (int i = 0; i < g_caughtKinds; ++i) {
        Log("[LuaError]   %6u x  %s%s%s", g_caught[i].count, g_caught[i].msg,
            g_caught[i].where[0] ? "  first raised from " : "", g_caught[i].where);
    }
    if (g_caughtDropped)
        Log("[LuaError]   %lu further error(s) had a message this did not have room "
            "to keep apart and are counted in the total only.", g_caughtDropped);
}

bool InstallLuaErrorDiag() {
    // Disassembly-verified: 0x84EF30 = sub_84EF30 = lua_error. Prologue: 55 8B EC (push ebp; mov ebp,esp).
    // Calls sub_850830 (luaD_throw) directly. __cdecl(lua_State*), __noreturn.
    void* target = (void*)0x0084EF30;
    unsigned char* p = (unsigned char*)target;
    if (p[0] != 0x55 || p[1] != 0x8B) {
        Log("[LuaErrorDiag] BAD PROLOGUE at 0x%08X (expected 55 8B, got %02X %02X) — skipping",
            (uintptr_t)target, p[0], p[1]);
        return false;
    }
    if (MH_CreateHook(target, DiagLuaError, (void**)&s_origLuaError) != MH_OK) return false;
    MH_EnableHook(target);
    Log("[LuaErrorDiag] ACTIVE at 0x84EF30 (lua_error): logging first %d Lua errors", MAX_DIAG_ERRORS);
    CrashDumper::RegisterFeature("LuaErrorDiag");
    CrashDumper::FeatureSetActive("LuaErrorDiag", true);
    return true;
}
