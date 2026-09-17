// ============================================================================
// Description: Batching and incremental parsing of combat log events.
// Safety & Threading: Thread-safe queue, rate-limited processing.
// ============================================================================

#include "combatlog_incremental.h"
#include "version.h"
#include <windows.h>
#include <vector>
#include "win_mutex.h"

extern "C" void Log(const char* fmt, ...);

namespace CombatLogIncremental {

struct StoredEvent {
    uint8_t data[128];
};

static std::vector<StoredEvent> g_eventQueue;
static WinMutex g_queueMutex;
static int g_eventsThisFrame = 0;
static constexpr int MAX_SYNC_EVENTS_PER_FRAME = 16;
static constexpr int MAX_DEFERRED_PROCESS_PER_FRAME = 8;
static void* g_origCombatLogEvent = nullptr;

bool Init() {
    g_origCombatLogEvent = nullptr;
    Log("[CombatLogIncremental] Active - Rate-limited combat log queue initialized.");
    return true;
}

void Shutdown() {
    WinLockGuard lock(g_queueMutex);
    g_eventQueue.clear();
}

typedef int (__thiscall* CombatLogEvent_fn)(void* This, int luaState);

static void SafeCallCombatLogEvent(CombatLogEvent_fn orig, void* data, int luaState) {
    __try {
        orig(data, luaState);
    } __except(EXCEPTION_EXECUTE_HANDLER) {}
}

// lua_gettop / lua_settop for stack cleanup around deferred event calls
typedef int  (__cdecl* lua_gettop_fn)(int L);
typedef void (__cdecl* lua_settop_fn)(int L, int idx);
static const lua_gettop_fn lua_gettop_ = (lua_gettop_fn)0x0084DBD0;
static const lua_settop_fn lua_settop_ = (lua_settop_fn)0x0084DBF0;

// Process deferred events with Lua stack protection.
// Separate function because __try cannot coexist with C++ object unwinding
// (std::vector) in the same function scope (MSVC C2712).
static void ProcessDeferredEvents(CombatLogEvent_fn orig, StoredEvent* events, int count, int luaState) {
    int topBefore = 0;
    __try { topBefore = lua_gettop_(luaState); } __except(EXCEPTION_EXECUTE_HANDLER) {}

    for (int i = 0; i < count; i++) {
        SafeCallCombatLogEvent(orig, events[i].data, luaState);
    }

    // CRITICAL: Restore stack to pre-event state to prevent stack leak.
    // WoW's combat log handler pushes event args onto the Lua stack. When
    // called from our Sleep hook (outside normal dispatch), a Lua error
    // (longjmp caught by SEH) or incomplete cleanup leaves values on the
    // stack. The next frame's lua_gettop(L)!=0 check then fires #134.
    __try { lua_settop_(luaState, topBefore); } __except(EXCEPTION_EXECUTE_HANDLER) {}
}

void OnFrame(int luaState) {
    g_eventsThisFrame = 0;

    if (!g_origCombatLogEvent || luaState < 0x10000 || luaState > 0xFFE00000) return;

    std::vector<StoredEvent> toProcess;
    {
        WinLockGuard lock(g_queueMutex);
        if (g_eventQueue.empty()) return;

        int count = (int)g_eventQueue.size();
        if (count > MAX_DEFERRED_PROCESS_PER_FRAME) {
            count = MAX_DEFERRED_PROCESS_PER_FRAME;
        }
        toProcess.assign(g_eventQueue.begin(), g_eventQueue.begin() + count);
        g_eventQueue.erase(g_eventQueue.begin(), g_eventQueue.begin() + count);
    }

    CombatLogEvent_fn orig = (CombatLogEvent_fn)g_origCombatLogEvent;
    ProcessDeferredEvents(orig, toProcess.data(), (int)toProcess.size(), luaState);
}

bool ShouldDefer(void* This, int luaState, void* orig_func) {
    g_origCombatLogEvent = orig_func;

#if 1
    return false;
#else
    g_eventsThisFrame++;
    if (g_eventsThisFrame <= MAX_SYNC_EVENTS_PER_FRAME) {
        return false;
    }

    // Queue the event and defer it
    WinLockGuard lock(g_queueMutex);
    if (g_eventQueue.size() < 1024) {
        StoredEvent ev;
        memcpy(ev.data, This, 128);
        g_eventQueue.push_back(ev);
    }
    return true;
#endif
}

} // namespace CombatLogIncremental
