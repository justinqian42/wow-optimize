#include <windows.h>
#include <cstdint>
#include "MinHook.h"
#include "version.h"
#include "d3d_evict_patch.h"

extern "C" void Log(const char* fmt, ...);

typedef void (__fastcall *EvictCheck_t)(void* ecx, void* edx);
static EvictCheck_t g_origEvictD3d = nullptr;
static EvictCheck_t g_origEvictD3dEx = nullptr;

static void __fastcall Hooked_EvictD3d(void* ecx, void* edx)
{
    __try {
        if (g_origEvictD3d) {
            g_origEvictD3d(ecx, edx);
        }
    } __except(EXCEPTION_EXECUTE_HANDLER) {}
}

static void __fastcall Hooked_EvictD3dEx(void* ecx, void* edx)
{
    __try {
        if (g_origEvictD3dEx) {
            g_origEvictD3dEx(ecx, edx);
        }
    } __except(EXCEPTION_EXECUTE_HANDLER) {}
}

bool InstallD3DEvictPatch()
{
    bool ok = true;

    // CGxD3dDevice::EvictManagedResources check (0x68E450)
    void* t1 = (void*)0x0068E450;
    if (MH_CreateHook(t1, (void*)Hooked_EvictD3d, (void**)&g_origEvictD3d) == MH_OK) {
        if (MH_EnableHook(t1) == MH_OK) {
            Log("[D3DEvict] D3D9 EvictManagedResources check suppressed");
        } else { ok = false; Log("[D3DEvict] D3D9 enable FAILED"); }
    } else { ok = false; Log("[D3DEvict] D3D9 CreateHook FAILED"); }

    // CGxD3d9ExDevice::EvictManagedResources check (0x69FDC0)
    void* t2 = (void*)0x0069FDC0;
    if (MH_CreateHook(t2, (void*)Hooked_EvictD3dEx, (void**)&g_origEvictD3dEx) == MH_OK) {
        if (MH_EnableHook(t2) == MH_OK) {
            Log("[D3DEvict] D3D9Ex EvictManagedResources check suppressed");
        } else { ok = false; Log("[D3DEvict] D3D9Ex enable FAILED"); }
    } else { ok = false; Log("[D3DEvict] D3D9Ex CreateHook FAILED"); }

    return ok;
}
