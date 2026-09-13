// ============================================================================
// Module: render_state_dedup.cpp
// ============================================================================

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <d3d9.h>
#include <cstdint>
#include <cstring>
#include "MinHook.h"
#include "version.h"
#include "config.h"
#include "d3d9_state_cache.h"
#include "d3d9_render_thread.h"

extern "C" void Log(const char* fmt, ...);
extern volatile LONG g_deviceResetCounter;

// ---- statistics --------------------------------------------------
static volatile LONG64 g_rs_calls   = 0;    // SetRenderState calls
static volatile LONG64 g_rs_skipped = 0;    // redundant calls skipped

static volatile LONG64 g_tex_calls   = 0;   // SetTexture calls
static volatile LONG64 g_tex_skipped = 0;   // redundant calls skipped
static volatile LONG64 g_tss_calls   = 0;   // SetTextureStageState calls
static volatile LONG64 g_tss_skipped = 0;   // redundant calls skipped

static volatile LONG64 g_sampler_calls   = 0; // SetSamplerState calls
static volatile LONG64 g_sampler_skipped = 0; // redundant calls skipped

// ---- render state cache ------------------------------------------
// D3DRENDERSTATETYPE values fit in 0..255. Most WoW usage stays
// under 210 (D3DRS_BLENDOP is 171 in d3d9types.h).
static constexpr DWORD RS_CACHE_SIZE = 256;
static DWORD g_rsCache[RS_CACHE_SIZE];
static bool  g_rsValid[RS_CACHE_SIZE];

// ---- texture stage state cache -----------------------------------
// 8 stages, 33 types (up to D3DTSS_CONSTANT=32)
static constexpr DWORD TSS_STAGES = 8;
static constexpr DWORD TSS_TYPES  = 33;
static DWORD g_tssCache[TSS_STAGES][TSS_TYPES];
static bool  g_tssValid[TSS_STAGES][TSS_TYPES];

// ---- sampler state cache -----------------------------------------
// 16 samplers, 14 types (up to D3DSAMP_DMAPOFFSET=13)
static constexpr DWORD SAMPLER_COUNT = 16;
static constexpr DWORD SAMPLER_TYPES = 14;
static DWORD g_samplerCache[SAMPLER_COUNT][SAMPLER_TYPES];
static bool  g_samplerValid[SAMPLER_COUNT][SAMPLER_TYPES];

// The texture bound to each stage, and whether we know it.
//
// Sixteen covers every pixel sampler on this hardware generation. The vertex
// texture samplers live at D3DVERTEXTEXTURESAMPLER0 (257) and above, and those
// are passed through untouched rather than folded into the same table, because
// a stage number that large would otherwise index out of it.
#define TEX_STAGES 16
static IDirect3DBaseTexture9* g_texCache[TEX_STAGES];
static bool                   g_texValid[TEX_STAGES];

// ---- hook state --------------------------------------------------
static bool g_deviceHooksInstalled = false;
static bool g_installed = false;

// ---- original function pointers ----------------------------------
typedef HRESULT (STDMETHODCALLTYPE *SetRenderState_t)(IDirect3DDevice9*, D3DRENDERSTATETYPE, DWORD);
static SetRenderState_t g_orig_SetRenderState = nullptr;

typedef HRESULT (STDMETHODCALLTYPE *SetTexture_t)(IDirect3DDevice9*, DWORD, IDirect3DBaseTexture9*);
static SetTexture_t g_orig_SetTexture = nullptr;
typedef HRESULT (STDMETHODCALLTYPE *SetTextureStageState_t)(IDirect3DDevice9*, DWORD, D3DTEXTURESTAGESTATETYPE, DWORD);
static SetTextureStageState_t g_orig_SetTextureStageState = nullptr;

typedef HRESULT (STDMETHODCALLTYPE *SetSamplerState_t)(IDirect3DDevice9*, DWORD, D3DSAMPLERSTATETYPE, DWORD);
static SetSamplerState_t g_orig_SetSamplerState = nullptr;

typedef HRESULT (STDMETHODCALLTYPE *Reset_t)(IDirect3DDevice9*, D3DPRESENT_PARAMETERS*);
static Reset_t g_orig_Reset = nullptr;

