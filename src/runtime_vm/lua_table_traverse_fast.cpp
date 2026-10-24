// ============================================================================
// Module: lua_table_traverse_fast.cpp
//
// Accelerates Lua GC table traversal (luaC_traversetable at 0x0085A960).
// In field profiles (wow_optimize_2026-10-22_16-54-34.log), luaC_traversetable
// accounted for 14,359 samples (1.43% of executing time), walking over 3 billion
// array and hash slots across millions of table visits per session.
//
// In the stock client, sub_85A960 iterates through the array and hash node
// portions slot-by-slot. For every collectable value (tt >= 4), it pushes
// arguments and calls reallymarkobject (sub_85A7D0). However, 90%+ of collectable
// table keys and values are strings (tt == 4), where reallymarkobject performs
// no recursion or gray-list insertion - it only clears the 2 white bits:
//     o->gch.marked &= ~3;
//
// This replacement:
//   1. Inlines the white-bit clear directly for strings (tt == 4), eliminating
//      tens of millions of function call frames and stack adjustments.
//   2. Prefetches 64 bytes ahead during the array and node list walks.
//   3. Retains exact client handling for metatables, weak tables, dead keys,
//      and non-string objects.
//
// Offline harness verified against verbatim client assembly over 100,000
// randomized cases with 0 bit differences (harness only, not run in a game).
// ============================================================================

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <cstdint>
#include <cstring>
#include <emmintrin.h>

#include "lua_table_traverse_fast.h"
#include "MinHook.h"
#include "version.h"
#include "config.h"
#include "sampling_profiler.h"

extern "C" void Log(const char* fmt, ...);
extern "C" void __cdecl LuaTableCensus_Note(uint32_t t);
MH_STATUS WineSafe_CreateHook(void* target, void* detour, void** original);
MH_STATUS WO_EnableHook(void* target);

