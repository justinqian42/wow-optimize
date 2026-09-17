// ============================================================================
// Description: Bypasses legacy serialization checks during CDataStore read and write loops.
// Safety & Threading: Thread-safe. Requires validation checks verifying read/write positions are above base_offset.
// ============================================================================

#include <windows.h>
#include <MinHook.h>
#include <cstdint>
#include "version.h"
#include "datastore_fastpath.h"

extern "C" void Log(const char* fmt, ...);

struct CDataStore {
    void**   vtable;         // +0x00: vtable pointer
    uint8_t* buffer;         // +0x04: data buffer base
    uint32_t base_offset;    // +0x08: subtracted from buffer ptr for effective addr
    uint32_t allocated_size; // +0x0C: allocated capacity in bytes
    uint32_t write_pos;      // +0x10: absolute write position
    uint32_t read_pos;       // +0x14: absolute read position
};

// TLS-cached pointer to avoid repeated buffer arithmetic
struct DataStoreTLS {
    CDataStore* store;
    uint8_t*    effective_base; // buffer - base_offset (precomputed)
    uint32_t    allocated_size;
};
static __declspec(thread) DataStoreTLS t_cache = {};

// ================================================================
// Statistics (diagnostic hit-rate only; incremented non-atomically on
// purpose -- these run on the hottest path in the binary and a locked
// increment costs several times the actual work. Lost counts under
// thread contention don't affect the ratio we log.)
// ================================================================
static volatile long g_getdword_calls = 0, g_getdword_hits = 0;
static volatile long g_putdword_calls = 0, g_putdword_hits = 0;
static volatile long g_getbyte_calls  = 0, g_getbyte_hits  = 0;
static volatile long g_putbyte_calls  = 0, g_putbyte_hits  = 0;
static volatile long g_getqword_calls = 0, g_getqword_hits = 0;
static volatile long g_putqword_calls = 0, g_putqword_hits = 0;

// How many of the six hooks went in. Without it a report of all zeroes cannot be
// told from a module that never installed, and this one reported neither for its
// whole life: the counters were printed only from ShutdownDataStoreFastPath,
// which does not run because the process leaves through TerminateProcess.
static uint32_t g_installedHooks = 0;
// Whether Init was reached at all. Init is only called when the switch is on, so
// this separates "switched off" from "asked to install and could not", which
// are different answers to the only question a reader has.
static bool g_initRan = false;

// ================================================================
// Original function pointers (__thiscall on x86)
// ================================================================
typedef CDataStore* (__thiscall* GetDword_t)(CDataStore*, uint32_t*);
typedef CDataStore* (__thiscall* PutDword_t)(CDataStore*, uint32_t);
typedef CDataStore* (__thiscall* GetByte_t)(CDataStore*, uint8_t*);
typedef CDataStore* (__thiscall* PutByte_t)(CDataStore*, uint8_t);
typedef CDataStore* (__thiscall* GetQword_t)(CDataStore*, uint32_t*);
typedef CDataStore* (__thiscall* PutQword_t)(CDataStore*, uint32_t, uint32_t);

static GetDword_t pOrigGetDword = nullptr;
static PutDword_t pOrigPutDword = nullptr;
static GetByte_t  pOrigGetByte  = nullptr;
static PutByte_t  pOrigPutByte  = nullptr;
static GetQword_t pOrigGetQword = nullptr;
static PutQword_t pOrigPutQword = nullptr;

// Inline helpers
__forceinline void UpdateTLS(CDataStore* s) {
    t_cache.store          = s;
    t_cache.effective_base = s->buffer - s->base_offset;
    t_cache.allocated_size = s->allocated_size;
}

// End of the window this store currently maps, clamped to the data actually
// written. The engine (sub_47B290) refuses a read that leaves
// [base_offset, base_offset + allocated_size) and calls a virtual method to fetch
// the next window instead. Checking read_pos against write_pos alone accepts
// positions that are inside the stream but outside the mapping, and reads them
// from unmapped memory.
__forceinline uint32_t WindowEnd(CDataStore* s) {
    uint32_t end = s->base_offset + s->allocated_size;
    return (end > s->write_pos) ? s->write_pos : end;
}

__forceinline bool TLSCacheValid(CDataStore* s) {
    return t_cache.store == s &&
           t_cache.allocated_size == s->allocated_size &&
           t_cache.effective_base == (s->buffer - s->base_offset);
}

// ================================================================
// sub_47B3C0: CDataStore::GetDword (read 4 bytes, 1477 xrefs)
// MinHook __thiscall workaround: use __fastcall, EDX is unused padding
// ================================================================
static CDataStore* __fastcall HookGetDword(CDataStore* self, void*, uint32_t* out) {
#if !TEST_DISABLE_DATASTORE_FASTPATH
    ++g_getdword_calls;

    if (TLSCacheValid(self)) {
        uint32_t rp = self->read_pos;
        if (rp >= self->base_offset && (rp + 4) <= WindowEnd(self)) {
            ++g_getdword_hits;
            *out = *reinterpret_cast<uint32_t*>(t_cache.effective_base + rp);
            self->read_pos = rp + 4;
            return self;
        }
    }
#endif
    CDataStore* r = pOrigGetDword(self, out);
#if !TEST_DISABLE_DATASTORE_FASTPATH
    UpdateTLS(self);
#endif
    return r;
}