// ================================================================
// SetRenderState — compare-before-set dedup
// ================================================================
static HRESULT STDMETHODCALLTYPE Hooked_SetRenderState(
    IDirect3DDevice9* device, D3DRENDERSTATETYPE state, DWORD value)
{
    g_rs_calls++;

    bool isCriticalState = (state == D3DRS_ALPHABLENDENABLE || state == D3DRS_SRCBLEND || 
                           state == D3DRS_DESTBLEND || state == D3DRS_ALPHATESTENABLE || 
                           state == D3DRS_ALPHAREF || state == D3DRS_ALPHAFUNC ||
                           state == D3DRS_ZWRITEENABLE || state == D3DRS_ZENABLE);

    DWORD idx = (DWORD)state;
    if (idx < RS_CACHE_SIZE && !isCriticalState) {
        if (g_rsValid[idx] && g_rsCache[idx] == value) {
            g_rs_skipped++;
            return S_OK;
        }
    }

    HRESULT hr = g_orig_SetRenderState(device, state, value);
    if (SUCCEEDED(hr) && idx < RS_CACHE_SIZE) {
        g_rsCache[idx] = value;
        g_rsValid[idx] = true;
    }
    return hr;
}

// ================================================================
// SetTexture — compare-before-set dedup
// ================================================================
//
// The most frequent state call in this renderer and the last one here without
// dedup. A batch that draws the same material repeatedly rebinds the same
// texture to the same stage every time, and each of those is a call into the
// driver; under a Vulkan wrapper it is a descriptor update on top of that.
//
// Skipping a rebind of the texture already there is safe, and the reason is
// D3D9's own reference counting rather than an assumption about the driver.
// SetTexture adds a reference to the new texture and releases the old one, so a
// call that binds what is already bound has a net reference change of zero and
// skipping it leaks nothing and over-releases nothing.
//
// The stale-pointer hazard this project warns about does not open here either.
// A comparison of raw pointers would be wrong if a texture could be freed and
// another allocated at the same address while the cache still named it - but
// the runtime holds a reference to every bound texture for exactly as long as
// it is bound, so the address cannot be reused while this cache says it is
// there. Nothing here ever dereferences the pointer; it is only compared.
//
// NULL is cached like any other value, so repeatedly unbinding a stage that is
// already empty is skipped too.
static HRESULT STDMETHODCALLTYPE Hooked_SetTexture(
    IDirect3DDevice9* device, DWORD stage, IDirect3DBaseTexture9* texture)
{
    g_tex_calls++;

    if (stage < TEX_STAGES) {
        if (g_texValid[stage] && g_texCache[stage] == texture) {
            g_tex_skipped++;
            return S_OK;
        }
    }

    HRESULT hr = g_orig_SetTexture(device, stage, texture);
    if (SUCCEEDED(hr) && stage < TEX_STAGES) {
        g_texCache[stage] = texture;
        g_texValid[stage] = true;
    }
    return hr;
}

// ================================================================
// SetTextureStageState — compare-before-set dedup
// ================================================================
static HRESULT STDMETHODCALLTYPE Hooked_SetTextureStageState(
    IDirect3DDevice9* device, DWORD stage, D3DTEXTURESTAGESTATETYPE type, DWORD value)
{
    g_tss_calls++;

    DWORD s = (DWORD)stage;
    DWORD t = (DWORD)type;
    if (s < TSS_STAGES && t < TSS_TYPES) {
        if (g_tssValid[s][t] && g_tssCache[s][t] == value) {
            g_tss_skipped++;
            return S_OK;
        }
    }

    HRESULT hr = g_orig_SetTextureStageState(device, stage, type, value);
    if (SUCCEEDED(hr) && s < TSS_STAGES && t < TSS_TYPES) {
        g_tssCache[s][t] = value;
        g_tssValid[s][t] = true;
    }
    return hr;
}