namespace LuaTableTraverseFast {

namespace {

constexpr uintptr_t kTarget = 0x0085A960;

// Expected opening bytes of luaC_traversetable (sub_85A960)
// push ebp / mov ebp, esp / sub esp, 0Ch / mov eax, [ebx+0Ch] / push esi / xor esi, esi / cmp eax, esi / push edi / mov [ebp-4], esi
static const unsigned char kExpectedPrologue[16] = {
    0x55, 0x8B, 0xEC, 0x83, 0xEC, 0x0C, 0x8B, 0x43,
    0x0C, 0x56, 0x33, 0xF6, 0x3B, 0xC6, 0x57, 0x89
};

#pragma pack(push, 1)
struct LuaGCObject {
    void*    next;       // 0x00
    uint32_t unk;        // 0x04
    uint8_t  tt;         // 0x08
    uint8_t  marked;     // 0x09
    uint8_t  pad[2];     // 0x0A
};

struct LuaTValue {
    union {
        LuaGCObject* gc;
        void* p;
        double n;
        int b;
    } value;             // 0x00 (8 bytes)
    int tt;              // 0x08 (4 bytes)
    int pad;             // 0x0C (4 bytes)
}; // 16 bytes

struct LuaTKey {
    union {
        LuaGCObject* gc;
        void* p;
        double n;
        int b;
    } value;             // 0x00 (8 bytes)
    int tt;              // 0x08 (4 bytes)
    int pad;             // 0x0C (4 bytes)
    void* next;          // 0x10 (4 bytes)
    int pad2;            // 0x14 (4 bytes)
}; // 24 bytes

struct LuaNode {
    LuaTValue i_val;     // 0x00 (16 bytes)
    LuaTKey   i_key;     // 0x10 (24 bytes)
}; // 40 bytes

struct LuaTable {
    void* next;          // 0x00
    uint32_t unk;        // 0x04
    uint8_t tt;          // 0x08
    uint8_t marked;      // 0x09
    uint8_t flags;       // 0x0A
    uint8_t lsizenode;   // 0x0B
    LuaTable* metatable; // 0x0C
    LuaTValue* array;    // 0x10
    LuaNode* node;       // 0x14
    LuaNode* lastfree;   // 0x18
    void* gclist;        // 0x1C
    int sizearray;       // 0x20
};
#pragma pack(pop)

typedef unsigned int (__cdecl *ReallyMarkObjectFn)(void* g, void* o);
static const ReallyMarkObjectFn fnReallyMarkObject = (ReallyMarkObjectFn)0x0085A7D0;

static void* g_origTraverse = nullptr;
static bool g_installed = false;
static bool g_retired = false;

static unsigned long long g_tablesVisited = 0;
static unsigned long long g_stringsInlined = 0;
static unsigned long long g_objectsMarked = 0;

__declspec(naked) static int Forward_OrigTraverse(LuaTable* h, void* g) {
    __asm {
        mov eax, [esp+8] // g
        mov ebx, [esp+4] // h
        push eax         // push g
        call dword ptr [g_origTraverse]
        add esp, 4
        retn
    }
}

#pragma check_stack(off)
__declspec(safebuffers) static int __cdecl LuaTableTraverseFast_Impl(LuaTable* h, void* g) {
    if (!h) return 0;

    ++g_tablesVisited;

    if (Config::g_settings.OptLuaTableCensus) {
        LuaTableCensus_Note((uint32_t)(uintptr_t)h);
    }

    // Metatable handling
    if (h->metatable) {
        LuaTable* mt = h->metatable;
        if (mt->marked & 3) {
            fnReallyMarkObject(g, mt);
            ++g_objectsMarked;
        }
        // If metatable might have weak mode (__mode) (bit 3 of flags is 0):
        if ((mt->flags & 8) == 0) {
            // Forward weak table configuration to client verbatim handler (<0.01% of tables)
            return Forward_OrigTraverse(h, g);
        }
    }

    // Array part
    int countA = h->sizearray;
    if (countA > 0) {
        LuaTValue* tv = h->array + countA;
        while (countA >= 4) {
            _mm_prefetch((const char*)(tv - 8), _MM_HINT_T0);
            for (int k = 0; k < 4; ++k) {
                --tv;
                if (tv->tt >= 4) {
                    LuaGCObject* gc = tv->value.gc;
                    if (gc && (gc->marked & 3)) {
                        if (gc->tt == 4) {
                            gc->marked &= 0xFC; // Inlined string mark
                            ++g_stringsInlined;
                        } else {
                            fnReallyMarkObject(g, gc);
                            ++g_objectsMarked;
                        }
                    }
                }
            }
            countA -= 4;
        }
        while (countA--) {
            --tv;
            if (tv->tt >= 4) {
                LuaGCObject* gc = tv->value.gc;
                if (gc && (gc->marked & 3)) {
                    if (gc->tt == 4) {
                        gc->marked &= 0xFC;
                        ++g_stringsInlined;
                    } else {
                        fnReallyMarkObject(g, gc);
                        ++g_objectsMarked;
                    }
                }
            }
        }
    }

    // Node part
    int countN = 1 << h->lsizenode;
    if (countN > 0) {
        LuaNode* n = h->node + countN;
        while (countN >= 2) {
            _mm_prefetch((const char*)(n - 4), _MM_HINT_T0);
            for (int k = 0; k < 2; ++k) {
                --n;
                if (n->i_val.tt != 0) {
                    // Key
                    if (n->i_key.tt >= 4) {
                        LuaGCObject* kgc = n->i_key.value.gc;
                        if (kgc && (kgc->marked & 3)) {
                            if (kgc->tt == 4) {
                                kgc->marked &= 0xFC; // Inlined string mark
                                ++g_stringsInlined;
                            } else {
                                fnReallyMarkObject(g, kgc);
                                ++g_objectsMarked;
                            }
                        }
                    }
                    // Val
                    if (n->i_val.tt >= 4) {
                        LuaGCObject* vgc = n->i_val.value.gc;
                        if (vgc && (vgc->marked & 3)) {
                            if (vgc->tt == 4) {
                                vgc->marked &= 0xFC; // Inlined string mark
                                ++g_stringsInlined;
                            } else {
                                fnReallyMarkObject(g, vgc);
                                ++g_objectsMarked;
                            }
                        }
                    }
                } else if (n->i_key.tt >= 4) {
                    n->i_key.tt = 11; // LUA_TDEADKEY (11)
                }
            }
            countN -= 2;
        }
        while (countN--) {
            --n;
            if (n->i_val.tt != 0) {
                if (n->i_key.tt >= 4) {
                    LuaGCObject* kgc = n->i_key.value.gc;
                    if (kgc && (kgc->marked & 3)) {
                        if (kgc->tt == 4) {
                            kgc->marked &= 0xFC;
                            ++g_stringsInlined;
                        } else {
                            fnReallyMarkObject(g, kgc);
                            ++g_objectsMarked;
                        }
                    }
                }
                if (n->i_val.tt >= 4) {
                    LuaGCObject* vgc = n->i_val.value.gc;
                    if (vgc && (vgc->marked & 3)) {
                        if (vgc->tt == 4) {
                            vgc->marked &= 0xFC;
                            ++g_stringsInlined;
                        } else {
                            fnReallyMarkObject(g, vgc);
                            ++g_objectsMarked;
                        }
                    }
                }
            } else if (n->i_key.tt >= 4) {
                n->i_key.tt = 11;
            }
        }
    }

    return 0;
}
#pragma check_stack()

// Naked thunk bridging __usercall (table in EBX, g on stack, caller pops stack)
__declspec(naked) static void Hook_TraverseTableThunk() {
    __asm {
        cmp byte ptr [g_retired], 0
        jnz loc_forward
        push dword ptr [esp+4] // g
        push ebx               // h
        call LuaTableTraverseFast_Impl
        add esp, 8
        retn
    loc_forward:
        jmp dword ptr [g_origTraverse]
    }
}

} // namespace

bool Init() {
    if (!Config::g_settings.OptLuaTableTraverseFast) {
        return true;
    }

    // Verify prologue bytes at 0x0085A960
    if (std::memcmp((const void*)kTarget, kExpectedPrologue, sizeof(kExpectedPrologue)) != 0) {
        Log("[LuaTableTraverseFast] NOT installed: prologue mismatch at 0x%08X", (unsigned)kTarget);
        return false;
    }

    // Startup self-test vectors
    {
        LuaGCObject strObj = {0};
        strObj.tt = 4; // string
        strObj.marked = 3; // white

        LuaTValue tv = {0};
        tv.tt = 4;
        tv.value.gc = &strObj;

        LuaNode node = {0};
        node.i_val.tt = 4;
        node.i_val.value.gc = &strObj;
        node.i_key.tt = 4;
        node.i_key.value.gc = &strObj;

        LuaTable testTable = {0};
        testTable.sizearray = 1;
        testTable.array = &tv;
        testTable.lsizenode = 0; // 1 node
        testTable.node = &node;

        LuaTableTraverseFast_Impl(&testTable, nullptr);

        if ((strObj.marked & 3) != 0) {
            Log("[LuaTableTraverseFast] Startup self-test FAILED: string marked bit not cleared");
            return false;
        }

        // Test dead key conversion
        LuaNode deadNode = {0};
        deadNode.i_val.tt = 0; // nil
        deadNode.i_key.tt = 4; // collectable key
        testTable.sizearray = 0;
        testTable.node = &deadNode;

        LuaTableTraverseFast_Impl(&testTable, nullptr);

        if (deadNode.i_key.tt != 11) {
            Log("[LuaTableTraverseFast] Startup self-test FAILED: dead key not set to 11");
            return false;
        }

        // Reset stats
        g_tablesVisited = 0;
        g_stringsInlined = 0;
        g_objectsMarked = 0;
    }

    if (WineSafe_CreateHook((void*)kTarget, (void*)Hook_TraverseTableThunk, &g_origTraverse) != MH_OK) {
        Log("[LuaTableTraverseFast] Hook creation failed at 0x%08X", (unsigned)kTarget);
        return false;
    }

    if (WO_EnableHook((void*)kTarget) != MH_OK) {
        Log("[LuaTableTraverseFast] Hook enable failed at 0x%08X", (unsigned)kTarget);
        return false;
    }

    SamplingProfiler::RegisterSelfSymbol("LuaTableTraverseFast_Hook", (const void*)&Hook_TraverseTableThunk);
    SamplingProfiler::RegisterSelfSymbol("LuaTableTraverseFast_Impl", (const void*)&LuaTableTraverseFast_Impl);

    g_installed = true;
    Log("[LuaTableTraverseFast] Hook installed at 0x%08X (inlined string mark & vector prefetch)", (unsigned)kTarget);
    return true;
}

void LogStats() {
    if (!Config::g_settings.OptLuaTableTraverseFast) {
        return;
    }
    if (!g_installed) {
        Log("[LuaTableTraverseFast] not installed");
        return;
    }
    Log("[LuaTableTraverseFast] %llu tables traversed, %llu string marks inlined, %llu objects marked",
        g_tablesVisited, g_stringsInlined, g_objectsMarked);
}

void Shutdown() {
}

} // namespace LuaTableTraverseFast
