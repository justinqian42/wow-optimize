#include "event_dispatch_cache.h"
#include <windows.h>
#include <cstdint>
#include <vector>
#include <unordered_map>
#include "MinHook.h"
#include "version.h"
#include "lua_optimize.h"
#include "crash_dumper.h"

extern "C" void Log(const char* fmt, ...);

// Statistics
static volatile long g_cacheCalls = 0;
static volatile long g_cacheHits  = 0;
static volatile long g_cacheMisses = 0;

struct EventCacheEntry {
    bool valid = false;
    std::vector<int> ref_ids;
};

// Keyed by event_ptr (event_structure_base)
static std::unordered_map<uintptr_t, EventCacheEntry> g_eventCache;

static inline bool IsValidPtr(uintptr_t p) {
    return p > 0x10000 && p < 0xFFE00000;
}

static __forceinline uintptr_t ResolveIndex(uintptr_t L, int idx) {
    if (idx > 0) {
        uintptr_t base = *(uintptr_t*)(L + 0x10);
        if (!IsValidPtr(base)) return 0;
        uintptr_t tv = base + (uintptr_t)(idx - 1) * 16;
        uintptr_t top = *(uintptr_t*)(L + 0x0C);
        if (tv >= top) return 0;
        return tv;
    }
    if (idx < 0 && idx > -10000) { // LUA_REGISTRYINDEX
        uintptr_t top = *(uintptr_t*)(L + 0x0C);
        if (!IsValidPtr(top)) return 0;
        uintptr_t tv = top + (uintptr_t)idx * 16;
        uintptr_t base = *(uintptr_t*)(L + 0x10);
        if (tv < base) return 0;
        return tv;
    }
    return 0;
}

static void InvalidateCache(uintptr_t event_ptr) {
    auto it = g_eventCache.find(event_ptr);
    if (it != g_eventCache.end()) {
        it->second.valid = false;
        it->second.ref_ids.clear();
    }
}

// ----------------------------------------------------------------
// Hook: sub_81A790 (EventRegisterNode)
// ----------------------------------------------------------------
typedef uintptr_t (__cdecl *EventRegister_t)(uintptr_t frame, uintptr_t event_ptr);
static EventRegister_t orig_EventRegister = nullptr;

static uintptr_t __cdecl Hooked_EventRegister(uintptr_t frame, uintptr_t event_ptr) {
    CrashDumper::RecordHookCall("EventDispatchCache_Register", (uintptr_t)event_ptr);
    uintptr_t res = orig_EventRegister(frame, event_ptr);
    if (event_ptr > 0x10000 && event_ptr < 0xFFE00000) {
        InvalidateCache(event_ptr);
    }
    return res;
}

// ----------------------------------------------------------------
// Hook: sub_81A8C0 (EventUnregisterNode)
// ----------------------------------------------------------------
typedef uintptr_t (__cdecl *EventUnregister_t)(uintptr_t frame, uintptr_t event_ptr);
static EventUnregister_t orig_EventUnregister = nullptr;

static uintptr_t __cdecl Hooked_EventUnregister(uintptr_t frame, uintptr_t event_ptr) {
    CrashDumper::RecordHookCall("EventDispatchCache_Unregister", (uintptr_t)event_ptr);
    uintptr_t res = orig_EventUnregister(frame, event_ptr);
    if (event_ptr > 0x10000 && event_ptr < 0xFFE00000) {
        InvalidateCache(event_ptr);
    }
    return res;
}

// ----------------------------------------------------------------
// Hook: sub_81BE70 (GetFramesRegisteredForEvent)
// ----------------------------------------------------------------
typedef int (__cdecl *GetFramesRegistered_t)(uintptr_t L);
static GetFramesRegistered_t orig_GetFramesRegistered = nullptr;

static int __cdecl Hooked_GetFramesRegistered(uintptr_t L) {
    CrashDumper::RecordHookCall("EventDispatchCache", (uintptr_t)L);
    ++g_cacheCalls;

    // Always use the original function. The previous cache fastpath was
    // missing luaD_checkstack and frame visibility validation, causing
    // Lua stack overflow and stale ref_id pushes that broke addon
    // OnEvent handlers (e.g., welcome text during PLAYER_ENTERING_WORLD).
    return orig_GetFramesRegistered(L);
}