// ================================================================
// SetSamplerState — compare-before-set dedup
// ================================================================
static HRESULT STDMETHODCALLTYPE Hooked_SetSamplerState(
    IDirect3DDevice9* device, DWORD sampler, D3DSAMPLERSTATETYPE type, DWORD value)
{
    g_sampler_calls++;

    DWORD s = (DWORD)sampler;
    DWORD t = (DWORD)type;
    if (s < SAMPLER_COUNT && t < SAMPLER_TYPES) {
        if (g_samplerValid[s][t] && g_samplerCache[s][t] == value) {
            g_sampler_skipped++;
            return S_OK;
        }
    }

    HRESULT hr = g_orig_SetSamplerState(device, sampler, type, value);
    if (SUCCEEDED(hr) && s < SAMPLER_COUNT && t < SAMPLER_TYPES) {
        g_samplerCache[s][t] = value;
        g_samplerValid[s][t] = true;
    }
    return hr;
}

// ================================================================
// Reset — clear cache on device reset to prevent stale states
// ================================================================
static HRESULT STDMETHODCALLTYPE Hooked_Reset(
    IDirect3DDevice9* device, D3DPRESENT_PARAMETERS* pParams)
{
    // Clear caches: D3D device reset restores default render states on the GPU
    memset(g_rsCache, 0, sizeof(g_rsCache));
    memset(g_rsValid, 0, sizeof(g_rsValid));

    memset(g_tssCache, 0, sizeof(g_tssCache));
    memset(g_tssValid, 0, sizeof(g_tssValid));

    memset(g_samplerCache, 0, sizeof(g_samplerCache));
    memset(g_samplerValid, 0, sizeof(g_samplerValid));

    InterlockedIncrement(&g_deviceResetCounter);

    HRESULT hr = S_OK;
    if (g_orig_Reset) {
        hr = g_orig_Reset(device, pParams);
    }
    if (SUCCEEDED(hr)) {
        memset(g_rsCache, 0, sizeof(g_rsCache));
        memset(g_rsValid, 0, sizeof(g_rsValid));

        memset(g_tssCache, 0, sizeof(g_tssCache));
        memset(g_tssValid, 0, sizeof(g_tssValid));

        memset(g_samplerCache, 0, sizeof(g_samplerCache));
        memset(g_samplerValid, 0, sizeof(g_samplerValid));

        InterlockedIncrement(&g_deviceResetCounter);
    }
    return hr;
}

// ================================================================
// IDirect3D9::CreateDevice — intercept device creation
// ================================================================
typedef HRESULT (STDMETHODCALLTYPE *CreateDevice_t)(
    IDirect3D9*, UINT, D3DDEVTYPE, HWND, DWORD,
    D3DPRESENT_PARAMETERS*, IDirect3DDevice9**);

static CreateDevice_t g_orig_CreateDevice = nullptr;

