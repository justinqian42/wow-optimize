#include <windows.h>
#include <cstdint>
#include "MinHook.h"
#include "version.h"
#include "crash_dumper.h"

extern "C" void Log(const char* fmt, ...);

typedef int(__cdecl *lessthan_fn)(uintptr_t L, int index1, int index2);
static lessthan_fn orig = nullptr;
static volatile long g_hits = 0, g_misses = 0;

static int __cdecl hook(uintptr_t L, int index1, int index2) {
    if (L > 0x10000 && L < 0xFFE00000) {
        __try {
            uintptr_t base = *(uintptr_t*)(L + 0x10);
            uintptr_t top = *(uintptr_t*)(L + 0x0C);
            if (base > 0x10000 && base < 0xFFE00000 && top > 0x10000 && top < 0xFFE00000) {
                uintptr_t tv1 = (index1 > 0) ? (base + (index1 - 1) * 16) : (top + index1 * 16);
                uintptr_t tv2 = (index2 > 0) ? (base + (index2 - 1) * 16) : (top + index2 * 16);
                if (tv1 > 0x10000 && tv1 < 0xFFE00000 && tv2 > 0x10000 && tv2 < 0xFFE00000) {
                    uint32_t tt1 = *(uint32_t*)(tv1 + 8);
                    uint32_t tt2 = *(uint32_t*)(tv2 + 8);
                    if (tt1 == 3 && tt2 == 3) {
                        double d1 = *(double*)(tv1);
                        double d2 = *(double*)(tv2);
                        g_hits++;
                        return (d1 < d2) ? 1 : 0;
                    }
                }
            }
        } __except(EXCEPTION_EXECUTE_HANDLER) {}
    }
    g_misses++;
    return orig(L, index1, index2);
}

bool InstallLuaLessThanFast() {
    Log("[LessThan] DISABLED — address 0x84F030 is actually format_num");
    return false;
}
