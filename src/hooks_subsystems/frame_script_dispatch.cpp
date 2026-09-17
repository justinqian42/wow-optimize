#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <cstdint>
#include <cstring>
#include "MinHook.h"
#include "version.h"
#include "frame_script_dispatch.h"

extern "C" void Log(const char* fmt, ...);

static uint64_t g_dispatch_calls = 0;
static uint64_t g_hash_hits = 0;
static uint64_t g_fallbacks = 0;

// Case-insensitive FNV-1a on ASCII lowercase
static inline uint32_t HashEventNameCI(const char* s) {
    uint32_t h = 0x811C9DC5u;
    unsigned char c;
    while ((c = (unsigned char)*s++) != 0) {
        if (c >= 'A' && c <= 'Z') c += 32;
        h ^= c;
        h *= 0x01000193u;
    }
    return h;
}

enum EventHash : uint32_t {
    H_OnLoad              = 0xA13DC00Cu,
    H_OnSizeChanged       = 0xC47EDA1Fu,
    H_OnUpdate            = 0x8B1C1611u,
    H_OnShow              = 0xCFA9CF69u,
    H_OnHide              = 0x57182150u,
    H_OnEnter             = 0x46011836u,
    H_OnLeave             = 0x94CE84EFu,
    H_OnMouseDown         = 0x16BFB0A7u,
    H_OnMouseUp           = 0x034E2F5Eu,
    H_OnMouseWheel        = 0x820ACDBAu,
    H_OnDragStart         = 0x4A342A9Au,
    H_OnDragStop          = 0xA2CDC0FAu,
    H_OnReceiveDrag       = 0x1FE0878Bu,
    H_OnChar              = 0x01FB83ACu,
    H_OnKeyDown           = 0x988CD17Bu,
    H_OnKeyUp             = 0x135A10BAu,
    H_OnAttributeChanged  = 0x4DA03CE4u,
    H_OnEnable            = 0x55118ECFu,
    H_OnDisable           = 0x17E45BACu,
};

struct HandlerEntry {
    EventHash hash;
    const char* name;
    int offset;
    const char* fmt;
};

static const HandlerEntry g_handlers[] = {
    { H_OnLoad,              "OnLoad",              308, nullptr },
    { H_OnSizeChanged,       "OnSizeChanged",       316, "return function(self,w,h) %s end" },
    { H_OnUpdate,            "OnUpdate",            324, "return function(self,elapsed) %s end" },
    { H_OnShow,              "OnShow",              332, nullptr },
    { H_OnHide,              "OnHide",              340, nullptr },
    { H_OnEnter,             "OnEnter",             348, "return function(self,motion) %s end" },
    { H_OnLeave,             "OnLeave",             356, "return function(self,motion) %s end" },
    { H_OnMouseDown,         "OnMouseDown",         364, "return function(self,button) %s end" },
    { H_OnMouseUp,           "OnMouseUp",           372, "return function(self,button) %s end" },
    { H_OnMouseWheel,        "OnMouseWheel",        380, "return function(self,delta) %s end" },
    { H_OnDragStart,         "OnDragStart",         388, "return function(self,button) %s end" },
    { H_OnDragStop,          "OnDragStop",          396, nullptr },
    { H_OnReceiveDrag,       "OnReceiveDrag",       404, nullptr },
    { H_OnChar,              "OnChar",              412, "return function(self,text) %s end" },
    { H_OnKeyDown,           "OnKeyDown",           420, "return function(self,key) %s end" },
    { H_OnKeyUp,             "OnKeyUp",             428, "return function(self,key) %s end" },
    { H_OnAttributeChanged,  "OnAttributeChanged",  436, "return function(self,name,value) %s end" },
    { H_OnEnable,            "OnEnable",            444, nullptr },
    { H_OnDisable,           "OnDisable",           452, nullptr },
};
static constexpr int NUM_HANDLERS = sizeof(g_handlers) / sizeof(g_handlers[0]);

// sub_816830 — the script registration check the original calls first.
// Must preserve its side effects. Returns nonzero to short-circuit.
// It is a __thiscall, so we use __fastcall to pass 'this' in ECX.
typedef int (__fastcall *fn_816830_t)(void* thisp, void* edx, const char* name, uint32_t* a3);
static fn_816830_t g_fn_816830 = nullptr;

// __thiscall: this=ECX, name=[esp+4], a3=[esp+8]
// Hook uses __fastcall (same register convention)
typedef int (__thiscall *orig_resolve_t)(void* thisp, char* name, uint32_t* a3);
static orig_resolve_t g_orig_resolve = nullptr;

static int __fastcall Hooked_Resolve(void* thisp, void* /* edx */, char* name, uint32_t* a3)
{
    if (!name || !a3) return g_orig_resolve(thisp, name, a3);

    g_dispatch_calls++;

    // Call sub_816830 first — preserves original side effects
    // and short-circuits if it returns nonzero.
    if (g_fn_816830) {
        int early = g_fn_816830(thisp, nullptr, name, a3);
        if (early) return early;
    }

    // Hash the event name and dispatch
    uint32_t h = HashEventNameCI(name);

    for (int i = 0; i < NUM_HANDLERS; i++) {
        if (h == (uint32_t)g_handlers[i].hash) {
            // Verify against collision with case-insensitive compare
            if (_stricmp(name, g_handlers[i].name) == 0) {
                g_hash_hits++;
                if (g_handlers[i].fmt) {
                    *a3 = (uint32_t)g_handlers[i].fmt;
                }
                return (int)((uintptr_t)thisp + g_handlers[i].offset);
            }
            // Hash collision on unknown name — fall through to original
            break;
        }
    }

    // Unknown handler name or hash collision — fall back to original
    // which will do the full strnicmp chain and return 0 for unknowns
    g_fallbacks++;
    return g_orig_resolve(thisp, name, a3);
}

bool InstallFrameScriptDispatch()
{
    void* target = reinterpret_cast<void*>(0x0048E680);

    unsigned char* p = (unsigned char*)target;
    if (p[0] != 0x55 || p[1] != 0x8B || p[2] != 0xEC) {
        Log("[FrameScriptDispatch] BAD PROLOGUE at 0x%08X", (uintptr_t)target);
        return false;
    }

    // Resolve sub_816830 address
    g_fn_816830 = (fn_816830_t)0x00816830;

    if (WineSafe_CreateHook(target, (void*)Hooked_Resolve, (void**)&g_orig_resolve) != MH_OK) {
        Log("[FrameScriptDispatch] MH_CreateHook FAILED");
        return false;
    }
    if (WO_EnableHook(target) != MH_OK) {
        Log("[FrameScriptDispatch] MH_EnableHook FAILED");
        MH_RemoveHook(target);
        return false;
    }

    Log("[FrameScriptDispatch] Installed: FNV-1a hash dispatch at 0x48E680 (18 handlers, O(1) lookup)");
    return true;
}

void UninstallFrameScriptDispatch()
{
    void* target = reinterpret_cast<void*>(0x0048E680);
    MH_DisableHook(target);
    MH_RemoveHook(target);

    uint64_t total = g_dispatch_calls;
    if (total > 0) {
        Log("[FrameScriptDispatch] Stats: %llu calls, %llu hash hits, %llu fallbacks (%.1f%% hit rate)",
            total, g_hash_hits, g_fallbacks, 100.0 * g_hash_hits / total);
    }
}