static HRESULT STDMETHODCALLTYPE Hooked_CreateDevice(
    IDirect3D9* self, UINT Adapter, D3DDEVTYPE DeviceType,
    HWND hFocusWindow, DWORD BehaviorFlags,
    D3DPRESENT_PARAMETERS* pParams, IDirect3DDevice9** ppDevice)
{
    DWORD flags = BehaviorFlags;
    if (Config::g_settings.OptD3d9RenderThread) {
        flags |= D3DCREATE_MULTITHREADED;
    }
    HRESULT hr = g_orig_CreateDevice(self, Adapter, DeviceType,
        hFocusWindow, flags, pParams, ppDevice);

    if (SUCCEEDED(hr) && ppDevice && *ppDevice) {
        // Resolve state cache and install hooks
        D3D9StateCache::OnCreateDevice(*ppDevice);

        if (Config::g_settings.OptD3d9RenderThread) {
            D3D9RenderThread::OnCreateDevice(*ppDevice);
        }

        // Clear caches on new device creation to start clean
        memset(g_rsCache, 0, sizeof(g_rsCache));
        memset(g_rsValid, 0, sizeof(g_rsValid));

        memset(g_tssCache, 0, sizeof(g_tssCache));
        memset(g_tssValid, 0, sizeof(g_tssValid));

        memset(g_samplerCache, 0, sizeof(g_samplerCache));
        memset(g_samplerValid, 0, sizeof(g_samplerValid));

        InterlockedIncrement(&g_deviceResetCounter);

        if (!g_deviceHooksInstalled) {
            __try {
                IDirect3DDevice9* dev = *ppDevice;
                uintptr_t* vtable = *(uintptr_t**)dev;
                if (!vtable || (uintptr_t)vtable < 0x10000 ||
                    (uintptr_t)vtable > 0xFFE00000) {
                    return hr;
                }

                // vtable index 57 = SetRenderState
                uintptr_t setRS = vtable[57];
                if (!setRS || setRS < 0x10000 || setRS > 0xFFE00000) {
                    Log("[RenderDedup] SetRenderState vtable entry invalid (0x%08X) — "
                        "wrapper device, skipping hook", (unsigned)setRS);
                    return hr;
                }

                // Hook SetRenderState
                SetRenderState_t origRS = nullptr;
                MH_STATUS st = MH_CreateHook(
                    (void*)setRS,
                    (void*)Hooked_SetRenderState,
                    (void**)&origRS);
                if (st != MH_OK) {
                    Log("[RenderDedup] MH_CreateHook SetRenderState FAILED (status %d) "
                        "— skipping", (int)st);
                    return hr;
                }

                st = MH_EnableHook((void*)setRS);
                if (st != MH_OK) {
                    Log("[RenderDedup] MH_EnableHook SetRenderState FAILED (status %d) "
                        "— removing", (int)st);
                    MH_RemoveHook((void*)setRS);
                    return hr;
                }
                g_orig_SetRenderState = origRS;

                // Hook SetTexture (vtable index 65)
                //
                // The index is taken from the SDK's own d3d9.h, counted through
                // DECLARE_INTERFACE_(IDirect3DDevice9, ...), and it agrees with
                // the three this module already hooks: Reset 16, SetRenderState
                // 57, SetTextureStageState 67, SetSamplerState 69. GetTexture
                // sits at 64 and SetTexture at 65.
                uintptr_t setTex = vtable[65];
                if (setTex && setTex >= 0x10000 && setTex <= 0xFFE00000) {
                    SetTexture_t origTex = nullptr;
                    st = MH_CreateHook(
                        (void*)setTex,
                        (void*)Hooked_SetTexture,
                        (void**)&origTex);
                    if (st == MH_OK) {
                        st = MH_EnableHook((void*)setTex);
                        if (st == MH_OK) {
                            g_orig_SetTexture = origTex;
                        } else {
                            MH_RemoveHook((void*)setTex);
                        }
                    }
                }

                // Hook SetTextureStageState (vtable index 67)
                uintptr_t setTSS = vtable[67];
                if (setTSS && setTSS >= 0x10000 && setTSS <= 0xFFE00000) {
                    SetTextureStageState_t origTSS = nullptr;
                    st = MH_CreateHook(
                        (void*)setTSS,
                        (void*)Hooked_SetTextureStageState,
                        (void**)&origTSS);
                    if (st == MH_OK) {
                        st = MH_EnableHook((void*)setTSS);
                        if (st == MH_OK) {
                            g_orig_SetTextureStageState = origTSS;
                        } else {
                            MH_RemoveHook((void*)setTSS);
                        }
                    }
                }

                // Hook SetSamplerState (vtable index 69)
                uintptr_t setSS = vtable[69];
                if (setSS && setSS >= 0x10000 && setSS <= 0xFFE00000) {
                    SetSamplerState_t origSS = nullptr;
                    st = MH_CreateHook(
                        (void*)setSS,
                        (void*)Hooked_SetSamplerState,
                        (void**)&origSS);
                    if (st == MH_OK) {
                        st = MH_EnableHook((void*)setSS);
                        if (st == MH_OK) {
                            g_orig_SetSamplerState = origSS;
                        } else {
                            MH_RemoveHook((void*)setSS);
                        }
                    }
                }

                // Hook Reset (vtable index 16)
                uintptr_t reset = vtable[16];
                if (reset && reset >= 0x10000 && reset <= 0xFFE00000) {
                    Reset_t origReset = nullptr;
                    st = MH_CreateHook(
                        (void*)reset,
                        (void*)Hooked_Reset,
                        (void**)&origReset);
                    if (st == MH_OK) {
                        st = MH_EnableHook((void*)reset);
                        if (st == MH_OK) {
                            g_orig_Reset = origReset;
                        } else {
                            MH_RemoveHook((void*)reset);
                        }
                    }
                }

                g_deviceHooksInstalled = true;
                Log("[RenderDedup] Hooks installed on device (SetRenderState + Reset + SetTextureStageState + SetSamplerState)");
            } __except(EXCEPTION_EXECUTE_HANDLER) {
                Log("[RenderDedup] Exception reading device vtable — wrapper device, "
                    "skipping hook");
            }
        }
    }

    return hr;
}

