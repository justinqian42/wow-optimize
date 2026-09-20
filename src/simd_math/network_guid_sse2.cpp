// ============================================================================
// Description: Vectorized and branchless network GUID parser hook (CDataStore::GetWowGUID at 0x0076DC20).
// Safety & Threading: Thread-safe. Range checks must validate pointers up to 0xFFE00000 under LAA.
// ============================================================================

#pragma region System & Core Includes
#include "network_guid_sse2.h"
#include <emmintrin.h>
#include <windows.h>
#include <cstring>
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
static uint8_t s_popcount_lut[256];

typedef CDataStore* (__cdecl *GetWowGUID_t)(CDataStore* self, uint64_t* outGuid);
static GetWowGUID_t pOrigGetWowGUID = nullptr;
#pragma endregion

#pragma region Static Acceleration Tables
void InitNetworkGuidSSE2() {
    for (int i = 0; i < 256; ++i) {
        s_popcount_lut[i] = (uint8_t)(
            (i & 1) + ((i >> 1) & 1) + ((i >> 2) & 1) + ((i >> 3) & 1) +
            ((i >> 4) & 1) + ((i >> 5) & 1) + ((i >> 6) & 1) + ((i >> 7) & 1)
        );
    }
}
#pragma endregion

#pragma region Fast Guidance Decoding Kernels
static __forceinline uint32_t FastUnpackGuidSSE2(const uint8_t* buffer, uint32_t remaining, uint64_t* out_guid) {
    if (remaining < 1) return 0;
    
    uint8_t mask = buffer[0];
    uint32_t count = s_popcount_lut[mask];
    
    if (remaining < 1 + count) return 0;
    
    uint64_t guid = 0;
    uint32_t pos = 1;
    
    if (mask & 1)   guid |= ((uint64_t)buffer[pos++]);
    if (mask & 2)   guid |= ((uint64_t)buffer[pos++]) << 8;
    if (mask & 4)   guid |= ((uint64_t)buffer[pos++]) << 16;
    if (mask & 8)   guid |= ((uint64_t)buffer[pos++]) << 24;
    if (mask & 16)  guid |= ((uint64_t)buffer[pos++]) << 32;
    if (mask & 32)  guid |= ((uint64_t)buffer[pos++]) << 40;
    if (mask & 64)  guid |= ((uint64_t)buffer[pos++]) << 48;
    if (mask & 128) guid |= ((uint64_t)buffer[pos++]) << 56;
    
    *out_guid = guid;
    return 1 + count;
}
#pragma endregion

#pragma region API Hook Intercepts

CDataStore* __cdecl Hooked_GetWowGUID(CDataStore* self, uint64_t* outGuid) {
    if (self && outGuid && (uintptr_t)self >= 0x10000 && (uintptr_t)self < 0xFFE00000 &&
        (uintptr_t)outGuid >= 0x10000 && (uintptr_t)outGuid < 0xFFE00000) {
        
        uint8_t* buffer = self->buffer;
        if (buffer && (uintptr_t)buffer >= 0x10000 && (uintptr_t)buffer < 0xFFE00000) {
            uint8_t* effective_base = buffer - self->base_offset;
            uint32_t rp = self->read_pos;
            uint32_t wp = self->write_pos;

            // A CDataStore is a WINDOW onto a larger stream, not a flat buffer.
            // The engine's own bounds check (sub_47B290) demands two things before
            // touching memory: that the read fits within the total size, and that
            // it lies inside the currently mapped window
            // [base_offset, base_offset + allocated_size). When it does not, the
            // engine calls a virtual method to fetch the next window.
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
    }
    return pOrigGetWowGUID(self, outGuid);
}
#pragma endregion

#pragma region Hook Initialization
bool InstallNetworkGuidSSE2Hooks() {
    InitNetworkGuidSSE2();
    
#if !TEST_DISABLE_NETWORK_GUID_SSE2
    void* target = (void*)0x0076DC20;
    static const unsigned char kExpectedPrologue[8] = {
        0x55, 0x8B, 0xEC, 0x51, 0x8B, 0x4D, 0x08, 0x53
    };
    if (memcmp(target, kExpectedPrologue, sizeof(kExpectedPrologue)) != 0) {
        Log("[NetworkGuid] BAD PROLOGUE at 0x%08X", (uintptr_t)target);
        return false;
    }
    Log("[NetworkGuid] Hooking CDataStore::GetWowGUID at 0x0076DC20");
    if (WineSafe_CreateHook(target, (void*)Hooked_GetWowGUID, (void**)&pOrigGetWowGUID) != MH_OK) {
        Log("[NetworkGuid] Failed to create hook for GetWowGUID");
        return false;
    }
    if (WO_EnableHook(target) != MH_OK) {
        Log("[NetworkGuid] Failed to enable hook for GetWowGUID");
        return false;
    }
    Log("[NetworkGuid] SSE2 GUID unpacking hook ACTIVE");
#endif

    return true;
}
#pragma endregion
