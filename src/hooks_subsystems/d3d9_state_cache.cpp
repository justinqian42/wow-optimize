// ============================================================================
// Description: Filters redundant D3D9 state modifications to reduce context switches.
// Safety & Threading: Main thread only. Invalidates cache on Reset().
// ============================================================================

#include "d3d9_state_cache.h"
#include "MinHook.h"
#include "version.h"
#include "mip_bias_governor.h"
#include <d3d9.h>
#include "d3d9_render_thread.h"
#include "config.h"
#include "dxvk_bridge.h"
#include "font_glyph_cache.h"
#include "vertex_buffer_prealloc.h"
#include "texture_unload_delay.h"
#include "d3d9_state_manager.h"
#include <atomic>

extern "C" void Log(const char* fmt, ...);
extern DWORD g_mainThreadId;
extern volatile LONG g_deviceResetCounter;
extern void RenderStateDedup_ClearCache(void);

namespace D3D9StateCache {

// Original function pointers


typedef HRESULT (WINAPI *SetRenderState_fn)(IDirect3DDevice9* device, D3DRENDERSTATETYPE state, DWORD value);
SetRenderState_fn orig_SetRenderState = nullptr;

typedef HRESULT (WINAPI *SetTransform_fn)(IDirect3DDevice9* device, D3DTRANSFORMSTATETYPE state, const D3DMATRIX* matrix);
SetTransform_fn orig_SetTransform = nullptr;

typedef HRESULT (WINAPI *SetViewport_fn)(IDirect3DDevice9* device, const D3DVIEWPORT9* viewport);
SetViewport_fn orig_SetViewport = nullptr;

typedef HRESULT (WINAPI *CreateVertexBuffer_fn)(IDirect3DDevice9* device, UINT Length, DWORD Usage, DWORD FVF, D3DPOOL Pool, IDirect3DVertexBuffer9** ppVertexBuffer, HANDLE* pSharedHandle);
static CreateVertexBuffer_fn orig_CreateVertexBuffer = nullptr;

typedef HRESULT (WINAPI *VB_Lock_fn)(IDirect3DVertexBuffer9* vb, UINT OffsetToLock, UINT SizeToLock, void** ppbData, DWORD Flags);
static VB_Lock_fn orig_VB_Lock = nullptr;

typedef HRESULT (WINAPI *VB_Unlock_fn)(IDirect3DVertexBuffer9* vb);
static VB_Unlock_fn orig_VB_Unlock = nullptr;

typedef HRESULT (WINAPI *SetVertexShaderConstantF_fn)(IDirect3DDevice9* device, UINT StartRegister, const float* pConstantData, UINT Vector4fCount);
SetVertexShaderConstantF_fn orig_SetVertexShaderConstantF = nullptr;

typedef HRESULT (WINAPI *SetVertexShader_fn)(IDirect3DDevice9* device, IDirect3DVertexShader9* shader);
static SetVertexShader_fn orig_SetVertexShader = nullptr;

typedef HRESULT (WINAPI *SetSamplerState_fn)(IDirect3DDevice9* device, DWORD Sampler, D3DSAMPLERSTATETYPE Type, DWORD Value);
SetSamplerState_fn orig_SetSamplerState = nullptr;

typedef HRESULT (WINAPI *SetTextureStageState_fn)(IDirect3DDevice9* device, DWORD Stage, D3DTEXTURESTAGESTATETYPE Type, DWORD Value);
SetTextureStageState_fn orig_SetTextureStageState = nullptr;

static bool g_vbHooksInstalled = false;



typedef HRESULT (WINAPI *Reset_fn)(IDirect3DDevice9* device, D3DPRESENT_PARAMETERS* params);
Reset_fn orig_Reset = nullptr;

typedef HRESULT (WINAPI *Present_fn)(IDirect3DDevice9* device, const RECT* src, const RECT* dest, HWND window, const RGNDATA* dirty);
Present_fn orig_Present = nullptr;

// Cache structures
static IDirect3DBaseTexture9* g_textureCache[16] = { nullptr };

static DWORD g_renderStateCache[512] = { 0 };
static bool g_renderStateValid[512] = { false };

static DWORD g_textureStageStateCache[8][64] = { {0} };
static bool g_textureStageStateValid[8][64] = { {false} };

static DWORD g_samplerStateCache[16][32] = { {0} };
static bool g_samplerStateValid[16][32] = { {false} };

struct CachedMatrix {
    D3DMATRIX matrix;
    bool valid;
};
static CachedMatrix g_transformCache[512] = { { {0}, false } };

static D3DVIEWPORT9 g_viewportCache = { 0 };
static bool g_viewportValid = false;

struct ShadowBufferEntry {
    IDirect3DVertexBuffer9* vb;
    void* data;
    UINT size;
    bool valid;
};
static constexpr int VB_CACHE_SIZE = 128;
static constexpr int VB_CACHE_MASK = VB_CACHE_SIZE - 1;
static ShadowBufferEntry g_vbCache[VB_CACHE_SIZE] = {};

static inline unsigned int HashVB(IDirect3DVertexBuffer9* vb) {
    uintptr_t val = (uintptr_t)vb;
    return (uint32_t)((val ^ (val >> 12)) & VB_CACHE_MASK);
}

struct ConstantRegister {
    float val[4];
    bool valid;
};
static ConstantRegister g_vsConstantCache[256] = { { {0.0f}, false } };

// Latency reduction structures (Max Frame Latency = 1)
#define LATENCY_QUEUE_SIZE 2
static IDirect3DQuery9* g_latencyQueries[LATENCY_QUEUE_SIZE] = { nullptr };
static int g_latencyQueryIndex = 0;
static bool g_latencyInitialized = false;

static void InvalidateLatencyQueries(bool release) {
    for (int i = 0; i < LATENCY_QUEUE_SIZE; i++) {
        if (g_latencyQueries[i]) {
            if (release) {
                __try {
                    g_latencyQueries[i]->Release();
                } __except(EXCEPTION_EXECUTE_HANDLER) {}
            }
            g_latencyQueries[i] = nullptr;
        }
    }
    g_latencyInitialized = false;
    g_latencyQueryIndex = 0;
}

// Statistics
// Plain counters, deliberately, and every one of these sits on a path whose
// entire job is to compare two dwords and return.
//
// They were std::atomic<long>, incremented with fetch_add on the skip branch -
// the fast branch, the one the whole dedup exists to reach. On 32-bit x86 that
// is a lock xadd: tens of cycles and a bus barrier, to count an event whose
// entire cost it then dwarfs. The client issues these calls thousands of times
// a frame, and this file measured about 1% of executing main-thread time in a
// corrected profile, which is what a lock prefix on a fast path buys.
//
// This project has made the same mistake twice before, in the memset hook and
// on the free wrapper, and wrote the reason down both times. An aligned 32-bit
// increment can only ever lose counts, never tear, so the numbers are a lower
// bound and are reported as one.
static long g_textureSkips = 0;
static long g_renderStateSkips = 0;
static long g_stageStateSkips = 0;
static long g_samplerSkips = 0;
static long g_transformSkips = 0;
static long g_viewportSkips = 0;
static long g_vsConstantSkips = 0;

// Clear the cache (called on Init and after device Reset)
static void CleanVBCache() {
    for (int i = 0; i < VB_CACHE_SIZE; i++) {
        if (g_vbCache[i].valid && g_vbCache[i].data) {
            VertexBufferPrealloc::FreeBuffer(g_vbCache[i].data);
            g_vbCache[i].data = nullptr;
            g_vbCache[i].valid = false;
        }
    }
}

static void InvalidateCache() {
    for (int i = 0; i < 16; i++) g_textureCache[i] = nullptr;
    for (int i = 0; i < 512; i++) g_renderStateValid[i] = false;
    for (int i = 0; i < 8; i++) {
        for (int j = 0; j < 64; j++) g_textureStageStateValid[i][j] = false;
    }
    for (int i = 0; i < 16; i++) {
        for (int j = 0; j < 32; j++) g_samplerStateValid[i][j] = false;
    }
    for (int i = 0; i < 512; i++) g_transformCache[i].valid = false;
    g_viewportValid = false;
    for (int i = 0; i < 256; i++) g_vsConstantCache[i].valid = false;
    CleanVBCache();
}



static HRESULT WINAPI Hooked_SetRenderState(IDirect3DDevice9* device, D3DRENDERSTATETYPE state, DWORD value) {
    bool isCriticalState = (state == D3DRS_ALPHABLENDENABLE || state == D3DRS_SRCBLEND || 
                           state == D3DRS_DESTBLEND || state == D3DRS_ALPHATESTENABLE || 
                           state == D3DRS_ALPHAREF || state == D3DRS_ALPHAFUNC ||
                           state == D3DRS_ZWRITEENABLE || state == D3DRS_ZENABLE);

    if ((DWORD)state < 512 && !isCriticalState) {
        if (g_renderStateValid[state] && g_renderStateCache[state] == value) {
            g_renderStateSkips += 1;
            return D3D_OK;
        }
        g_renderStateCache[state] = value;
        g_renderStateValid[state] = true;
    }
    if (D3D9RenderThread::IsActive() && GetCurrentThreadId() == g_mainThreadId) {
        D3D9RenderThread::QueueSetRenderState(device, state, value);
        return D3D_OK;
    }
    return orig_SetRenderState(device, state, value);
}

static HRESULT WINAPI Hooked_SetTransform(IDirect3DDevice9* device, D3DTRANSFORMSTATETYPE state, const D3DMATRIX* matrix) {
    bool isWorldTransform = (state == D3DTS_WORLD || (state >= 256 && state <= 511));

    if ((DWORD)state < 512 && matrix && !isWorldTransform) {
        if (g_transformCache[state].valid && memcmp(&g_transformCache[state].matrix, matrix, sizeof(D3DMATRIX)) == 0) {
            g_transformSkips += 1;
            return D3D_OK;
        }
        memcpy(&g_transformCache[state].matrix, matrix, sizeof(D3DMATRIX));
        g_transformCache[state].valid = true;
    }
    if (D3D9RenderThread::IsActive() && GetCurrentThreadId() == g_mainThreadId) {
        D3D9RenderThread::QueueSetTransform(device, state, matrix);
        return D3D_OK;
    }
    return orig_SetTransform(device, state, matrix);
}

static HRESULT WINAPI Hooked_SetViewport(IDirect3DDevice9* device, const D3DVIEWPORT9* viewport) {
    if (viewport) {
        if (g_viewportValid && memcmp(&g_viewportCache, viewport, sizeof(D3DVIEWPORT9)) == 0) {
            g_viewportSkips += 1;
            return D3D_OK;
        }
        memcpy(&g_viewportCache, viewport, sizeof(D3DVIEWPORT9));
        g_viewportValid = true;
    }
    if (D3D9RenderThread::IsActive() && GetCurrentThreadId() == g_mainThreadId) {
        D3D9RenderThread::QueueSetViewport(device, viewport);
        return D3D_OK;
    }
    return orig_SetViewport(device, viewport);
}

static HRESULT WINAPI Hooked_VB_Lock(IDirect3DVertexBuffer9* vb, UINT OffsetToLock, UINT SizeToLock, void** ppbData, DWORD Flags) {
    D3D9RenderThread::PipelineFlush();
    if (DXVKBridge::IsActive()) {
        return orig_VB_Lock(vb, OffsetToLock, SizeToLock, ppbData, Flags);
    }
    #if !TEST_DISABLE_D3D9_VB_CACHE
    if (vb && ppbData && (Flags & D3DLOCK_DISCARD)) {
        unsigned int slot = HashVB(vb);
        ShadowBufferEntry* e = &g_vbCache[slot];
        
        if (e->valid && e->vb != vb) {
            // Collision! Bypass cache to prevent heap allocation/free churn within the frame
            return orig_VB_Lock(vb, OffsetToLock, SizeToLock, ppbData, Flags);
        }
        
        if (e->valid && e->vb == vb) {
            // Recycled pointer check: make sure the cached size matches the active vertex buffer
            D3DVERTEXBUFFER_DESC desc;
            if (FAILED(vb->GetDesc(&desc)) || desc.Size != e->size) {
                if (e->data) VertexBufferPrealloc::FreeBuffer(e->data);
                e->valid = false;
                e->data = nullptr;
            }
        }
        
        if (!e->valid) {
            D3DVERTEXBUFFER_DESC desc;
            if (SUCCEEDED(vb->GetDesc(&desc))) {
                e->vb = vb;
                e->size = desc.Size;
                e->data = VertexBufferPrealloc::AllocateBuffer(desc.Size);
                e->valid = true;
            }
        }
        
        if (e->valid && e->data) {
            *ppbData = (void*)((uintptr_t)e->data + OffsetToLock);
            return D3D_OK;
        }
    }
    #endif
    return orig_VB_Lock(vb, OffsetToLock, SizeToLock, ppbData, Flags);
}


static HRESULT WINAPI Hooked_VB_Unlock(IDirect3DVertexBuffer9* vb) {
    D3D9RenderThread::PipelineFlush();
    if (DXVKBridge::IsActive()) {
        return orig_VB_Unlock(vb);
    }
    #if !TEST_DISABLE_D3D9_VB_CACHE
    if (vb) {
        unsigned int slot = HashVB(vb);
        ShadowBufferEntry* e = &g_vbCache[slot];
        if (e->valid && e->vb == vb && e->data) {
            void* realData = nullptr;
            HRESULT hr = orig_VB_Lock(vb, 0, e->size, &realData, D3DLOCK_DISCARD);
            if (SUCCEEDED(hr) && realData) {
                memcpy(realData, e->data, e->size);
                orig_VB_Unlock(vb);
            }
            return D3D_OK;
        }
    }
    #endif
    return orig_VB_Unlock(vb);
}

static HRESULT WINAPI Hooked_CreateVertexBuffer(IDirect3DDevice9* device, UINT Length, DWORD Usage, DWORD FVF, D3DPOOL Pool, IDirect3DVertexBuffer9** ppVertexBuffer, HANDLE* pSharedHandle) {
    HRESULT hr = orig_CreateVertexBuffer(device, Length, Usage, FVF, Pool, ppVertexBuffer, pSharedHandle);
    if (hr == D3D_OK && ppVertexBuffer && *ppVertexBuffer && (Usage & D3DUSAGE_DYNAMIC)) {
        if (!g_vbHooksInstalled) {
            uintptr_t* vb_vtable = *(uintptr_t**)(*ppVertexBuffer);
            void* target_Lock = (void*)vb_vtable[11];
            void* target_Unlock = (void*)vb_vtable[12];
            
            if (MH_CreateHook(target_Lock, (void*)Hooked_VB_Lock, (void**)&orig_VB_Lock) == MH_OK) {
                MH_EnableHook(target_Lock);
            }
            if (MH_CreateHook(target_Unlock, (void*)Hooked_VB_Unlock, (void**)&orig_VB_Unlock) == MH_OK) {
                MH_EnableHook(target_Unlock);
            }
            g_vbHooksInstalled = true;
            Log("[D3D9StateCache] Detoured IDirect3DVertexBuffer9::Lock/Unlock for dynamic buffer optimization");
        }
    }
    return hr;
}

static HRESULT WINAPI Hooked_SetVertexShaderConstantF(IDirect3DDevice9* device, UINT StartRegister, const float* pConstantData, UINT Vector4fCount) {
    #if !TEST_DISABLE_D3D9_VS_CONSTANT_CACHE
    if (pConstantData && StartRegister + Vector4fCount <= 256) {
        bool allCached = true;
        for (UINT i = 0; i < Vector4fCount; i++) {
            UINT reg = StartRegister + i;
            if (!g_vsConstantCache[reg].valid || memcmp(g_vsConstantCache[reg].val, pConstantData + i * 4, 16) != 0) {
                allCached = false;
                break;
            }
        }
        
        if (allCached) {
            g_vsConstantSkips += Vector4fCount;
            return D3D_OK;
        }
        
        for (UINT i = 0; i < Vector4fCount; i++) {
            UINT reg = StartRegister + i;
            memcpy(g_vsConstantCache[reg].val, pConstantData + i * 4, 16);
            g_vsConstantCache[reg].valid = true;
        }
    }
    #endif
    if (D3D9RenderThread::IsActive() && GetCurrentThreadId() == g_mainThreadId) {
        D3D9RenderThread::QueueSetVertexShaderConstantF(device, StartRegister, pConstantData, Vector4fCount);
        return D3D_OK;
    }
    return orig_SetVertexShaderConstantF(device, StartRegister, pConstantData, Vector4fCount);
}

static HRESULT WINAPI Hooked_SetVertexShader(IDirect3DDevice9* device, IDirect3DVertexShader9* shader) {
    #if !TEST_DISABLE_D3D9_VS_CONSTANT_CACHE
    // Invalidate VS constant cache since a new shader might change register meanings or load compiler-embedded constants
    for (int i = 0; i < 256; i++) {
        g_vsConstantCache[i].valid = false;
    }
    #endif
    return orig_SetVertexShader(device, shader);
}

static HRESULT WINAPI Hooked_SetSamplerState(IDirect3DDevice9* device, DWORD Sampler, D3DSAMPLERSTATETYPE Type, DWORD Value) {
    if (Sampler < 16 && (DWORD)Type < 32) {
        if (g_samplerStateValid[Sampler][Type] && g_samplerStateCache[Sampler][Type] == Value) {
            g_samplerSkips += 1;
            return D3D_OK;
        }
        g_samplerStateCache[Sampler][Type] = Value;
        g_samplerStateValid[Sampler][Type] = true;
    }

    #if !TEST_DISABLE_MIP_BIAS_GOVERNOR
    if (Type == 10 /* D3DSAMP_MIPMAPLODBIAS */) {
        float bias = MipBiasGovernor::GetCurrentBias();
        if (bias > 0.0f) {
            float floatVal = *(float*)&Value;
            floatVal += bias;
            Value = *(DWORD*)&floatVal;
        }
    }
    #endif

    if (D3D9RenderThread::IsActive() && GetCurrentThreadId() == g_mainThreadId) {
        D3D9RenderThread::QueueSetSamplerState(device, Sampler, Type, Value);
        return D3D_OK;
    }
    return orig_SetSamplerState(device, Sampler, Type, Value);
}

static HRESULT WINAPI Hooked_SetTextureStageState(IDirect3DDevice9* device, DWORD Stage, D3DTEXTURESTAGESTATETYPE Type, DWORD Value) {
    if (Stage < 8 && (DWORD)Type < 64) {
        if (g_textureStageStateValid[Stage][Type] && g_textureStageStateCache[Stage][Type] == Value) {
            g_stageStateSkips += 1;
            return D3D_OK;
        }
        g_textureStageStateCache[Stage][Type] = Value;
        g_textureStageStateValid[Stage][Type] = true;
    }
    if (D3D9RenderThread::IsActive() && GetCurrentThreadId() == g_mainThreadId) {
        D3D9RenderThread::QueueSetTextureStageState(device, Stage, Type, Value);
        return D3D_OK;
    }
    return orig_SetTextureStageState(device, Stage, Type, Value);
}

void InvalidateAllCaches(bool safeToRelease);

static HRESULT WINAPI Hooked_Reset(IDirect3DDevice9* device, D3DPRESENT_PARAMETERS* params) {
    Log("[D3D9StateCache] Device Reset requested. Flushing render pipeline...");
    if (D3D9RenderThread::IsActive()) {
        D3D9RenderThread::PipelineFlush();
    }

    InvalidateAllCaches(true);
    FontGlyphCache::ClearCache();
    TextureUnloadDelay::Discard();
    RenderStateDedup_ClearCache();
    InterlockedIncrement(&g_deviceResetCounter);

    Log("[D3D9StateCache] Executing Reset synchronously on main thread...");
    HRESULT hr = orig_Reset(device, params);
    Log("[D3D9StateCache] Device Reset result: 0x%08X", hr);
    if (SUCCEEDED(hr)) {
        InvalidateAllCaches(true);
        FontGlyphCache::ClearCache();
        RenderStateDedup_ClearCache();
        InterlockedIncrement(&g_deviceResetCounter);
    }
    return hr;
}

// Defined with the rest of the draw census, further down.
void NoteFrameForDrawCensus();

static HRESULT WINAPI Hooked_Present(IDirect3DDevice9* device, const RECT* src, const RECT* dest, HWND window, const RGNDATA* dirty) {
    InvalidateCache();
    NoteFrameForDrawCensus();

    if (D3D9RenderThread::IsActive() && GetCurrentThreadId() == g_mainThreadId) {
        D3D9RenderThread::QueuePresent(device, src, dest, window, dirty);
        return D3D_OK;
    }

#if !TEST_DISABLE_LOW_LATENCY_SYNC
    if (device) {
        if (!g_latencyInitialized) {
            bool ok = true;
            for (int i = 0; i < LATENCY_QUEUE_SIZE; i++) {
                HRESULT hr = device->CreateQuery(D3DQUERYTYPE_EVENT, &g_latencyQueries[i]);
                if (FAILED(hr)) {
                    ok = false;
                    g_latencyQueries[i] = nullptr;
                }
            }
            if (ok) {
                g_latencyInitialized = true;
                Log("[D3D9StateCache] Low-latency GPU sync active (MaxFrameLatency = 1)");
            } else {
                InvalidateLatencyQueries(true);
            }
        }

        if (g_latencyInitialized) {
            IDirect3DQuery9* q = g_latencyQueries[g_latencyQueryIndex];
            if (q) {
                ULONGLONG start = GetTickCount64();
                while (q->GetData(nullptr, 0, D3DGETDATA_FLUSH) == S_FALSE) {
                    if (GetTickCount64() - start > 16) {
                        break;
                    }
                    SwitchToThread();
                }
            }

            IDirect3DQuery9* current_q = g_latencyQueries[g_latencyQueryIndex];
            if (current_q) {
                current_q->Issue(D3DISSUE_END);
            }

            g_latencyQueryIndex = (g_latencyQueryIndex + 1) % LATENCY_QUEUE_SIZE;
        }
    }
#endif

    return orig_Present(device, src, dest, window, dirty);
}

bool Init() {
#if TEST_DISABLE_D3D_STATE_CACHE
    Log("[D3D9StateCache] DISABLED via TEST_DISABLE_D3D_STATE_CACHE.");
    return false;
#endif


    InvalidateCache();
    // The device hooks are never going to arrive, and saying "waiting for" them
    // for the length of a session is worse than saying nothing.
    //
    // They are installed by OnCreateDevice below, which is reached only from
    // render_state_dedup's CreateDevice detour, and that file is compiled out by
    // TEST_DISABLE_RENDER_STATE_DEDUP - set to 1 after it was found triple-
    // hooking the same setters as this file and d3d9_state_manager. So the
    // linker discards OnCreateDevice and everything only it calls.
    //
    // What is missing with it: this file's own redundant render state filter,
    // which d3d9_state_manager does anyway and is the reason the dedup was
    // disabled in the first place; the IDirect3DVertexBuffer9::Lock detour; and
    // the low-latency GPU sync in Hooked_Present. The last one is a spin on an
    // event query with a 16 ms cap on the main thread, so reviving it needs a
    // measurement rather than a wire-up.
    //
    // The draw census and the merger used to be in the same position and are
    // not any more: InstallDrawHooks is called from the state manager's
    // PatchDeviceVTable, which runs.
    Log("[D3D9StateCache] initialised, but its device hooks cannot install in "
        "this build: they come from OnCreateDevice, which only the compiled-out "
        "render_state_dedup calls. The render state filtering this switch names "
        "is done by D3D9StateManager instead.");
    return true;
}

// ---- draw-call census ------------------------------------------------------
//
// Direct3D 9 charges the CPU for every draw, and the usual explanation for a
// city or raid dropping frames is that the client issues far too many small
// ones. That may well be true here - but nothing in this project has ever
// counted them, so any work on batching would start from a guess.
//
// This counts DrawPrimitive and DrawIndexedPrimitive per frame and reports the
// distribution. Three hundred draws a frame means batching has nothing to find;
// three thousand means it is the whole story.
//
// Off by default. It is a wrapper on the hottest call in the renderer, and the
// point is to answer the question in one session and switch it back off - not to
// carry a trampoline per draw forever. The counter is a plain increment because
// draws come from one thread and an interlocked one here would cost more than it
// measures.
typedef HRESULT (WINAPI *DrawPrimitive_fn)(IDirect3DDevice9*, D3DPRIMITIVETYPE, UINT, UINT);
typedef HRESULT (WINAPI *DrawIndexedPrimitive_fn)(IDirect3DDevice9*, D3DPRIMITIVETYPE,
                                                  INT, UINT, UINT, UINT, UINT);

static DrawPrimitive_fn        orig_DrawPrimitive        = nullptr;
static DrawIndexedPrimitive_fn orig_DrawIndexedPrimitive = nullptr;

// Whether the redundancy-filter hooks actually went in. They do not unless
// DXVK support or the render thread is switched on, and the report used to
// print its counters either way.
static bool g_stateHooksInstalled = false;

static uint32_t g_drawsThisFrame = 0;

// ---------------------------------------------------------------------------
// Could these draws have been merged?
//
// A session: 27,357,216 draw calls over 77,047 frames, 355 a frame, carrying
// 1.54 billion primitives. 35.3% of them carried eight primitives or fewer and
// 21.2% carried one or two. At that size the cost is the call, not the
// triangles, and under DXVK a call is command buffer recording and state
// validation.
//
// So the question is how many of them the client could have issued as one, and
// nothing has ever counted it. Two consecutive DrawIndexedPrimitive calls can
// become one when nothing changed between them, they draw the same primitive
// type from the same vertex base, and the second picks up where the first left
// off in the index buffer.
//
// Triangle lists only. A strip or a fan needs degenerate triangles inserted to
// join, which is a different and larger change, so counting them here would
// promise something this measurement is not about.
//
// "Nothing changed" is g_stateEpoch, which the state manager bumps when a
// setter actually reaches D3D9 and not when its dedup skips one - a skipped
// call means the state did not change, which is exactly right here.
//
// This counts and does not act. Merging draw calls is a renderer change; the
// point of this is to find out in one session whether it is worth making.
#include "draw_merge.h"

static uint64_t g_mergeable      = 0;   // draws that could join the one before
static uint64_t g_mergeablePrims = 0;
static uint64_t g_chains         = 0;   // runs of two or more
static uint32_t g_chainLen       = 0;
static uint32_t g_longestChain   = 0;
static uint64_t g_indexedDraws   = 0;

static unsigned long g_prevEpoch  = 0xFFFFFFFFu;
static DWORD    g_prevType       = 0;
static INT      g_prevBaseVertex = 0;
static UINT     g_prevStartIndex = 0;
static UINT     g_prevPrimCount  = 0;
static bool     g_havePrev       = false;

static inline void NoteMergeChance(DWORD type, INT baseVertex,
                                   UINT startIndex, UINT primCount) {
    ++g_indexedDraws;
    const bool joins =
        g_havePrev &&
        g_stateEpoch == g_prevEpoch &&
        type == g_prevType &&
        type == 4 /* D3DPT_TRIANGLELIST */ &&
        baseVertex == g_prevBaseVertex &&
        startIndex == g_prevStartIndex + g_prevPrimCount * 3;
    if (joins) {
        ++g_mergeable;
        g_mergeablePrims += primCount;
        if (g_chainLen == 0) g_chainLen = 2; else ++g_chainLen;
        if (g_chainLen > g_longestChain) g_longestChain = g_chainLen;
    } else {
        if (g_chainLen >= 2) ++g_chains;
        g_chainLen = 0;
    }
    g_prevEpoch      = g_stateEpoch;
    g_prevType       = type;
    g_prevBaseVertex = baseVertex;
    g_prevStartIndex = startIndex;
    g_prevPrimCount  = primCount;
    g_havePrev       = true;
}

// ---------------------------------------------------------------------------
// The merger, and the field result that says not to use it.
//
// 2026-09-05, build 1875f18b, a 2318 second session with all thirty-four
// barriers installed and no state blocks created, so the census below is sound
// rather than the over-counted 10.6% from the session where none of them went
// in:
//
//     293,120,052 indexed draws, 2728 a frame, 26.0% carrying eight primitives
//     or fewer. 2.4% of them could join the draw before them. The merger removed
//     exactly those - 7,174,101 calls, longest chain 17, both ways of counting
//     agreed, no merged call returned an error - and the frame rate went down.
//
// Which is the answer this was built to get. The module's own criterion is
// written a few lines below: a share in the tens of percent says build it, a few
// percent says the client already batches what it can. Two point four is a few
// percent, and holding every triangle list draw for one step to find them costs
// more on 293 million calls than removing seven million of them saves.
//
// The switch is gone from the launcher and the code stays, because the next
// person to wonder about draw call batching should find the number rather than
// the idea. What it does not close is batching by some other rule than index
// contiguity: 26% of these draws carry eight primitives or fewer, and this only
// ever looked at the ones already adjacent in the index buffer.
//
// The census counts pairs that could have been one call. This issues them as
// one. It holds a DrawIndexedPrimitive instead of passing it on, and when the
// next one turns out to be its continuation it folds it in and issues nothing.
// The held draw goes out the moment anything happens that could change what it
// produces.
//
// Two draws become one when they are triangle lists from the same vertex base
// and the second picks up exactly where the first stopped in the index buffer:
// [startIndex, startIndex + 3*primCount) and the range beginning at its end are
// one contiguous run of indices, so a single call over the whole run draws the
// same triangles in the same order. MinVertexIndex and NumVertices only bound
// which vertices the run touches, so the union of both bounds is correct for
// the merged call.
//
// What must flush it, and where each one is caught:
//
//   the fourteen wrapped setters      D3D9_StateBarrier() in d3d9_state_manager
//   thirty-four device methods       the barrier thunks, same file
//   Present and Reset                 their hooks in d3d9_state_manager
//   a different kind of draw          Hooked_DrawPrimitive below
//   a vertex or index buffer lock     the Lock thunks in d3d9_state_manager
//   a texture lock that can write     the texture Lock thunks, same file
//   a surface lock that can write     the surface LockRect thunk
//   an occlusion query opening or     the query Issue thunk
//     closing
//
// The last two are on vtables no device method returns reliably - a surface
// comes from GetSurfaceLevel as readily as from GetBackBuffer - so the state
// manager asks the device for one of each at install time, patches the vtable
// every object of that type shares, and releases them. Both have to be in
// before the merger starts.
//
// The buffer and texture locks are patched the first time SetStreamSource,
// SetIndices or SetTexture hands one over, which is safe by construction: a
// draw needs a vertex and an index buffer, and a draw that samples a texture
// had that texture bound, so all three precede the first draw that could care.
//   a readback of the render target   GetRenderTargetData, GetFrontBufferData
//   a chain getting long              kMaxHeld
//
// What is NOT caught, and is why this is off by default:
//
//   IDirect3DStateBlock9::Apply changes device state without touching the
//   device vtable. The census counts state block creation and says outright
//   that its number is unsound if the client made any; the merger is bound by
//   the same limit and refuses to hold anything once one has been created.
//
//   Nothing else. A texture rewritten through LockRect between two draws used to
//   be the second hole here; the lock sits at vtable slot 19 for all three
//   texture types and is a barrier now, and merging stops outright if a texture
//   type turns up whose lock cannot be patched.
//
// The client is told D3D_OK for a draw that has not been issued yet. WoW does
// not read it, and a failure surfaces on the flush instead, counted below.
// ---------------------------------------------------------------------------
extern "C" unsigned char g_drawMergePending = 0;

static bool g_mergeOn = false;   // switch on AND the draw hook actually in

static IDirect3DDevice9* g_pendDevice = nullptr;
static D3DPRIMITIVETYPE  g_pendType   = D3DPT_TRIANGLELIST;
static INT      g_pendBase  = 0;
static UINT     g_pendMin   = 0;   // lowest vertex index the held run touches
static UINT     g_pendEnd   = 0;   // one past the highest
static UINT     g_pendStart = 0;
static UINT     g_pendPrims = 0;
static UINT     g_pendHeld  = 0;   // draws folded into it, one or more
static unsigned long g_pendEpoch = 0;

// The thread that held the first draw. Draws come from one thread here, but
// D3d9RenderThread replays state setters on a second one, and a setter is a
// barrier - so a held draw could be issued from a thread that never made it,
// against state kept in plain non-atomic variables. Nothing in this build
// actually reaches that path (the queueing hooks are in the dead half of this
// file), which is a reason to check rather than a reason to assume.
static unsigned long g_mergeThread = 0;

// Long chains are where the win is, but an unbounded one delays geometry for
// no extra saving worth the exposure.
static constexpr UINT kMaxHeld = 64;

static uint64_t g_drawsHeld    = 0;   // calls that returned without drawing
static uint64_t g_drawsIssued  = 0;   // calls this actually made
static uint64_t g_callsSaved   = 0;
static uint32_t g_longestMerge = 0;
static uint64_t g_mergedPrims  = 0;
static uint64_t g_flushNotNext = 0;   // the next draw was not the continuation
static uint64_t g_flushCap     = 0;   // the chain hit kMaxHeld
static uint64_t g_flushLock    = 0;   // a buffer was locked
static uint64_t g_flushTexLock = 0;   // a texture was locked
static uint64_t g_mergeFailed  = 0;   // the merged call itself returned an error
static unsigned char g_foreignThread = 0;  // a second thread reached the merger

extern "C" void __cdecl D3D9DrawMerge_FlushPending(void) {
    if (!g_drawMergePending) return;
    g_drawMergePending = 0;

    // Issued from whichever thread got here: the draw is already late and the
    // client asked for it before whatever is about to happen. Merging stops
    // after it, because the state it was held against is not shared safely.
    const unsigned long here = GetCurrentThreadId();
    if (g_mergeThread && here != g_mergeThread) {
        g_foreignThread = 1;
        g_mergeOn = false;
    }

    if (g_pendHeld >= 2) {
        g_callsSaved += g_pendHeld - 1;
        if (g_pendHeld > g_longestMerge) g_longestMerge = g_pendHeld;
        g_mergedPrims += g_pendPrims;
    }
    ++g_drawsIssued;
    HRESULT hr = orig_DrawIndexedPrimitive(g_pendDevice, g_pendType, g_pendBase,
                                           g_pendMin, g_pendEnd - g_pendMin,
                                           g_pendStart, g_pendPrims);
    if (FAILED(hr)) ++g_mergeFailed;
}

extern "C" void __cdecl D3D9DrawMerge_BufferLockBarrier(unsigned long flags) {
    // D3DLOCK_NOOVERWRITE is the client promising not to touch anything already
    // drawn from, which is the whole of what the held draw reads.
    if (flags & 0x00001000UL) return;
    if (g_drawMergePending) { ++g_flushLock; D3D9DrawMerge_FlushPending(); }
    ++g_stateEpoch;
}

// The client created a state block. Applying one changes device state without
// touching the device vtable, so from here on nothing can be held safely. This
// is one-way: there is no way to know that the last state block is gone.
static bool g_disabledByStateBlock = false;
extern "C" void __cdecl D3D9DrawMerge_Disable(void) {
    if (!g_mergeOn) return;
    D3D9DrawMerge_FlushPending();
    g_mergeOn = false;
    g_disabledByStateBlock = true;
}

extern "C" void __cdecl D3D9DrawMerge_TextureLockBarrier(unsigned long flags) {
    // D3DLOCK_READONLY: the client promises to read and not write, so no pixel
    // a held draw samples can change under it.
    if (flags & 0x00000010UL) return;
    if (g_drawMergePending) { ++g_flushTexLock; D3D9DrawMerge_FlushPending(); }
    ++g_stateEpoch;
}

// Returns true when the call was absorbed and the caller must not draw.
static inline bool MergeIndexedDraw(IDirect3DDevice9* device, D3DPRIMITIVETYPE type,
                                    INT baseVertex, UINT minIndex, UINT numVertices,
                                    UINT startIndex, UINT primCount) {
    if (g_drawMergePending) {
        if (g_pendHeld >= kMaxHeld) {
            ++g_flushCap;
            D3D9DrawMerge_FlushPending();
        } else if (device       == g_pendDevice &&
                   type         == D3DPT_TRIANGLELIST &&
                   g_pendType   == D3DPT_TRIANGLELIST &&
                   baseVertex   == g_pendBase &&
                   g_stateEpoch == g_pendEpoch &&
                   startIndex   == g_pendStart + g_pendPrims * 3) {
            g_pendPrims += primCount;
            if (minIndex < g_pendMin)               g_pendMin = minIndex;
            if (minIndex + numVertices > g_pendEnd) g_pendEnd = minIndex + numVertices;
            ++g_pendHeld;
            ++g_drawsHeld;
            return true;
        } else {
            ++g_flushNotNext;
            D3D9DrawMerge_FlushPending();
        }
    }

    if (type != D3DPT_TRIANGLELIST) return false;

    const unsigned long here = GetCurrentThreadId();
    if (g_mergeThread == 0) g_mergeThread = here;
    if (here != g_mergeThread) {
        g_foreignThread = 1;
        g_mergeOn = false;
        return false;
    }

    g_pendDevice = device;
    g_pendType   = type;
    g_pendBase   = baseVertex;
    g_pendMin    = minIndex;
    g_pendEnd    = minIndex + numVertices;
    g_pendStart  = startIndex;
    g_pendPrims  = primCount;
    g_pendEpoch  = g_stateEpoch;
    g_pendHeld   = 1;
    g_drawMergePending = 1;
    ++g_drawsHeld;
    return true;
}

// Buckets of 100 draws, up to 5000, then an overflow bin.
static constexpr int DRAW_BUCKETS = 51;
static uint32_t g_drawHistogram[DRAW_BUCKETS] = {};
static uint32_t g_drawFrames  = 0;
static uint32_t g_drawMax     = 0;
static uint64_t g_drawTotal   = 0;

// Batching a draw call is only possible when several small ones sit next to
// each other. The count of draws per frame does not say whether any do, so it
// cannot size the win - and building a batcher to find out is how this project
// spent three days undoing features nobody had measured.
//
// A UI quad is two triangles. Anything at or below that is a candidate; a run
// of them back to back is what a batcher would merge into one call. The longest
// and the total run length bound the saving exactly: merging a run of n costs
// one call instead of n.
static constexpr UINT SMALL_PRIM_MAX = 2;
static uint64_t g_smallDraws   = 0;   // draws of <= SMALL_PRIM_MAX primitives
static uint64_t g_runDraws     = 0;   // small draws that were part of a run >= 2
static uint64_t g_runs         = 0;   // number of such runs
static uint32_t g_longestRun   = 0;
static uint32_t g_currentRun   = 0;

static inline void NoteDrawShape(UINT primCount) {
    if (primCount <= SMALL_PRIM_MAX) {
        ++g_smallDraws;
        ++g_currentRun;
        if (g_currentRun > g_longestRun) g_longestRun = g_currentRun;
    } else {
        if (g_currentRun >= 2) { ++g_runs; g_runDraws += g_currentRun; }
        g_currentRun = 0;
    }
}

static HRESULT WINAPI Hooked_DrawPrimitive(IDirect3DDevice9* device,
                                           D3DPRIMITIVETYPE type,
                                           UINT startVertex, UINT primCount) {
    if (g_drawMergePending) { ++g_flushNotNext; D3D9DrawMerge_FlushPending(); }
    ++g_drawsThisFrame;
    NoteDrawShape(primCount);
    return orig_DrawPrimitive(device, type, startVertex, primCount);
}

static HRESULT WINAPI Hooked_DrawIndexedPrimitiveCount(IDirect3DDevice9* device,
                                                       D3DPRIMITIVETYPE type,
                                                       INT baseVertex, UINT minIndex,
                                                       UINT numVertices, UINT startIndex,
                                                       UINT primCount) {
    ++g_drawsThisFrame;
    NoteDrawShape(primCount);
    NoteMergeChance((DWORD)type, baseVertex, startIndex, primCount);
    if (g_mergeOn &&
        MergeIndexedDraw(device, type, baseVertex, minIndex, numVertices,
                         startIndex, primCount)) {
        return D3D_OK;
    }
    ++g_drawsIssued;
    return orig_DrawIndexedPrimitive(device, type, baseVertex, minIndex,
                                     numVertices, startIndex, primCount);
}

void LogMergeCensus() {
    if (g_indexedDraws == 0) {
        Log("[DrawMerge] no indexed draws were counted, so nothing here says "
            "whether merging would be worth anything.");
        return;
    }
    const double pct = 100.0 * (double)g_mergeable / (double)g_indexedDraws;
    Log("[DrawMerge] %llu of %llu indexed draws (%.1f%%) could have been issued "
        "as part of the one before them: same triangle list, same vertex base, "
        "the next indices along, and no state change in between.",
        g_mergeable, g_indexedDraws, pct);
    Log("[DrawMerge]   they carried %llu primitives, in %llu run(s) of two or "
        "more, the longest %lu draws long. Collapsing every run leaves %llu "
        "calls where the client made %llu.",
        g_mergeablePrims, g_chains, g_longestChain,
        g_indexedDraws - g_mergeable, g_indexedDraws);
    Log("[DrawMerge]   this counts and changes nothing. Merging draws is a "
        "renderer change and the point of counting first is to find out in one "
        "session whether it is worth making. A share in the tens of percent "
        "says yes; a few percent says the client already batches what it can "
        "and the small draws are small because their state differs.");
    Log("[DrawMerge]   strips and fans are not counted as mergeable at all - "
        "joining them needs degenerate triangles, which is a different and "
        "larger change than this measurement is about.");

    // Whether the share above can be believed.
    //
    // "Nothing changed between them" is only as good as the set of things that
    // say something changed. The state manager wraps fourteen setters; a draw
    // also depends on shader constants, Clear, SetRenderTarget, the scene
    // boundary, lights and clip planes, and those bump the epoch through
    // barriers patched into the device vtable beside the state hooks. A
    // barrier that did not install leaves the share too high.
    int installed = 0, total = 0;
    unsigned long stateBlocks = 0;
    D3D9StateManager_GetBarrierState(&installed, &total, &stateBlocks);
    if (total == 0) {
        Log("[DrawMerge]   THE SHARE ABOVE IS NOT MEASURED: no merge barriers "
            "were installed at all, so nothing but the fourteen wrapped setters "
            "could move the epoch.");
    } else if (installed < total) {
        Log("[DrawMerge]   THE SHARE ABOVE IS TOO HIGH: %d of %d merge barriers "
            "installed. The %d that did not are device methods that change what "
            "a draw produces without moving the epoch, so pairs that are not "
            "joinable were counted as joinable.",
            installed, total, total - installed);
    } else {
        Log("[DrawMerge]   all %d merge barriers installed: a pair counted as "
            "joinable had no shader constant, no Clear, no render target change "
            "and no scene boundary between it and the draw before it.", total);
    }
    if (stateBlocks) {
        Log("[DrawMerge]   THE SHARE ABOVE IS UNSOUND: the client created %lu "
            "state block(s). IDirect3DStateBlock9::Apply changes device state "
            "without touching the device vtable, so it cannot be seen from here "
            "and a merge across one would be wrong. Build nothing on this "
            "number until that is handled.", stateBlocks);
    } else {
        Log("[DrawMerge]   the client created no state blocks, so the one state "
            "change this cannot see - a state block applying itself, which "
            "never touches the device vtable - did not happen.");
    }
}

void DrawMerge_LogStats(void) {
    if (!Config::g_settings.OptDrawMerge) {
        Log("[DrawMerger] not measured: switched off.");
        return;
    }
    if (!orig_DrawIndexedPrimitive) {
        Log("[DrawMerger] not measured: the draw hook is not installed, so "
            "nothing could be merged. It needs the same device vtable the draw "
            "census uses.");
        return;
    }
    if (g_foreignThread) {
        Log("[DrawMerger] STOPPED: a second thread reached the merger. Its "
            "state is plain non-atomic variables held between two draws, so a "
            "held draw could be issued against state another thread had "
            "already changed. Merging was switched off at that point.");
    }
    if (g_disabledByStateBlock) {
        Log("[DrawMerger] STOPPED: the client created a state block. Applying "
            "one changes device state without touching the device vtable, so a "
            "held draw could be issued under state it never saw. Merging was "
            "switched off at that point and stayed off.");
    }
    // Two ways of counting the same thing. The difference between what the
    // client asked for and what D3D9 received is the saving; adding up the
    // chain lengths is the saving too. They must agree, and if they do not the
    // number is wrong rather than interesting.
    //
    // A chain can be open while this runs, and its draws are counted as asked
    // for and not yet issued, so it is exactly `held` of the difference.
    const uint64_t held   = g_drawMergePending ? (uint64_t)g_pendHeld : 0;
    const uint64_t before = g_indexedDraws;
    const uint64_t after  = g_drawsIssued;
    if (g_callsSaved == 0) {
        Log("[DrawMerger] measured and zero: %llu indexed draws went through "
            "and none was merged into another.", before);
        return;
    }
    const uint64_t saved = (before > after) ? (before - after) : 0;
    Log("[DrawMerger] %llu calls saved. The client made %llu indexed draw calls "
        "and %llu reached D3D9, so %.1f%% of them never happened.",
        g_callsSaved, before, after,
        before ? 100.0 * (double)g_callsSaved / (double)before : 0.0);
    if (saved != g_callsSaved + held) {
        Log("[DrawMerger]   DISAGREEMENT: the chain lengths add up to %llu saved "
            "calls, the call counts say %llu with %llu still held. One of the "
            "two is wrong; do not use either.", g_callsSaved, saved, held);
    }
    Log("[DrawMerger]   longest chain %lu draws, %llu primitives went out in a "
        "merged call.", (unsigned long)g_longestMerge, g_mergedPrims);
    Log("[DrawMerger]   held draws let go because: %llu were not the "
        "continuation, %llu hit the %u-draw cap, %llu had a buffer locked under "
        "them, %llu had a texture rewritten under them. Everything else was a "
        "state change.",
        g_flushNotNext, g_flushCap, (unsigned)kMaxHeld, g_flushLock,
        g_flushTexLock);
    if (g_mergeFailed) {
        Log("[DrawMerger]   WARNING: %llu merged call(s) returned an error. The "
            "client was already told D3D_OK for those draws.", g_mergeFailed);
    } else {
        Log("[DrawMerger]   no merged call returned an error.");
    }
    Log("[DrawMerger]   this measured negative once already: 2.4%% of 293 "
        "million draws on a sound census, removed correctly, and the frame rate "
        "went down. Holding every triangle list draw for one step costs more "
        "than the calls it finds. Compare against that before concluding "
        "anything from the numbers above.");
}

// Called from the Present hook, which already runs once per presented frame.
void NoteFrameForDrawCensus() {
    if (!orig_DrawIndexedPrimitive) return;

    // A frame boundary ends whatever run was open.
    if (g_currentRun >= 2) { ++g_runs; g_runDraws += g_currentRun; }
    g_currentRun = 0;
    if (g_chainLen >= 2) ++g_chains;
    g_chainLen = 0;
    g_havePrev = false;

    uint32_t n = g_drawsThisFrame;
    g_drawsThisFrame = 0;

    g_drawFrames++;
    g_drawTotal += n;
    if (n > g_drawMax) g_drawMax = n;

    int b = (int)(n / 100);
    if (b >= DRAW_BUCKETS) b = DRAW_BUCKETS - 1;
    g_drawHistogram[b]++;
}

void ReportDrawCensus() {
    if (!orig_DrawIndexedPrimitive) {
        Log("[DrawCensus] not installed - draws per frame were not counted");
        return;
    }
    if (g_drawFrames == 0) {
        Log("[DrawCensus] installed but no frame was presented");
        return;
    }

    // What a UI batcher could actually save, before one is written.
    if (g_drawTotal > 0) {
        double pctSmall = 100.0 * (double)g_smallDraws / (double)g_drawTotal;
        Log("[DrawCensus] %llu of %llu draws were <= %u primitives (%.1f%%)",
            (unsigned long long)g_smallDraws, (unsigned long long)g_drawTotal,
            SMALL_PRIM_MAX, pctSmall);

        if (g_runs > 0) {
            double avgRun = (double)g_runDraws / (double)g_runs;
            // Merging a run of n turns n calls into one, so the saving is
            // runDraws - runs. Stated as calls per frame, which is the unit the
            // rest of this block uses.
            double savedPerFrame = (double)(g_runDraws - g_runs) / (double)g_drawFrames;
            Log("[DrawCensus] %llu runs of consecutive small draws, average %.1f, "
                "longest %u - batching them would remove about %.0f calls/frame "
                "of the %.0f measured",
                (unsigned long long)g_runs, avgRun, g_longestRun, savedPerFrame,
                (double)g_drawTotal / (double)g_drawFrames);
        } else {
            Log("[DrawCensus] no runs of consecutive small draws - a UI batcher "
                "would have nothing to merge here");
        }
    }

    // Median from the histogram rather than a stored series.
    uint32_t half = g_drawFrames / 2, seen = 0;
    int medianBucket = 0;
    for (int i = 0; i < DRAW_BUCKETS; i++) {
        seen += g_drawHistogram[i];
        if (seen >= half) { medianBucket = i; break; }
    }

    Log("[DrawCensus] %u frames: %.0f draws avg, ~%d median, %u peak",
        g_drawFrames, (double)g_drawTotal / (double)g_drawFrames,
        medianBucket * 100 + 50, g_drawMax);

    Log("[DrawCensus]   distribution (draws per frame):");
    for (int i = 0; i < DRAW_BUCKETS; i++) {
        if (!g_drawHistogram[i]) continue;
        if (i == DRAW_BUCKETS - 1)
            Log("[DrawCensus]     %4d+      %6u frames (%5.1f%%)", i * 100,
                g_drawHistogram[i], 100.0 * g_drawHistogram[i] / g_drawFrames);
        else
            Log("[DrawCensus]     %4d-%-4d  %6u frames (%5.1f%%)", i * 100, i * 100 + 99,
                g_drawHistogram[i], 100.0 * g_drawHistogram[i] / g_drawFrames);
    }
}

// The draw census and the merger, installed from wherever a device vtable is
// available.
//
// This used to live inside OnCreateDevice, which is reached only from
// render_state_dedup's CreateDevice hook - and that file is compiled out by
// TEST_DISABLE_RENDER_STATE_DEDUP, which has been 1 since the dedup was found
// to triple-hook the same setters. So the linker dropped OnCreateDevice, and
// with it the census, and the DrawCensus switch has never installed anything.
// The state manager patches the same vtable and does run, so it calls this.
//
// It takes the original DrawPrimitive and DrawIndexedPrimitive addresses rather
// than the vtable, because by the time the state manager can call it those two
// slots hold its own hooks. MinHook detours the function bodies, so the state
// manager's hook calls the raw address, lands here, and this calls the
// trampoline - three layers, each running once.
static bool g_drawHooksInstalled = false;

void InstallDrawHooks(void* origDrawPrimitive, void* origDrawIndexed) {
    if (g_drawHooksInstalled) return;

    // The switch first, because the state manager stopped patching the two draw
    // slots when nothing is measuring and therefore has no original to hand
    // over. Asking about the pointers first turned a switch the player left
    // alone into "the draw entry points were not resolved", which is what a
    // client with a moved address would say and reads as a fault on a machine
    // that has nothing wrong with it.
    //
    // The merger needs the same trampoline and the same barriers, so it brings
    // the census with it rather than duplicating either.
    if (!Config::g_settings.OptDrawCensus && !Config::g_settings.OptDrawMerge) {
        orig_DrawIndexedPrimitive = nullptr;   // marks the census as not counting
        return;
    }

    if (!origDrawPrimitive || !origDrawIndexed) {
        Log("[DrawCensus] not installed: the draw entry points were not "
            "resolved, so nothing counts draws this session.");
        return;
    }
    g_drawHooksInstalled = true;

    orig_DrawPrimitive        = (DrawPrimitive_fn)origDrawPrimitive;
    orig_DrawIndexedPrimitive = (DrawIndexedPrimitive_fn)origDrawIndexed;

    if (MH_CreateHook(origDrawPrimitive, (void*)Hooked_DrawPrimitive,
                      (void**)&orig_DrawPrimitive) == MH_OK &&
        MH_CreateHook(origDrawIndexed, (void*)Hooked_DrawIndexedPrimitiveCount,
                      (void**)&orig_DrawIndexedPrimitive) == MH_OK) {
        MH_EnableHook(origDrawPrimitive);
        MH_EnableHook(origDrawIndexed);
        Log("[DrawCensus] Counting draw calls per frame");
        if (Config::g_settings.OptDrawMerge) {
            // Holding a draw is only safe while every way the client can change
            // what that draw produces is caught. Thirty of those are barrier
            // thunks in the device vtable, and running without them is not a
            // degraded measurement, it is wrong output: a draw held across a
            // vertex shader constant write comes back drawn under the previous
            // object's transform, which is what the screen looked like.
            int installed = 0, total = 0;
            unsigned long stateBlocks = 0;
            D3D9StateManager_GetBarrierState(&installed, &total, &stateBlocks);
            const bool derived = D3D9StateManager_DerivedBarriersOk();
            if (total > 0 && installed == total && derived) {
                g_mergeOn = true;
                Log("[DrawMerger] ACTIVE: consecutive triangle-list draws that "
                    "continue each other in the index buffer with no state "
                    "change between them go out as one call. All %d barriers "
                    "are in.", total);
            } else {
                Log("[DrawMerger] REFUSED TO START: %d of %d device barriers "
                    "installed, surface and query barriers %s. Without all of "
                    "them a held draw can be issued under state it never saw, "
                    "which draws geometry in the wrong place. Nothing is being "
                    "merged this session.",
                    installed, total, derived ? "in" : "MISSING");
            }
        }
    } else {
        orig_DrawIndexedPrimitive = nullptr;
        Log("[DrawCensus] ERROR: could not hook the draw calls");
    }
}

void OnCreateDevice(IDirect3DDevice9* device) {
    if (!device) return;

    // Invalidate the cache whenever a new device is created to prevent stale cache entries from being reused
    InvalidateCache();

    uintptr_t* vtable = *(uintptr_t**)device;
    if (!vtable) return;

    // Always resolve the original pointers to avoid null dereferences on D3D9RenderThread
    orig_Reset = (Reset_fn)vtable[16];
    orig_Present = (Present_fn)vtable[17];
    orig_SetRenderState = (SetRenderState_fn)vtable[57];
    orig_SetTransform = (SetTransform_fn)vtable[44];
    orig_SetViewport = (SetViewport_fn)vtable[47];
    orig_CreateVertexBuffer = (CreateVertexBuffer_fn)vtable[26];
    orig_SetVertexShaderConstantF = (SetVertexShaderConstantF_fn)vtable[94];
    orig_SetSamplerState = (SetSamplerState_fn)vtable[69];
    orig_SetTextureStageState = (SetTextureStageState_fn)vtable[67];
    orig_SetVertexShader = (SetVertexShader_fn)vtable[92];

    InstallDrawHooks((void*)vtable[81], (void*)vtable[82]);

    // Only install state cache hooks if it is actually enabled by the user config
    if (!Config::g_settings.OptVulkanDXVK && !Config::g_settings.OptD3d9RenderThread) {
        return;
    }

    if (g_stateHooksInstalled) return;

    void* target_Reset = (void*)orig_Reset;
    void* target_Present = (void*)orig_Present;
    void* target_SetRenderState = (void*)orig_SetRenderState;
    void* target_SetTransform = (void*)orig_SetTransform;
    void* target_SetViewport = (void*)orig_SetViewport;
    void* target_CreateVertexBuffer = (void*)orig_CreateVertexBuffer;
    void* target_SetVertexShaderConstantF = (void*)orig_SetVertexShaderConstantF;
    void* target_SetSamplerState = (void*)orig_SetSamplerState;
    void* target_SetTextureStageState = (void*)orig_SetTextureStageState;
    void* target_SetVertexShader = (void*)orig_SetVertexShader;

    if (MH_CreateHook(target_Reset, (void*)Hooked_Reset, (void**)&orig_Reset) != MH_OK ||
        MH_CreateHook(target_Present, (void*)Hooked_Present, (void**)&orig_Present) != MH_OK ||
        MH_CreateHook(target_SetRenderState, (void*)Hooked_SetRenderState, (void**)&orig_SetRenderState) != MH_OK ||
        MH_CreateHook(target_SetTransform, (void*)Hooked_SetTransform, (void**)&orig_SetTransform) != MH_OK ||
        MH_CreateHook(target_SetViewport, (void*)Hooked_SetViewport, (void**)&orig_SetViewport) != MH_OK ||
        MH_CreateHook(target_CreateVertexBuffer, (void*)Hooked_CreateVertexBuffer, (void**)&orig_CreateVertexBuffer) != MH_OK ||
        MH_CreateHook(target_SetVertexShaderConstantF, (void*)Hooked_SetVertexShaderConstantF, (void**)&orig_SetVertexShaderConstantF) != MH_OK ||
        MH_CreateHook(target_SetSamplerState, (void*)Hooked_SetSamplerState, (void**)&orig_SetSamplerState) != MH_OK ||
        MH_CreateHook(target_SetTextureStageState, (void*)Hooked_SetTextureStageState, (void**)&orig_SetTextureStageState) != MH_OK ||
        MH_CreateHook(target_SetVertexShader, (void*)Hooked_SetVertexShader, (void**)&orig_SetVertexShader) != MH_OK) 
    {
        Log("[D3D9StateCache] Failed to create MinHook detours");
        return;
    }

    MH_EnableHook(target_Reset);
    MH_EnableHook(target_Present);
    MH_EnableHook(target_SetRenderState);
    MH_EnableHook(target_SetTransform);
    MH_EnableHook(target_SetViewport);
    MH_EnableHook(target_CreateVertexBuffer);
    MH_EnableHook(target_SetVertexShaderConstantF);
    MH_EnableHook(target_SetSamplerState);
    MH_EnableHook(target_SetTextureStageState);
    MH_EnableHook(target_SetVertexShader);

    g_stateHooksInstalled = true;
    Log("[D3D9StateCache] Active - Redundant render state filtering successfully hooked on main thread");
}

// Printed from the periodic report. Shutdown does not run - the DLL exits via
// TerminateProcess - so anything reported only from there is never seen.
void LogStats() {
    // Three states, not two. These seven counters read as a feature that ran and
    // found nothing, and in the session that prompted this they meant the hooks
    // were never installed at all: they go in only under DXVK or the D3D9 render
    // thread, and both were off. Seven zeros printed as measurements.
    if (!g_stateHooksInstalled) {
        Log("[D3D9StateCache] not installed - its state hooks go in only when "
            "Vulkan/DXVK support or the D3D9 render thread is on, and neither "
            "is. The counters below would all be zero because nothing ran, "
            "which is not the same as nothing to skip. The D3D9 State Manager "
            "is the one doing this work; its numbers are elsewhere in this "
            "report.");
        return;
    }
    Log("[D3D9StateCache] redundancy skips - textures %ld, render states %ld, "
        "stage states %ld, samplers %ld, transforms %ld, viewports %ld, "
        "vs constants %ld",
        g_textureSkips, g_renderStateSkips, g_stageStateSkips,
        g_samplerSkips, g_transformSkips, g_viewportSkips,
        g_vsConstantSkips);
}

void Shutdown() {
    InvalidateLatencyQueries(false);
    CleanVBCache();
    LogStats();
}

void InvalidateAllCaches(bool safeToRelease) {
    Log("[D3D9StateCache] Clearing all state cache registries (device change/reset, safeToRelease=%d)...", safeToRelease);
    InvalidateCache();
    InvalidateLatencyQueries(safeToRelease);
    CleanVBCache();
}

} // namespace D3D9StateCache