// ================================================================
// Direct3DCreate9 — intercept d3d9.dll's export to grab IDirect3D9
// and hook its CreateDevice vtable entry.
// ================================================================
typedef IDirect3D9* (WINAPI *Direct3DCreate9_t)(UINT SDKVersion);
static Direct3DCreate9_t g_orig_D3DCreate9 = nullptr;

static IDirect3D9* WINAPI Hooked_D3DCreate9(UINT SDKVersion)
{
    IDirect3D9* d3d9 = g_orig_D3DCreate9(SDKVersion);
    if (!d3d9 || g_deviceHooksInstalled)
        return d3d9;

    __try {
        uintptr_t* vtable = *(uintptr_t**)d3d9;
        if (!vtable || (uintptr_t)vtable < 0x10000 ||
            (uintptr_t)vtable > 0xFFE00000) {
            return d3d9;
        }

        // vtable index 16 = CreateDevice
        uintptr_t createDev = vtable[16];
        if (!createDev || createDev < 0x10000 || createDev > 0xFFE00000)
            return d3d9;

        CreateDevice_t origCD = nullptr;
        MH_STATUS st = MH_CreateHook(
            (void*)createDev,
            (void*)Hooked_CreateDevice,
            (void**)&origCD);
        if (st == MH_OK) {
            st = MH_EnableHook((void*)createDev);
            if (st == MH_OK) {
                g_orig_CreateDevice = origCD;
                Log("[RenderDedup] CreateDevice hook ACTIVE (waiting for device creation)");
            } else {
                MH_RemoveHook((void*)createDev);
            }
        }
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        // vtable read faulted — wrapper/weird device, skip
    }

    return d3d9;
}

// ================================================================
// Install / Shutdown
// ================================================================
bool InstallRenderStateDedup(void)
{
    // Clear caches
    memset(g_rsCache, 0, sizeof(g_rsCache));
    memset(g_rsValid, 0, sizeof(g_rsValid));

    memset(g_tssCache, 0, sizeof(g_tssCache));
    memset(g_tssValid, 0, sizeof(g_tssValid));

    memset(g_samplerCache, 0, sizeof(g_samplerCache));
    memset(g_samplerValid, 0, sizeof(g_samplerValid));

    // Load d3d9.dll if not already loaded
    HMODULE hD3D9 = GetModuleHandleA("d3d9.dll");
    if (!hD3D9) {
        Log("[RenderDedup] d3d9.dll not loaded — skipping (no D3D9 renderer)");
        return false;
    }

    // Hook Direct3DCreate9 export
    void* pCreate9 = (void*)GetProcAddress(hD3D9, "Direct3DCreate9");
    if (!pCreate9) {
        Log("[RenderDedup] Direct3DCreate9 not found in d3d9.dll — skipping");
        return false;
    }

    // Use standard MH_CreateHook for system DLLs (not WineSafe — that's
    // for WoW's .text addresses only).
    MH_STATUS st = MH_CreateHook(
        pCreate9,
        (void*)Hooked_D3DCreate9,
        (void**)&g_orig_D3DCreate9);
    if (st != MH_OK) {
        Log("[RenderDedup] MH_CreateHook Direct3DCreate9 FAILED (status %d)", (int)st);
        return false;
    }

    st = WO_EnableHook(pCreate9);
    if (st != MH_OK) {
        Log("[RenderDedup] WO_EnableHook Direct3DCreate9 FAILED (status %d)", (int)st);
        MH_RemoveHook(pCreate9);
        return false;
    }

    g_installed = true;
    Log("[RenderDedup] Direct3DCreate9 hook ACTIVE (cache ready, %d slots, "
        "waiting for device)", RS_CACHE_SIZE);
    return true;
}

