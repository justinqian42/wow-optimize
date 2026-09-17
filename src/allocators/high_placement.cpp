// ============================================================================
// Who holds the address space, and moving the large reservations above 2GB.
//
// Every out-of-memory report from this client ends the same way: the process
// cannot find a contiguous block, and the dumps say how much private memory
// sits below 2GB but never whose it is. The occupancy dump names images and
// mapped sections by file and has to call everything else "private" - 1377 MB
// in 4461 regions in the last tester log - because nothing on the path that
// created those regions wrote down who asked.
//
// That path is NtAllocateVirtualMemory, and on Windows 10 and later also
// NtAllocateVirtualMemoryEx. VirtualAlloc, the C runtime, mimalloc, DXVK and
// the GPU driver reserve through them, and so does ntdll's heap when it grows,
// which is why the VirtualAlloc arena (VaArena) never saw heap growth. The
// second entry point is not optional: a standalone test reserved an 8 MB heap
// block that a hook on the first one alone never saw.
//
// Both are hooked, with NtFreeVirtualMemory, and one entry is kept per live
// private reservation, indexed by its base. Windows places reservations on a
// 64 KB granularity, so a 4 GB space has 65536 possible bases, and a
// direct-mapped table of that many entries needs no hashing and no lock. Each
// entry names the module that asked.
//
// Finding the module. The first return address outside ntdll, kernel32,
// kernelbase and the C runtimes is the caller. RtlCaptureStackBackTrace follows
// frame pointers, and the system DLLs between this hook and the caller keep
// them, so the caller's return address is on the chain even when the caller
// itself was built without. When the walk still yields only system frames, the
// stack is scanned for a word inside a non-system module with a call
// instruction ending at it. Both ways are counted, and so are the reservations
// neither could attribute, so the report says what it did not see.
// Reservations that already existed when the hook went in are recorded once,
// from an address-space walk, under their own name: their callers are gone.
//
// Nothing on the hooked path takes a lock or allocates. It runs inside heap
// growth with the heap lock held and inside DLL loads with the loader lock
// held, and either would deadlock. The module list it attributes against is
// rebuilt elsewhere and published by swapping a pointer.
//
// Moving reservations up. With a switch on, a reservation of at least
// HighPlacementMinKB from an enabled class of caller gets MEM_TOP_DOWN added,
// so on a large-address-aware client it lands above 2GB and leaves the low
// half's contiguous space alone. This is placement only: nothing is
// redirected, the caller frees it the ordinary way, and a reservation that
// names an address, asks for zero high bits or carries extended parameters is
// passed through untouched. A top-down request that fails is retried exactly
// as the caller made it, so this cannot turn a success into a failure.
//
// The two classes are separate switches because they carry different risk.
// Modules other than wow.exe - DXVK, the GPU driver, other injected DLLs - are
// ordinary modern code. wow.exe is 2008 code whose large-address-aware flag was
// set by a patch rather than by its authors, and nothing has checked that every
// path in it treats a pointer above 2GB as unsigned. One observation is in its
// favour and it is not a proof: a tester's client kept running for a minute
// after the low half had run out, which it could only do on memory the OS
// placed above 2GB. And one limit on the module class: a heap segment placed
// high because a module grew a shared heap is afterwards used by every caller
// of that heap, wow.exe included.
//
// Not covered: NtMapViewOfSection. Mapped files and GPU-mapped memory reserve
// through it, and they are the "mapped" figure in the occupancy dump.
// ============================================================================

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <psapi.h>
#include <intrin.h>
#include <cstdint>
#include <cstring>

#include "MinHook.h"
#include "version.h"
#include "config.h"
#include "high_placement.h"

extern "C" void Log(const char* fmt, ...);