// ================================================================
// sub_47B0A0: CDataStore::PutDword (write 4 bytes, 1185 xrefs)
// ================================================================
static CDataStore* __fastcall HookPutDword(CDataStore* self, void*, uint32_t value) {
#if !TEST_DISABLE_DATASTORE_FASTPATH
    ++g_putdword_calls;

    if (TLSCacheValid(self)) {
        uint32_t wp = self->write_pos;
        uint32_t end = self->base_offset + t_cache.allocated_size;
        if (wp >= self->base_offset && (wp + 4) <= end) {
            ++g_putdword_hits;
            *reinterpret_cast<uint32_t*>(t_cache.effective_base + wp) = value;
            self->write_pos = wp + 4;
            return self;
        }
    }
#endif
    CDataStore* r = pOrigPutDword(self, value);
#if !TEST_DISABLE_DATASTORE_FASTPATH
    UpdateTLS(self);
#endif
    return r;
}

// ================================================================
// sub_47B340: CDataStore::GetByte (read 1 byte, 504 xrefs)
// ================================================================
static CDataStore* __fastcall HookGetByte(CDataStore* self, void*, uint8_t* out) {
#if !TEST_DISABLE_DATASTORE_FASTPATH
    ++g_getbyte_calls;

    if (TLSCacheValid(self)) {
        uint32_t rp = self->read_pos;
        if (rp >= self->base_offset && (rp + 1) <= WindowEnd(self)) {
            ++g_getbyte_hits;
            *out = t_cache.effective_base[rp];
            self->read_pos = rp + 1;
            return self;
        }
    }
#endif
    CDataStore* r = pOrigGetByte(self, out);
#if !TEST_DISABLE_DATASTORE_FASTPATH
    UpdateTLS(self);
#endif
    return r;
}

// ================================================================
// sub_47AFE0: CDataStore::PutByte (write 1 byte, 439 xrefs)
// ================================================================
static CDataStore* __fastcall HookPutByte(CDataStore* self, void*, uint8_t value) {
#if !TEST_DISABLE_DATASTORE_FASTPATH
    ++g_putbyte_calls;

    if (TLSCacheValid(self)) {
        uint32_t wp = self->write_pos;
        uint32_t end = self->base_offset + t_cache.allocated_size;
        if (wp >= self->base_offset && (wp + 1) <= end) {
            ++g_putbyte_hits;
            t_cache.effective_base[wp] = value;
            self->write_pos = wp + 1;
            return self;
        }
    }
#endif
    CDataStore* r = pOrigPutByte(self, value);
#if !TEST_DISABLE_DATASTORE_FASTPATH
    UpdateTLS(self);
#endif
    return r;
}

// ================================================================
// sub_47B100: CDataStore::PutQword (write 8 bytes, 290 xrefs)
// Two stack args: low_dword, high_dword
// ================================================================
static CDataStore* __fastcall HookPutQword(CDataStore* self, void*, uint32_t lo, uint32_t hi) {
#if !TEST_DISABLE_DATASTORE_FASTPATH
    ++g_putqword_calls;

    if (TLSCacheValid(self)) {
        uint32_t wp = self->write_pos;
        uint32_t end = self->base_offset + t_cache.allocated_size;
        if (wp >= self->base_offset && (wp + 8) <= end) {
            ++g_putqword_hits;
            uint8_t* p = t_cache.effective_base + wp;
            *reinterpret_cast<uint32_t*>(p)     = lo;
            *reinterpret_cast<uint32_t*>(p + 4) = hi;
            self->write_pos = wp + 8;
            return self;
        }
    }
#endif
    CDataStore* r = pOrigPutQword(self, lo, hi);
#if !TEST_DISABLE_DATASTORE_FASTPATH
    UpdateTLS(self);
#endif
    return r;
}

// ================================================================
// sub_47B400: CDataStore::GetQword (read 8 bytes, 284 xrefs)
// Output param: uint32_t* out (_DWORD* a2, writes 8 bytes)
// ================================================================
static CDataStore* __fastcall HookGetQword(CDataStore* self, void*, uint32_t* out) {
#if !TEST_DISABLE_DATASTORE_FASTPATH
    ++g_getqword_calls;

    if (TLSCacheValid(self)) {
        uint32_t rp = self->read_pos;
        if (rp >= self->base_offset && (rp + 8) <= WindowEnd(self)) {
            ++g_getqword_hits;
            uint8_t* p = t_cache.effective_base + rp;
            out[0] = *reinterpret_cast<uint32_t*>(p);
            out[1] = *reinterpret_cast<uint32_t*>(p + 4);
            self->read_pos = rp + 8;
            return self;
        }
    }
#endif
    CDataStore* r = pOrigGetQword(self, out);
#if !TEST_DISABLE_DATASTORE_FASTPATH
    UpdateTLS(self);
#endif
    return r;
}