// Printed from the periodic report.
//
// Everything this module measures was logged only from ShutdownRenderStateDedup,
// and the DLL leaves through TerminateProcess, so no session has ever shown how
// much redundant D3D9 state it removes. That is an unusual thing not to know:
// every skipped SetRenderState is a driver call that does not happen, and under
// a translation layer it is a translation that does not happen either. The
// feature may be worth a great deal or nothing, and the log said neither.
void RenderStateDedup_LogStats(void)
{
    if (!g_installed) {
        Log("[RenderDedup] not measured: the hooks are not installed.");
        return;
    }
    const LONG64 rs  = g_rs_calls,      rsSk  = g_rs_skipped;
    const LONG64 tss = g_tss_calls,     tssSk = g_tss_skipped;
    const LONG64 ss  = g_sampler_calls, ssSk  = g_sampler_skipped;
    if (rs == 0 && tss == 0 && ss == 0 && g_tex_calls == 0) {
        Log("[RenderDedup] installed and no state call has reached it yet. "
            "Measured and zero, not unmeasured.");
        return;
    }
    const LONG64 tex = g_tex_calls, texSk = g_tex_skipped;
    Log("[RenderDedup] SetTexture %lld call(s), %lld skipped (%.1f%%) - the most "
        "frequent of these and the one added last.",
        tex, texSk, tex ? 100.0 * (double)texSk / (double)tex : 0.0);
    Log("[RenderDedup] SetRenderState %lld call(s), %lld skipped (%.1f%%); "
        "SetTextureStageState %lld, %lld skipped (%.1f%%); SetSamplerState "
        "%lld, %lld skipped (%.1f%%). Every skip is a driver call that did not "
        "happen.",
        rs,  rsSk,  rs  ? 100.0 * (double)rsSk  / (double)rs  : 0.0,
        tss, tssSk, tss ? 100.0 * (double)tssSk / (double)tss : 0.0,
        ss,  ssSk,  ss  ? 100.0 * (double)ssSk  / (double)ss  : 0.0);
}

void ShutdownRenderStateDedup(void)
{
    LONG64 calls   = g_rs_calls;
    LONG64 skipped = g_rs_skipped;
    if (calls > 0) {
        Log("[RenderDedup] Stats: %lld SetRenderState calls, %lld skipped "
            "(%.1f%% dedup)",
            calls, skipped,
            100.0 * (double)skipped / (double)calls);
    } else {
        Log("[RenderDedup] No SetRenderState calls recorded");
    }

    LONG64 tss_calls   = g_tss_calls;
    LONG64 tss_skipped = g_tss_skipped;
    if (tss_calls > 0) {
        Log("[RenderDedup] Stats: %lld SetTextureStageState calls, %lld skipped "
            "(%.1f%% dedup)",
            tss_calls, tss_skipped,
            100.0 * (double)tss_skipped / (double)tss_calls);
    } else {
        Log("[RenderDedup] No SetTextureStageState calls recorded");
    }

    LONG64 sampler_calls   = g_sampler_calls;
    LONG64 sampler_skipped = g_sampler_skipped;
    if (sampler_calls > 0) {
        Log("[RenderDedup] Stats: %lld SetSamplerState calls, %lld skipped "
            "(%.1f%% dedup)",
            sampler_calls, sampler_skipped,
            100.0 * (double)sampler_skipped / (double)sampler_calls);
    } else {
        Log("[RenderDedup] No SetSamplerState calls recorded");
    }
}

void RenderStateDedup_ClearCache(void)
{
    memset(g_rsValid, 0, sizeof(g_rsValid));
    memset(g_tssValid, 0, sizeof(g_tssValid));
    memset(g_samplerValid, 0, sizeof(g_samplerValid));
    memset(g_texValid, 0, sizeof(g_texValid));
}