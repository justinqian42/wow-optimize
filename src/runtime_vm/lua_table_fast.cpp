#include <windows.h>
#include <cstdint>
#include "MinHook.h"
#include "version.h"
#include "crash_dumper.h"

extern "C" void Log(const char* fmt, ...);

#define TAINT_CELL ( *(uint32_t**)0x00D4139C )
#define TAINT_A0       0x00D413A0
#define TAINT_A4       0x00D413A4
#define TAINT_CALLBACK 0x00D413B0

typedef int(__cdecl *settable_fn)(uintptr_t L, uintptr_t tt, uintptr_t kt, uintptr_t vt);
static settable_fn orig = nullptr;
static volatile long g_hits = 0, g_misses = 0;

extern "C" void InvalidateTableCacheSlot(void* table, void* key_str);

static int __cdecl hook(uintptr_t L, uintptr_t tt, uintptr_t kt, uintptr_t vt)
{
    // Common case: table is a real table (tt==5) and key is a string (tt==4)
    if (*(int*)(tt + 8) != 5) goto fallback;

    uintptr_t table = *(uintptr_t*)tt;
    if (table < 0x10000) goto fallback;

    int key_tt = *(int*)(kt + 8);
    if (key_tt == 4) {
        uintptr_t key_str = *(uintptr_t*)kt;
        if (key_str >= 0x10000) {
            InvalidateTableCacheSlot((void*)table, (void*)key_str);
        }
    }
    uintptr_t val0 = *(uintptr_t*)(vt + 0);
    uint32_t val1 = *(uint32_t*)(vt + 4);
    uint32_t val2 = *(uint32_t*)(vt + 8);
    uint32_t val3 = *(uint32_t*)(vt + 12);

    uintptr_t mt = *(uintptr_t*)(table + 12);
    if (mt < 0x10000) {  // No metatable — safe for direct write
        if (key_tt == 4) {  // String key
            uintptr_t key_str = *(uintptr_t*)kt;
            if (key_str < 0x10000) goto fallback;
            typedef uintptr_t(__cdecl *getstr_fn)(uintptr_t, uintptr_t);
            uintptr_t node = ((getstr_fn)0x0085C430)(table, key_str);
            if (node < 0x10000 || node == 0x00A46F78) goto fallback;
            uint32_t old_tt = *(uint32_t*)(node + 8);
            uint32_t old_taint = *(uint32_t*)(node + 12);
            *(uintptr_t*)(node + 0) = val0;
            *(uint32_t*)(node + 4) = val1;
            *(uint32_t*)(node + 8) = val2;
            *(uint32_t*)(node + 12) = val3;
            if (*(uintptr_t*)TAINT_CALLBACK) {
                if (table == *(uintptr_t*)(L + 0x48) && val3) {
                    if (!old_tt || old_taint != val3)
                        ((void(__cdecl*)(uintptr_t,int,const char*,uint32_t))*(uintptr_t*)TAINT_CALLBACK)(L, 1, (const char*)(key_str + 20), val3);
                }
            }
        } else if (key_tt == 3) {  // Numeric key — array write
            int idx = (int)*(double*)kt;
            if ((double)idx != *(double*)kt || idx < 1) goto fallback;
            if ((unsigned)(idx - 1) >= *(uint32_t*)(table + 32)) goto fallback;
            uintptr_t slot = *(uintptr_t*)(table + 16) + (idx - 1) * 16;
            *(uintptr_t*)(slot + 0) = val0;
            *(uint32_t*)(slot + 4) = val1;
            *(uint32_t*)(slot + 8) = val2;
            *(uint32_t*)(slot + 12) = val3;
        } else {
            goto fallback;
        }

        *(unsigned char*)(table + 10) = 0;  // luaH_set flag clear

        // Taint propagation (match engine)
        if (val3 && *(uint32_t*)TAINT_A0 && !*(uint32_t*)TAINT_A4)
            *(uint32_t*)0x00D4139C = val3;

        // GC barrier: check if value is white and table is black
        if (val2 >= 4 && (*(unsigned char*)(val0 + 9) & 3)) {
            if (*(unsigned char*)(table + 9) & 4) {
                // Black table, white value → barrier
                typedef void(__cdecl *barrier_fn)(uintptr_t, uintptr_t);
                ((barrier_fn)0x0085BA90)(L, table);
            }
        }

        g_hits++;
        return 0;
    }

fallback:
    g_misses++;
    return orig(L, tt, kt, vt);
}

bool InstallLuaTableFast() {
    void* t = (void*)0x008573C0;
    if (MH_CreateHook(t, hook, (void**)&orig) != MH_OK) return false;
    MH_EnableHook(t);
    Log("[TableFast] ACTIVE — luaV_settable inline at 0x8573C0");
    CrashDumper::RegisterFeature("TableFast");
    CrashDumper::FeatureSetActive("TableFast", true);
    return true;
}