// Install hooks
bool InitDataStoreFastPath() {
    g_initRan = true;
#if TEST_DISABLE_DATASTORE_FASTPATH
    Log("[DataStore] DISABLED via feature flag");
    return false;
#else
    struct HookDef {
        void*     addr;
        void*     hook;
        void**    orig;
        const char* name;
        uint32_t  xrefs;
    };
    HookDef hooks[] = {
        {(void*)0x0047B3C0, (void*)HookGetDword, (void**)&pOrigGetDword, "GetDword", 1477},
        {(void*)0x0047B0A0, (void*)HookPutDword, (void**)&pOrigPutDword, "PutDword", 1185},
        {(void*)0x0047B340, (void*)HookGetByte,  (void**)&pOrigGetByte,  "GetByte",  504},
        {(void*)0x0047AFE0, (void*)HookPutByte,  (void**)&pOrigPutByte,  "PutByte",  439},
        {(void*)0x0047B100, (void*)HookPutQword, (void**)&pOrigPutQword, "PutQword", 290},
        {(void*)0x0047B400, (void*)HookGetQword, (void**)&pOrigGetQword, "GetQword", 284}
    };

    uint32_t installed = 0;
    for (auto& h : hooks) {
        if (MH_CreateHook(h.addr, h.hook, h.orig) == MH_OK &&
            MH_EnableHook(h.addr) == MH_OK) {
            installed++;
        } else {
            Log("[DataStore] Failed to hook %s at 0x%p", h.name, h.addr);
        }
    }

    Log("[DataStore] FastPath installed %d/6 hooks", installed);
    g_installedHooks = installed;
    return installed == 6;
#endif
}

void LogDataStoreStats() {
#if TEST_DISABLE_DATASTORE_FASTPATH
    Log("[DataStore] not measured: compiled out at build time.");
#else
    if (g_installedHooks == 0) {
        if (!g_initRan) {
            Log("[DataStore] not measured: switched off. The six CDataStore "
                "accessors go in under Combat_Net/SavedVarsPretoken, which does "
                "not name them.");
        } else {
            Log("[DataStore] NOT active: asked to hook the six CDataStore "
                "accessors and none of them went in. The reason is earlier in "
                "this log.");
        }
        return;
    }
    if (g_getdword_calls == 0 && g_putdword_calls == 0 &&
        g_getbyte_calls == 0  && g_putbyte_calls == 0 &&
        g_getqword_calls == 0 && g_putqword_calls == 0) {
        Log("[DataStore] measured and zero: %u of 6 accessors hooked and the client "
            "read no packet field through them.", g_installedHooks);
        return;
    }
    {
        Log("[DataStore] Stats (%u of 6 accessors hooked; plain counters, so every "
            "figure is a lower bound):", g_installedHooks);
        if (g_getdword_calls > 0) Log("  GetDword: %ld/%ld hits (%ld%%)", g_getdword_hits, g_getdword_calls, (g_getdword_hits * 100) / g_getdword_calls);
        if (g_putdword_calls > 0) Log("  PutDword: %ld/%ld hits (%ld%%)", g_putdword_hits, g_putdword_calls, (g_putdword_hits * 100) / g_putdword_calls);
        if (g_getbyte_calls > 0)  Log("  GetByte:  %ld/%ld hits (%ld%%)", g_getbyte_hits, g_getbyte_calls, (g_getbyte_hits * 100) / g_getbyte_calls);
        if (g_putbyte_calls > 0)  Log("  PutByte:  %ld/%ld hits (%ld%%)", g_putbyte_hits, g_putbyte_calls, (g_putbyte_hits * 100) / g_putbyte_calls);
        if (g_getqword_calls > 0) Log("  GetQword: %ld/%ld hits (%ld%%)", g_getqword_hits, g_getqword_calls, (g_getqword_hits * 100) / g_getqword_calls);
        if (g_putqword_calls > 0) Log("  PutQword: %ld/%ld hits (%ld%%)", g_putqword_hits, g_putqword_calls, (g_putqword_hits * 100) / g_putqword_calls);
    }
#endif
}

void DumpDataStoreStats() {
    LogDataStoreStats();
}

void ShutdownDataStoreFastPath() {
#if !TEST_DISABLE_DATASTORE_FASTPATH
    MH_DisableHook((void*)0x0047B3C0);
    MH_DisableHook((void*)0x0047B0A0);
    MH_DisableHook((void*)0x0047B340);
    MH_DisableHook((void*)0x0047AFE0);
    MH_DisableHook((void*)0x0047B100);
    MH_DisableHook((void*)0x0047B400);
    DumpDataStoreStats();
#endif
}
