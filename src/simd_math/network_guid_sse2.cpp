// ============================================================================
// Description: Vectorized and branchless network GUID parser hook (CDataStore::GetWowGUID at 0x0076DC20).
// Safety & Threading: Thread-safe. Range checks must validate pointers up to 0xFFE00000 under LAA.
// ============================================================================

#pragma region System & Core Includes
#include "network_guid_sse2.h"
#include <emmintrin.h>
#include <windows.h>
#include <MinHook.h>
#include "core/version.h"
#pragma endregion

extern "C" void Log(const char* fmt, ...);

#pragma region Local Data Types & Structs

struct CDataStore {
    void**   vtable;         // +0x00: vtable pointer
    uint8_t* buffer;         // +0x04: data buffer base
    uint32_t base_offset;    // +0x08: subtracted from buffer ptr for effective addr
    uint32_t allocated_size; // +0x0C: allocated capacity in bytes
    uint32_t write_pos;      // +0x10: absolute write position
    uint32_t read_pos;       // +0x14: absolute read position
};
#pragma endregion

#pragma region Static Lookups & Variables
static uint8_t g_popcntLut[256];
static bool g_guidTablesInit = false;

typedef CDataStore* (__cdecl* GetWowGUID_t)(CDataStore*, uint64_t*);
static GetWowGUID_t pOrigGetWowGUID = nullptr;
#pragma endregion

#pragma region Internal Acceleration Logic
void InitNetworkGuidSSE2() {
    if (g_guidTablesInit) return;
    
    for (int i = 0; i < 256; ++i) {
        uint8_t count = 0;
        for (int b = 0; b < 8; ++b) {
            if (i & (1 << b)) count++;
        }
        g_popcntLut[i] = count;
    }
    g_guidTablesInit = true;
}

uint32_t FastUnpackGuidSSE2(const uint8_t* buffer, uint32_t remaining, uint64_t* out_guid) {
    if (remaining == 0) {
        *out_guid = 0;
        return 0;
    }
    
    uint8_t mask = buffer[0];
    if (mask == 0) {
        *out_guid = 0;
        return 1;
    }
    
    uint8_t count = g_popcntLut[mask];
    if (remaining < 1u + count) {
        *out_guid = 0;
        return 0; // Error, buffer underflow
    }
    
    uint64_t guid = 0;
    uint32_t pos = 1;
    
    if (mask & 1) guid |= ((uint64_t)buffer[pos++]) << 0;
    if (mask & 2) guid |= ((uint64_t)buffer[pos++]) << 8;
    if (mask & 4) guid |= ((uint64_t)buffer[pos++]) << 16;
    if (mask & 8) guid |= ((uint64_t)buffer[pos++]) << 24;
    if (mask & 16) guid |= ((uint64_t)buffer[pos++]) << 32;
    if (mask & 32) guid |= ((uint64_t)buffer[pos++]) << 40;
    if (mask & 64) guid |= ((uint64_t)buffer[pos++]) << 48;
    if (mask & 128) guid |= ((uint64_t)buffer[pos++]) << 56;
    
    *out_guid = guid;
    return 1 + count;
}
#pragma endregion

#pragma region API Hook Intercepts

CDataStore* __cdecl Hooked_GetWowGUID(CDataStore* self, uint64_t* outGuid) {
    __try {
        if (self && outGuid && (uintptr_t)self >= 0x10000 && (uintptr_t)self < 0xFFE00000 &&
            (uintptr_t)outGuid >= 0x10000 && (uintptr_t)outGuid < 0xFFE00000) {
            
            uint8_t* effective_base = self->buffer - self->base_offset;
            uint32_t rp = self->read_pos;
            uint32_t wp = self->write_pos;

            // A CDataStore is a WINDOW onto a larger stream, not a flat buffer.
            // The engine's own bounds check (sub_47B290) demands two things before
            // touching memory: that the read fits within the total size, and that
            // it lies inside the currently mapped window
            // [base_offset, base_offset + allocated_size). When it does not, the
            // engine calls a virtual method to fetch the next window.
            //
            // This fast path only checked the first. For a store whose window does
            // not cover the whole stream - which is what a segmented packet is -
            // a position past the window still passed, and the read came from
            // buffer - base_offset + rp: memory outside the mapping. read_pos was
            // then advanced over it, so every field after that GUID in the packet
            // was parsed from the wrong place.
            uint32_t winEnd = self->base_offset + self->allocated_size;
            if (winEnd > wp) winEnd = wp;

            if (rp >= self->base_offset && rp < winEnd) {
                uint32_t remaining = winEnd - rp;
                uint32_t bytesRead = FastUnpackGuidSSE2(effective_base + rp, remaining, outGuid);
                if (bytesRead > 0) {
                    self->read_pos = rp + bytesRead;
                    return self;
                }
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    return pOrigGetWowGUID(self, outGuid);
}
#pragma endregion

#pragma region Hook Initialization
bool InstallNetworkGuidSSE2Hooks() {
    InitNetworkGuidSSE2();
    
#if !TEST_DISABLE_NETWORK_GUID_SSE2
    Log("[NetworkGuid] Hooking CDataStore::GetWowGUID at 0x0076DC20");
    if (WineSafe_CreateHook((void*)0x0076DC20, (void*)Hooked_GetWowGUID, (void**)&pOrigGetWowGUID) != MH_OK) {
        Log("[NetworkGuid] Failed to create hook for GetWowGUID");
        return false;
    }
    if (WO_EnableHook((void*)0x0076DC20) != MH_OK) {
        Log("[NetworkGuid] Failed to enable hook for GetWowGUID");
        return false;
    }
    Log("[NetworkGuid] SSE2 GUID unpacking hook ACTIVE");
#endif

    return true;
}
#pragma endregion