// Install / Uninstall
bool InstallEventDispatchCache()
{
    // Verify target addresses
    unsigned char* p1 = (unsigned char*)0x0081BE70;
    unsigned char* p2 = (unsigned char*)0x0081A790;
    unsigned char* p3 = (unsigned char*)0x0081A8C0;

    if (p1[0] != 0x55 || p1[1] != 0x8B ||
        p2[0] != 0x55 || p2[1] != 0x8B ||
        p3[0] != 0x55 || p3[1] != 0x8B) {
        Log("[EventDispatchCache] BAD PROLOGUEs (expected 55 8B)");
        return false;
    }

    MH_STATUS st = WineSafe_CreateHook(
        (void*)0x0081BE70,
        (void*)Hooked_GetFramesRegistered,
        (void**)&orig_GetFramesRegistered);
    if (st != MH_OK) return false;

    st = WineSafe_CreateHook(
        (void*)0x0081A790,
        (void*)Hooked_EventRegister,
        (void**)&orig_EventRegister);
    if (st != MH_OK) {
        MH_RemoveHook((void*)0x0081BE70);
        return false;
    }

    st = WineSafe_CreateHook(
        (void*)0x0081A8C0,
        (void*)Hooked_EventUnregister,
        (void**)&orig_EventUnregister);
    if (st != MH_OK) {
        MH_RemoveHook((void*)0x0081BE70);
        MH_RemoveHook((void*)0x0081A790);
        return false;
    }

    WO_EnableHook((void*)0x0081BE70);
    WO_EnableHook((void*)0x0081A790);
    WO_EnableHook((void*)0x0081A8C0);

    g_eventCache.clear();

    Log("[EventDispatchCache] ACTIVE (GetFramesRegisteredForEvent cached, register/unregister hooks active)");
    return true;
}

void UninstallEventDispatchCache()
{
    MH_DisableHook((void*)0x0081BE70);
    MH_DisableHook((void*)0x0081A790);
    MH_DisableHook((void*)0x0081A8C0);

    MH_RemoveHook((void*)0x0081BE70);
    MH_RemoveHook((void*)0x0081A790);
    MH_RemoveHook((void*)0x0081A8C0);

    g_eventCache.clear();

    LONG64 total  = g_cacheCalls;
    LONG64 hits   = g_cacheHits;
    LONG64 misses = g_cacheMisses;
    if (total > 0) {
        Log("[EventDispatchCache] Stats: %lld calls, %lld hits (%.1f%%), %lld misses",
            total, hits, 100.0 * (double)hits / (double)total, misses);
    }
}

static bool g_preWarmDone = false;

void ClearEventDispatchCache()
{
    for (auto& pair : g_eventCache) {
        pair.second.valid = false;
        pair.second.ref_ids.clear();
    }
    g_preWarmDone = false;
}

void PreWarmEventDispatchCache()
{
    if (g_preWarmDone) return;
    g_preWarmDone = true;

    static const char* hotEvents[] = {
        "PLAYER_ENTERING_WORLD",
        "PLAYER_LEAVING_WORLD",
        "PLAYER_TARGET_CHANGED",
        "PLAYER_REGEN_DISABLED",
        "PLAYER_REGEN_ENABLED",
        "UNIT_HEALTH",
        "UNIT_POWER",
        "UNIT_MAXHEALTH",
        "UNIT_MAXPOWER",
        "UNIT_AURA",
        "UNIT_NAME_UPDATE",
        "COMBAT_LOG_EVENT_UNFILTERED",
        "COMBAT_TEXT_UPDATE",
        "BAG_UPDATE",
        "UNIT_INVENTORY_CHANGED",
        "ITEM_LOCK_CHANGED", // vendor-sell/lock-state notifications drive the greyed-out bag icon; GitHub issue #34
        "ACTIONBAR_SLOT_CHANGED",
        "UPDATE_BINDINGS",
        "PARTY_MEMBERS_CHANGED",
        "RAID_ROSTER_UPDATE",
        "CHAT_MSG_ADDON",
    };
    static constexpr int hotEventCount = sizeof(hotEvents) / sizeof(hotEvents[0]);

    typedef uintptr_t (__cdecl *EventResolve_t)(const char* name);
    EventResolve_t resolve = (EventResolve_t)0x0081B510;

    int warmed = 0;
    __try {
        for (int i = 0; i < hotEventCount; ++i) {
            const char* event_name = hotEvents[i];
            uintptr_t v3 = resolve(event_name);
            if (v3 < 0x10000 || v3 > 0xFFE00000) continue;

            uintptr_t event_ptr = v3 - 24;

            auto& entry = g_eventCache[event_ptr];
            if (entry.valid) continue;  // already cached by lazy miss path

            entry.ref_ids.clear();
            uintptr_t list_head = *(uintptr_t*)(event_ptr + 32);
            while ((list_head & 1) == 0 && list_head) {
                uintptr_t frame = *(uintptr_t*)(list_head + 8);
                if (frame > 0x10000 && frame < 0xFFE00000) {
                    int ref_id = *(int*)(frame + 8);
                    entry.ref_ids.push_back(ref_id);
                }
                list_head = *(uintptr_t*)(list_head + 4);
            }
            entry.valid = true;
            ++warmed;
        }
    } __except(EXCEPTION_EXECUTE_HANDLER) {}

    if (warmed > 0) {
        Log("[EventDispatchCache] Pre-warmed %d hot event caches", warmed);
    }
}