namespace HighPlacement {
namespace {

typedef LONG (NTAPI* NtAllocateVirtualMemory_fn)(HANDLE, PVOID*, ULONG_PTR, PSIZE_T, ULONG, ULONG);
// Seven arguments: the WoW64 stub returns with ret 1Ch. Same order as
// VirtualAlloc2 in memoryapi.h, with the base and the size passed by pointer as
// in every other Nt allocation call. The standalone test calls it directly to
// confirm the order before anything here relies on it.
typedef LONG (NTAPI* NtAllocateVirtualMemoryEx_fn)(HANDLE, PVOID*, PSIZE_T, ULONG, ULONG, PVOID, ULONG);
typedef LONG (NTAPI* NtFreeVirtualMemory_fn)(HANDLE, PVOID*, PSIZE_T, ULONG);

NtAllocateVirtualMemory_fn   orig_NtAllocateVirtualMemory   = nullptr;
NtAllocateVirtualMemoryEx_fn orig_NtAllocateVirtualMemoryEx = nullptr;
NtFreeVirtualMemory_fn       orig_NtFreeVirtualMemory       = nullptr;

const HANDLE    kCurrentProcess = (HANDLE)(LONG_PTR)-1;
const uintptr_t kLowHalfEnd     = 0x80000000u;

const unsigned char kClassUnresolved = 0;
const unsigned char kClassBefore     = 1;
const unsigned char kClassClient     = 2;
const unsigned char kClassOurs       = 3;
const unsigned char kClassSystem     = 4;
const unsigned char kClassOther      = 5;

// A slot names one module for the whole session, so counters and live entries
// survive the module list being rebuilt. Slot 0 is "caller not found" and slot 1
// is "existed before install"; modules take the rest in the order first seen.
const LONG kMaxSlots       = 256;
const LONG kSlotUnresolved = 0;
const LONG kSlotBefore     = 1;

struct Slot {
    uintptr_t     base;
    uintptr_t     end;
    unsigned char cls;
    char          name[39];
};
Slot g_slots[kMaxSlots];
volatile LONG g_slotCount = 2;

// Plain counters. Several threads reserve at once and an increment can be lost,
// so every figure printed from these is a lower bound.
ULONG g_slotReserves[kMaxSlots];
ULONG g_slotTopDown[kMaxSlots];

struct ModuleRange { uintptr_t base; uintptr_t end; LONG slot; };
struct ModuleTable { LONG count; ModuleRange mod[kMaxSlots]; };
ModuleTable g_tables[2];
ModuleTable* volatile g_currentTable = nullptr;
SRWLOCK g_refreshLock = SRWLOCK_INIT;
DWORD   g_lastRefreshTick = 0;

// One entry per possible 64 KB allocation base. Zero size means no entry.
struct LiveEntry { ULONG sizeKB; USHORT slot; USHORT reserved; };
const ULONG kLiveEntries = 65536;
LiveEntry* g_live = nullptr;

HMODULE g_self = nullptr;
HMODULE g_client = nullptr;
bool    g_installed = false;
const char* g_exState = "not looked for";

ULONG g_reserveCalls = 0, g_reserveCallsEx = 0;
ULONG g_attribWalk = 0, g_attribScan = 0, g_attribNone = 0;
ULONG g_topDownAdded = 0, g_topDownHigh = 0, g_topDownLow = 0, g_topDownRetried = 0;
ULONG g_liveOverwrites = 0;
ULONG g_lastReportReserves = 0, g_lastReportTopDown = 0;

bool IsSystemModuleName(const char* name) {
    static const char* const kSystem[] = {
        "ntdll.dll", "kernel32.dll", "kernelbase.dll", "ucrtbase.dll",
        "msvcrt.dll", "msvcr80.dll", "msvcr90.dll", "msvcr100.dll",
        "msvcr120.dll", "msvcp_win.dll", "msvcp140.dll", "vcruntime140.dll",
        "apphelp.dll",
    };
    for (size_t i = 0; i < sizeof(kSystem) / sizeof(kSystem[0]); i++) {
        if (_stricmp(name, kSystem[i]) == 0) return true;
    }
    return false;
}

unsigned char ClassifyModule(HMODULE h, const char* name) {
    if (h == g_client) return kClassClient;
    if (h == g_self) return kClassOurs;
    if (IsSystemModuleName(name)) return kClassSystem;
    return kClassOther;
}

// Called with g_refreshLock held, so only one thread ever adds a slot. A slot's
// fields are written before the count that makes it visible.
LONG FindOrAddSlot(HMODULE h, uintptr_t base, uintptr_t end) {
    char full[MAX_PATH];
    if (!GetModuleBaseNameA(GetCurrentProcess(), h, full, MAX_PATH)) {
        lstrcpynA(full, "(unnamed module)", MAX_PATH);
    }
    char name[sizeof(g_slots[0].name)];
    lstrcpynA(name, full, sizeof(name));

    const LONG count = g_slotCount;
    for (LONG s = kSlotBefore + 1; s < count; s++) {
        if (g_slots[s].base == base && g_slots[s].end == end &&
            strcmp(g_slots[s].name, name) == 0) {
            return s;
        }
    }
    if (count >= kMaxSlots) return -1;
    Slot& slot = g_slots[count];
    slot.base = base;
    slot.end  = end;
    lstrcpynA(slot.name, name, sizeof(slot.name));
    slot.cls  = ClassifyModule(h, full);
    g_slotCount = count + 1;
    return count;
}

// Readers may hold a table that a later refresh is rewriting. Every index read
// from it is bounded, so a torn read costs one wrong attribution and never a
// read outside the arrays.
LONG SlotForAddress(const ModuleTable* t, uintptr_t addr) {
    LONG lo = 0;
    LONG hi = t->count;
    if (hi < 0) hi = 0;
    if (hi > kMaxSlots) hi = kMaxSlots;
    while (lo < hi) {
        const LONG mid = (lo + hi) / 2;
        if (addr < t->mod[mid].base) {
            hi = mid;
        } else if (addr >= t->mod[mid].end) {
            lo = mid + 1;
        } else {
            const LONG s = t->mod[mid].slot;
            return (s >= 0 && s < kMaxSlots) ? s : -1;
        }
    }
    return -1;
}

// Whether a call instruction ends exactly at this address: E8 rel32, the far 9A,
// or FF /2 in any of its lengths. The same filter FreezeCallPrecedes applies in
// dllmain.cpp, for the same reason: a return address always has one in front of
// it, and a stale word on the stack almost never does.
bool CallEndsAt(uintptr_t addr) {
    __try {
        const unsigned char* p = (const unsigned char*)addr;
        if (p[-5] == 0xE8) return true;
        if (p[-7] == 0x9A) return true;
        for (int len = 2; len <= 7; len++) {
            if (p[-len] == 0xFF && ((p[-len + 1] >> 3) & 7) == 2) return true;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    return false;
}

bool Attributable(LONG slot) {
    return slot >= 0 && g_slots[slot].cls != kClassSystem;
}

// A new reservation is MEM_RESERVE, or MEM_COMMIT with no address, which
// reserves and commits in one call. The first version accepted only the flag,
// and a standalone test's 8 MB HeapAlloc went past both the census and placement
// entirely: ntdll's heap asks for large blocks with MEM_COMMIT alone.
bool IsOwnReserve(HANDLE process, PVOID* baseAddress, PSIZE_T regionSize,
                  ULONG allocationType) {
    if (process != kCurrentProcess || !baseAddress || !regionSize) return false;
    if ((allocationType & MEM_RESERVE) != 0) return true;
    return (allocationType & MEM_COMMIT) != 0 && *baseAddress == nullptr;
}

struct Decision {
    LONG slot;
    bool addTopDown;
};

#pragma optimize("y", off)

// retAddr is the return address of the hook itself, which is the instruction
// after the call into ntdll, and retSlot is where it sits on the stack. Frames
// are considered only from there outward, so this module's own frames are never
// mistaken for the caller.
__declspec(noinline)
LONG AttributeCaller(const ModuleTable* t, uintptr_t retAddr, uintptr_t retSlot) {
    LONG s = SlotForAddress(t, retAddr);
    if (Attributable(s)) { ++g_attribWalk; return s; }

    PVOID frames[24];
    const USHORT n = RtlCaptureStackBackTrace(0, 24, frames, NULL);
    bool past = false;
    for (USHORT i = 0; i < n; i++) {
        const uintptr_t a = (uintptr_t)frames[i];
        if (!past) {
            past = (a == retAddr);
            continue;
        }
        s = SlotForAddress(t, a);
        if (Attributable(s)) { ++g_attribWalk; return s; }
    }

    __try {
        const uintptr_t stackBase = (uintptr_t)__readfsdword(4);
        uintptr_t sp = retSlot + sizeof(uintptr_t);
        uintptr_t stop = sp + 1024 * sizeof(uintptr_t);
        if (stackBase > sp && stackBase < stop) stop = stackBase;
        for (; sp + sizeof(uintptr_t) <= stop; sp += sizeof(uintptr_t)) {
            const uintptr_t v = *(uintptr_t*)sp;
            s = SlotForAddress(t, v);
            if (Attributable(s) && CallEndsAt(v)) { ++g_attribScan; return s; }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }

    ++g_attribNone;
    return kSlotUnresolved;
}

// Everything decided before the call: who asked, and whether to add
// MEM_TOP_DOWN. mayPlace carries what only the hook knows - zero high bits for
// one entry point, no extended parameters for the other.
__declspec(noinline)
Decision Decide(PVOID* baseAddress, SIZE_T asked, ULONG allocationType, bool mayPlace,
                uintptr_t retAddr, uintptr_t retSlot) {
    Decision d = { kSlotUnresolved, false };
    const bool placeClient  = Config::g_settings.OptHighPlacementClient;
    const bool placeModules = Config::g_settings.OptHighPlacementModules;
    const SIZE_T minBytes   = (SIZE_T)Config::g_settings.HighPlacementMinKB * 1024;
    const bool candidate    = mayPlace && (placeClient || placeModules) &&
                              *baseAddress == nullptr &&
                              (allocationType & (MEM_TOP_DOWN | MEM_PHYSICAL |
                                                 MEM_LARGE_PAGES)) == 0 &&
                              asked >= minBytes;

    const ModuleTable* table = g_currentTable;
    if (table && (g_live != nullptr || candidate)) {
        d.slot = AttributeCaller(table, retAddr, retSlot);
    }
    if (candidate) {
        const unsigned char cls = g_slots[d.slot].cls;
        d.addTopDown = (cls == kClassClient && placeClient) ||
                       (cls == kClassOther && placeModules);
    }
    return d;
}

void Record(const Decision& d, LONG status, bool retried, PVOID base, SIZE_T size) {
    if (d.addTopDown) {
        ++g_topDownAdded;
        ++g_slotTopDown[d.slot];
        if (retried) {
            if (status >= 0) ++g_topDownRetried;
        } else if (status >= 0) {
            if ((uintptr_t)base >= kLowHalfEnd) ++g_topDownHigh; else ++g_topDownLow;
        }
    }
    if (status < 0) return;
    ++g_slotReserves[d.slot];
    if (g_live) {
        const ULONG index = (ULONG)((uintptr_t)base >> 16);
        if (g_live[index].sizeKB != 0) ++g_liveOverwrites;
        g_live[index].slot   = (USHORT)d.slot;
        g_live[index].sizeKB = (ULONG)((size + 1023) / 1024);
    }
}

LONG NTAPI Hooked_NtAllocateVirtualMemory(HANDLE process, PVOID* baseAddress,
                                          ULONG_PTR zeroBits, PSIZE_T regionSize,
                                          ULONG allocationType, ULONG protect) {
    if (!IsOwnReserve(process, baseAddress, regionSize, allocationType)) {
        return orig_NtAllocateVirtualMemory(process, baseAddress, zeroBits,
                                            regionSize, allocationType, protect);
    }
    ++g_reserveCalls;
    const SIZE_T asked = *regionSize;
    const Decision d = Decide(baseAddress, asked, allocationType, zeroBits == 0,
                              (uintptr_t)_ReturnAddress(),
                              (uintptr_t)_AddressOfReturnAddress());
    LONG status = orig_NtAllocateVirtualMemory(
        process, baseAddress, zeroBits, regionSize,
        d.addTopDown ? (allocationType | MEM_TOP_DOWN) : allocationType, protect);
    bool retried = false;
    if (d.addTopDown && status < 0) {
        *baseAddress = nullptr;
        *regionSize  = asked;
        status = orig_NtAllocateVirtualMemory(process, baseAddress, zeroBits,
                                              regionSize, allocationType, protect);
        retried = true;
    }
    Record(d, status, retried, *baseAddress, *regionSize);
    return status;
}

LONG NTAPI Hooked_NtAllocateVirtualMemoryEx(HANDLE process, PVOID* baseAddress,
                                            PSIZE_T regionSize, ULONG allocationType,
                                            ULONG protect, PVOID extended,
                                            ULONG extendedCount) {
    if (!IsOwnReserve(process, baseAddress, regionSize, allocationType)) {
        return orig_NtAllocateVirtualMemoryEx(process, baseAddress, regionSize,
                                              allocationType, protect, extended,
                                              extendedCount);
    }
    ++g_reserveCalls;
    ++g_reserveCallsEx;
    const SIZE_T asked = *regionSize;
    const Decision d = Decide(baseAddress, asked, allocationType, extendedCount == 0,
                              (uintptr_t)_ReturnAddress(),
                              (uintptr_t)_AddressOfReturnAddress());
    LONG status = orig_NtAllocateVirtualMemoryEx(
        process, baseAddress, regionSize,
        d.addTopDown ? (allocationType | MEM_TOP_DOWN) : allocationType, protect,
        extended, extendedCount);
    bool retried = false;
    if (d.addTopDown && status < 0) {
        *baseAddress = nullptr;
        *regionSize  = asked;
        status = orig_NtAllocateVirtualMemoryEx(process, baseAddress, regionSize,
                                                allocationType, protect, extended,
                                                extendedCount);
        retried = true;
    }
    Record(d, status, retried, *baseAddress, *regionSize);
    return status;
}

LONG NTAPI Hooked_NtFreeVirtualMemory(HANDLE process, PVOID* baseAddress,
                                      PSIZE_T regionSize, ULONG freeType) {
    if (g_live && process == kCurrentProcess && baseAddress &&
        (freeType & MEM_RELEASE) != 0) {
        const ULONG index = (ULONG)((uintptr_t)*baseAddress >> 16);
        const LiveEntry saved = g_live[index];
        // Cleared before the release rather than after it. The moment the
        // release completes another thread can be handed this same base, and
        // clearing afterwards would wipe that thread's new entry.
        g_live[index].sizeKB = 0;
        const LONG status = orig_NtFreeVirtualMemory(process, baseAddress,
                                                     regionSize, freeType);
        if (status < 0 && saved.sizeKB != 0 && g_live[index].sizeKB == 0) {
            g_live[index] = saved;
        }
        return status;
    }
    return orig_NtFreeVirtualMemory(process, baseAddress, regionSize, freeType);
}

#pragma optimize("", on)

void RecordExisting(uintptr_t base, SIZE_T size) {
    if (base == 0 || size == 0) return;
    const ULONG index = (ULONG)(base >> 16);
    g_live[index].slot   = (USHORT)kSlotBefore;
    g_live[index].sizeKB = (ULONG)((size + 1023) / 1024);
}

// Every private reservation that exists before the hook, so the census accounts
// for the whole address space rather than only what came after it.
void RecordExistingReservations() {
    MEMORY_BASIC_INFORMATION mbi;
    uintptr_t addr = 0x10000;
    uintptr_t runBase = 0;
    SIZE_T    runSize = 0;
    while (VirtualQuery((LPCVOID)addr, &mbi, sizeof(mbi))) {
        if (mbi.State != MEM_FREE && mbi.Type == MEM_PRIVATE) {
            const uintptr_t allocBase = (uintptr_t)mbi.AllocationBase;
            if (allocBase != runBase) {
                RecordExisting(runBase, runSize);
                runBase = allocBase;
                runSize = 0;
            }
            runSize += mbi.RegionSize;
        } else {
            RecordExisting(runBase, runSize);
            runBase = 0;
            runSize = 0;
        }
        const uintptr_t next = (uintptr_t)mbi.BaseAddress + mbi.RegionSize;
        if (next <= addr) break;
        addr = next;
    }
    RecordExisting(runBase, runSize);
}

const char* ClassName(unsigned char cls) {
    switch (cls) {
        case kClassClient: return "client";
        case kClassOurs:   return "this tool";
        case kClassSystem: return "system";
        case kClassOther:  return "module";
        default:           return "-";
    }
}

bool HookOne(const char* name, void* detour, void** original, bool required) {
    HMODULE ntdll = GetModuleHandleA("ntdll.dll");
    void* target = ntdll ? (void*)GetProcAddress(ntdll, name) : nullptr;
    if (!target) {
        if (required) Log("[HighPlacement] ntdll does not export %s", name);
        return false;
    }
    if (MH_CreateHook(target, detour, original) != MH_OK) {
        Log("[HighPlacement] %s could not be hooked", name);
        return false;
    }
    if (WO_EnableHook(target) != MH_OK) {
        MH_RemoveHook(target);
        Log("[HighPlacement] %s could not be enabled", name);
        return false;
    }
    return true;
}

}  // namespace

void RefreshModules() {
    if (!g_installed) return;
    AcquireSRWLockExclusive(&g_refreshLock);

    HMODULE mods[kMaxSlots];
    DWORD needed = 0;
    if (!EnumProcessModules(GetCurrentProcess(), mods, sizeof(mods), &needed)) {
        ReleaseSRWLockExclusive(&g_refreshLock);
        return;
    }
    DWORD n = needed / sizeof(HMODULE);
    if (n > (DWORD)kMaxSlots) n = (DWORD)kMaxSlots;

    ModuleTable* next = (g_currentTable == &g_tables[0]) ? &g_tables[1] : &g_tables[0];
    next->count = 0;
    LONG count = 0;
    for (DWORD i = 0; i < n; i++) {
        MODULEINFO info;
        if (!GetModuleInformation(GetCurrentProcess(), mods[i], &info, sizeof(info))) continue;
        const uintptr_t base = (uintptr_t)info.lpBaseOfDll;
        const uintptr_t end  = base + info.SizeOfImage;
        const LONG slot = FindOrAddSlot(mods[i], base, end);
        if (slot < 0) continue;
        LONG j = count;
        while (j > 0 && next->mod[j - 1].base > base) {
            next->mod[j] = next->mod[j - 1];
            j--;
        }
        next->mod[j].base = base;
        next->mod[j].end  = end;
        next->mod[j].slot = slot;
        count++;
    }
    next->count = count;
    g_currentTable = next;
    g_lastRefreshTick = GetTickCount();

    ReleaseSRWLockExclusive(&g_refreshLock);
}

bool Init() {
    const bool census       = Config::g_settings.OptVaCensus;
    const bool placeClient  = Config::g_settings.OptHighPlacementClient;
    const bool placeModules = Config::g_settings.OptHighPlacementModules;
    if (!census && !placeClient && !placeModules) {
        Log("[HighPlacement] off: VaCensus, HighPlacementModules and "
            "HighPlacementClient are all 0");
        return true;
    }
    if (RunningUnderTranslation()) {
        Log("[HighPlacement] NOT active: running under Wine or Rosetta, where "
            "ntdll's allocation entry points are not the Windows ones this "
            "hooks");
        return false;
    }

    SYSTEM_INFO si = {};
    GetSystemInfo(&si);
    const uintptr_t top = (uintptr_t)si.lpMaximumApplicationAddress;
    if ((placeClient || placeModules) && top <= kLowHalfEnd) {
        Log("[HighPlacement] placement has nowhere to put anything: the address "
            "space ends at 0x%08X, so this client is not large-address-aware. "
            "The census still runs if it is on.", (unsigned)top);
    }

    g_client = GetModuleHandleA(NULL);
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                       GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       (LPCSTR)&RefreshModules, &g_self);

    lstrcpynA(g_slots[kSlotUnresolved].name, "(caller not found)",
              sizeof(g_slots[0].name));
    g_slots[kSlotUnresolved].cls = kClassUnresolved;
    lstrcpynA(g_slots[kSlotBefore].name, "(existed before install)",
              sizeof(g_slots[0].name));
    g_slots[kSlotBefore].cls = kClassBefore;

    if (census) {
        g_live = (LiveEntry*)VirtualAlloc(nullptr, sizeof(LiveEntry) * kLiveEntries,
                                          MEM_COMMIT | MEM_RESERVE | MEM_TOP_DOWN,
                                          PAGE_READWRITE);
        if (!g_live) {
            Log("[HighPlacement] the census table (%u KB) could not be committed, "
                "error %lu, so the census does not run",
                (unsigned)(sizeof(LiveEntry) * kLiveEntries / 1024), GetLastError());
        } else {
            RecordExistingReservations();
        }
    }

    g_installed = true;
    RefreshModules();

    if (!HookOne("NtFreeVirtualMemory", (void*)Hooked_NtFreeVirtualMemory,
                 (void**)&orig_NtFreeVirtualMemory, true) ||
        !HookOne("NtAllocateVirtualMemory", (void*)Hooked_NtAllocateVirtualMemory,
                 (void**)&orig_NtAllocateVirtualMemory, true)) {
        g_installed = false;
        Log("[HighPlacement] NOT active - the reason is the line above");
        return false;
    }
    HMODULE ntdll = GetModuleHandleA("ntdll.dll");
    if (!GetProcAddress(ntdll, "NtAllocateVirtualMemoryEx")) {
        g_exState = "not exported by this Windows, so there is nothing to miss";
    } else if (HookOne("NtAllocateVirtualMemoryEx", (void*)Hooked_NtAllocateVirtualMemoryEx,
                       (void**)&orig_NtAllocateVirtualMemoryEx, false)) {
        g_exState = "hooked";
    } else {
        g_exState = "NOT hooked, so heap growth is missing from the census and "
                    "from placement";
    }

    Log("[HighPlacement] ACTIVE: census %s, MEM_TOP_DOWN for reservations of at "
        "least %d KB from %s. NtAllocateVirtualMemoryEx: %s. Placement only - "
        "nothing is redirected and nothing is freed differently.",
        g_live ? "on" : "off", Config::g_settings.HighPlacementMinKB,
        (placeClient && placeModules) ? "wow.exe and other modules"
        : placeClient                 ? "wow.exe only"
        : placeModules                ? "modules other than wow.exe"
                                      : "nobody (placement is off)",
        g_exState);
    return true;
}

void LogLiveByCaller(bool lowHalfOnly, const char* compareWith) {
    if (!g_live) {
        Log("[HighPlacement]   live reservations by caller: not measured, the "
            "census is off");
        return;
    }

    unsigned long long lowKB[kMaxSlots] = {};
    unsigned long long highKB[kMaxSlots] = {};
    ULONG lowCount[kMaxSlots] = {};
    unsigned long long totalLow = 0, totalHigh = 0;
    for (ULONG i = 0; i < kLiveEntries; i++) {
        const LiveEntry e = g_live[i];
        if (e.sizeKB == 0 || e.slot >= kMaxSlots) continue;
        if (i < 0x8000) {
            lowKB[e.slot] += e.sizeKB;
            lowCount[e.slot]++;
            totalLow += e.sizeKB;
        } else {
            highKB[e.slot] += e.sizeKB;
            totalHigh += e.sizeKB;
        }
    }

    Log("[HighPlacement]   live private reservations: %.0f MB below 2GB, %.0f MB "
        "above, by the module that asked%s%s%s:",
        totalLow / 1024.0, totalHigh / 1024.0,
        compareWith ? " (the below-2GB total should roughly match " : "",
        compareWith ? compareWith : "",
        compareWith ? ")" : "");

    bool shown[kMaxSlots] = {};
    const LONG slotCount = g_slotCount;
    for (int row = 0; row < 12; row++) {
        LONG best = -1;
        unsigned long long bestKey = 0;
        for (LONG s = 0; s < slotCount && s < kMaxSlots; s++) {
            if (shown[s]) continue;
            const unsigned long long key = lowHalfOnly ? lowKB[s] : lowKB[s] + highKB[s];
            if (key == 0) continue;
            if (best < 0 || key > bestKey) { best = s; bestKey = key; }
        }
        if (best < 0) break;
        shown[best] = true;
        Log("[HighPlacement]     %-26s %-9s %7.1f MB below 2GB in %lu reservation(s), "
            "%7.1f MB above",
            g_slots[best].name, ClassName(g_slots[best].cls),
            lowKB[best] / 1024.0, lowCount[best], highKB[best] / 1024.0);
    }
}

void LogStats() {
    const bool census       = Config::g_settings.OptVaCensus;
    const bool placeClient  = Config::g_settings.OptHighPlacementClient;
    const bool placeModules = Config::g_settings.OptHighPlacementModules;
    if (!census && !placeClient && !placeModules) {
        Log("[HighPlacement] not measured: switched off");
        return;
    }
    if (!g_installed) {
        Log("[HighPlacement] not active - the reason is at the top of this log");
        return;
    }
    if (GetTickCount() - g_lastRefreshTick > 60000) RefreshModules();

    const ULONG reserves = g_reserveCalls;
    const ULONG added    = g_topDownAdded;
    Log("[HighPlacement] %lu private reservations seen (%lu through "
        "NtAllocateVirtualMemoryEx), %lu since the last report; lower bounds. "
        "Callers found by frame walk %lu, by stack scan %lu, not found %lu.",
        reserves, g_reserveCallsEx, reserves - g_lastReportReserves,
        g_attribWalk, g_attribScan, g_attribNone);
    if (placeClient || placeModules) {
        Log("[HighPlacement] MEM_TOP_DOWN added to %lu of them, %lu since the last "
            "report: %lu landed above 2GB, %lu still below because the high half "
            "had no room, %lu failed that way and succeeded when retried as asked.",
            added, added - g_lastReportTopDown,
            g_topDownHigh, g_topDownLow, g_topDownRetried);
    } else {
        Log("[HighPlacement] placement is off; this session only records who "
            "reserves what.");
    }
    if (g_liveOverwrites) {
        Log("[HighPlacement] %lu reservation(s) arrived at a base the census still "
            "held, so a release was missed - the live totals below overstate by "
            "that much at most.", g_liveOverwrites);
    }
    g_lastReportReserves = reserves;
    g_lastReportTopDown  = added;

    LogLiveByCaller(false, nullptr);
}

}  // namespace HighPlacement
