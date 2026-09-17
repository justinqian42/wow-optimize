// ============================================================================
// Description: Lock-Free Read-Copy-Update (RCU) Shadow Object Manager Cache
// Safety & Threading: Frame-boundary snapshotting without list mutation hooks.
// ============================================================================

#include "rcu_obj_mgr.h"
#include "core/config.h"
#include "MinHook.h"
#include "version.h"
#include <windows.h>
#include <atomic>
#include "win_mutex.h"
#include <intrin.h>

extern "C" void Log(const char* fmt, ...);

namespace RcuObjMgr {

struct RcuObjectArray {
    uint32_t count;
    void* objects[2048];
};

static std::atomic<RcuObjectArray*> g_rcuArray{nullptr};
static std::atomic<RcuObjectArray*> g_oldArrays[16]{nullptr};

typedef int (__cdecl *ClntObjMgrEnum_fn)(int (__cdecl *callback)(uint32_t, uint32_t, int), int context);
static ClntObjMgrEnum_fn orig_ClntObjMgrEnum = nullptr;

typedef void* (__thiscall *GetObjectByGUID_fn)(void* pThis, uint64_t guid);
static GetObjectByGUID_fn orig_GetObjectByGUID = nullptr;

inline void* GetActiveObjMgr() {
    // fs:[0x2C] is already the array of TLS blocks; the extra dereference this
    // used to have read its first entry instead. Corrected here for the same
    // reason it was corrected in objmgr_enum_fast, though this module stays off
    // for the snapshot reasons written in that one.
    uintptr_t** tls = (uintptr_t**)__readfsdword(0x2C);
    if (!tls) return nullptr;
    uint32_t* tlsIndexPtr = (uint32_t*)0x00D439BC;
    if (!tlsIndexPtr) return nullptr;
    uint32_t tlsIndex = *tlsIndexPtr;
    uintptr_t* tlsBlock = (uintptr_t*)tls[tlsIndex];
    if (!tlsBlock) return nullptr;
    return (void*)tlsBlock[2]; // Offset 8
}

void UpdateRcuArray(void* objMgr) {
    if (!objMgr) return;

    RcuObjectArray* newArray = new RcuObjectArray();
    newArray->count = 0;

    __try {
        uintptr_t firstObj = *(uintptr_t*)((char*)objMgr + 172);
        uintptr_t linkOffset = *(uintptr_t*)((char*)objMgr + 164);
        uintptr_t current = firstObj;

        while (current && (current & 1) == 0 && (uintptr_t)current >= 0x10000 && (uintptr_t)current < 0xFFE00000) {
            if (newArray->count < 2048) {
                newArray->objects[newArray->count++] = (void*)current;
            } else {
                break;
            }
            current = *(uintptr_t*)(current + linkOffset + 4);
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        delete newArray;
        return;
    }

    RcuObjectArray* oldArray = g_rcuArray.exchange(newArray, std::memory_order_release);

    if (oldArray) {
        bool queued = false;
        for (int i = 0; i < 16; i++) {
            RcuObjectArray* expected = nullptr;
            if (g_oldArrays[i].compare_exchange_strong(expected, oldArray)) {
                queued = true;
                break;
            }
        }
        if (!queued) {
            delete oldArray;
        }
    }
}

static int __cdecl Hooked_ClntObjMgrEnum(int (__cdecl *callback)(uint32_t, uint32_t, int), int context) {
    RcuObjectArray* arr = g_rcuArray.load(std::memory_order_acquire);
    if (arr) {
        for (uint32_t i = 0; i < arr->count; i++) {
            void* obj = arr->objects[i];
            if (obj && (uintptr_t)obj >= 0x10000 && (uintptr_t)obj < 0xFFE00000) {
                __try {
                    uint32_t* j = (uint32_t*)obj;
                    uint32_t guidLow = j[12];
                    uint32_t guidHigh = j[13];
                    if (!callback(guidLow, guidHigh, context)) {
                        return 0;
                    }
                }
                __except (EXCEPTION_EXECUTE_HANDLER) {}
            }
        }
        return 1;
    }
    return orig_ClntObjMgrEnum(callback, context);
}

static void* __fastcall Hooked_GetObjectByGUID(void* pThis, void* unused, uint64_t guid) {
    if (guid == 0 || !pThis) return nullptr;

    __try {
        RcuObjectArray* arr = g_rcuArray.load(std::memory_order_acquire);
        if (arr) {
            uint32_t targetLow = (uint32_t)guid;
            uint32_t targetHigh = (uint32_t)(guid >> 32);
            for (uint32_t i = 0; i < arr->count; i++) {
                void* obj = arr->objects[i];
                if (obj && (uintptr_t)obj >= 0x10000 && (uintptr_t)obj < 0xFFE00000) {
                    uint32_t* j = (uint32_t*)obj;
                    if (j[12] == targetLow && j[13] == targetHigh) {
                        return obj;
                    }
                }
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {}

    return orig_GetObjectByGUID ? orig_GetObjectByGUID(pThis, guid) : nullptr;
}

bool Init() {
    if (!Config::g_settings.OptRcuObjMgr) {
        Log("[RcuObjMgr] DISABLED via configuration");
        return true;
    }
    Log("[RcuObjMgr] Init");

    for (int i = 0; i < 16; i++) {
        g_oldArrays[i].store(nullptr);
    }
    g_rcuArray.store(nullptr);

    void* enumTarget = (void*)0x004D4B30;
    void* getObjTarget = (void*)0x006792E0;

    if (WineSafe_CreateHook(enumTarget, (void*)Hooked_ClntObjMgrEnum, (void**)&orig_ClntObjMgrEnum) == MH_OK) {
        WO_EnableHook(enumTarget);
    }

    if (WineSafe_CreateHook(getObjTarget, (void*)Hooked_GetObjectByGUID, (void**)&orig_GetObjectByGUID) == MH_OK) {
        if (WO_EnableHook(getObjTarget) == MH_OK) {
            Log("[RcuObjMgr] GetObjectByGUID hook at 0x006792E0 ACTIVE (frame-boundary RCU)");
        }
    }

    Log("[RcuObjMgr] Active - Lock-free entity traverser initialized");
    return true;
}

void Shutdown() {
    if (!Config::g_settings.OptRcuObjMgr) return;
    void* enumTarget = (void*)0x004D4B30;
    void* getObjTarget = (void*)0x006792E0;

    MH_DisableHook(enumTarget);
    MH_DisableHook(getObjTarget);

    RcuObjectArray* arr = g_rcuArray.exchange(nullptr);
    if (arr) delete arr;

    for (int i = 0; i < 16; i++) {
        RcuObjectArray* old = g_oldArrays[i].exchange(nullptr);
        if (old) delete old;
    }
}

void OnFrame() {
    if (!Config::g_settings.OptRcuObjMgr) return;

    void* activeMgr = GetActiveObjMgr();
    if (activeMgr) {
        UpdateRcuArray(activeMgr);
    }

    for (int i = 0; i < 16; i++) {
        RcuObjectArray* old = g_oldArrays[i].exchange(nullptr);
        if (old) {
            delete old;
        }
    }
}

void UpdateActiveRcuArray() {
    if (!Config::g_settings.OptRcuObjMgr) return;
    void* activeMgr = GetActiveObjMgr();
    if (activeMgr) {
        UpdateRcuArray(activeMgr);
    }
}

} // namespace RcuObjMgr
