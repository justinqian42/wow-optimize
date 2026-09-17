#include <windows.h>
#include <cstdint>
#include "MinHook.h"
#include "version.h"
#include "crash_dumper.h"

extern "C" void Log(const char* fmt, ...);

// luaL_prepbuffer at 0x84F520 — prepare buffer for writing.
// If B->p != B->buffer (has content), pushes current content as string,
// updates lvl, resets p. Returns B->buffer pointer.
// Fast path: if buffer is already empty, skip the push/reset and return directly.

typedef uintptr_t(__cdecl *prepbuffer_fn)(uintptr_t B);
static prepbuffer_fn orig = nullptr;
static volatile long g_hits = 0, g_misses = 0;

// lua_pushlstring at 0x84E300
typedef const char*(__cdecl *pushlstring_fn)(uintptr_t L, const char* s, size_t len);
// lua_concat at 0x84EF90
typedef void(__cdecl *concat_fn)(uintptr_t L, int n);

static uintptr_t __cdecl hook(uintptr_t B) {
    if (B < 0x10000 || B > 0xFFE00000) {
        g_misses++;
        return orig(B);
    }
    __try {
        uintptr_t p = *(uintptr_t*)(B);
        uintptr_t buffer_addr = B + 12;
        if (p == buffer_addr) {
            // Buffer is empty — fast path, just return buffer pointer
            g_hits++;
            return buffer_addr;
        }
        // Buffer has content — need to push it (fall through to original)
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    g_misses++;
    return orig(B);
}

bool InstallLuaPrepbufferFast() {
    void* t = (void*)0x0084F520;
    if (MH_CreateHook(t, hook, (void**)&orig) != MH_OK) return false;
    WO_EnableHook(t);
    Log("[PrepBuf] ACTIVE — luaL_prepbuffer inline at 0x84F520");
    CrashDumper::RegisterFeature("PrepBuf");
    CrashDumper::FeatureSetActive("PrepBuf", true);
    return true;
}
