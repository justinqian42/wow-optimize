// grass_parallel.cpp - Parallel vertex & index generation for ground clutter / detail doodads
#include "grass_parallel.h"
#include <windows.h>
#include <emmintrin.h>
#include <cmath>
#include <cstdint>
#include "config.h"
#include "MinHook.h"
#include "version.h"

extern "C" void Log(const char* fmt, ...);

namespace {

constexpr uintptr_t kInitRenderStateAddr = 0x007B2D30; // CDetailDoodad::InitializeRenderState
constexpr uintptr_t kAcquireBuffersAddr  = 0x007B2CA0; // CDetailDoodadInstBatch::AcquireBuffers
constexpr uintptr_t kTextureGetGxTexAddr = 0x004B6CB0; // TextureGetGxTex
constexpr uintptr_t kWaitForLoadAddr     = 0x00823ED0; // CM2Model::WaitForLoad
constexpr uintptr_t kMatrixRotAddr       = 0x007B1000; // sub_7B1000
constexpr uintptr_t kGxDevicePtrAddr     = 0x00C5DF88; // dword_C5DF88
constexpr uintptr_t kDoodadListAddr      = 0x00D1C4FC; // CDetailDoodad::s_doodadList
constexpr uintptr_t kShadersReadyAddr    = 0x00D1C4F0; // CDetailDoodad::s_vertexShadersReady
constexpr uintptr_t kSortTableListAddr   = 0x00CDAF54; // sortTable.m_detailDoodadInstList
constexpr uintptr_t kTlsIndexAddr        = 0x00D439BC; // TlsIndex for ObjectMgrClient

typedef int   (__cdecl* InitRenderState_fn)();
typedef int   (__cdecl* AcquireBuffers_fn)(int vertexCount, uint32_t* pVertexBuf, int indexCount, uint32_t* pIndexBuf);
typedef int   (__cdecl* TextureGetGxTex_fn)(uint32_t texture, int a2, int a3);
typedef void  (__thiscall* WaitForLoad_fn)(void* model, void* dummy);
typedef float*(__stdcall* MatrixRot_fn)(float* outMtx, float* tempMtx, const float* basis, float angle, const float* axis);

typedef void* (__thiscall* BufLock_fn)(uint32_t device, uint32_t buf);
typedef void  (__thiscall* BufUnlock_fn)(uint32_t device, uint32_t buf, uint32_t param);

static InitRenderState_fn pOrigInitRenderState = nullptr;

constexpr int kMaxWorkers   = 7;
constexpr int kMaxGrassFills= 512;

struct MappedBatch {
    uint32_t* batch;
    char*     vertices;
    uint16_t* indices;
    uint32_t  vertexBuf;
    uint32_t  indexBuf;
};

struct WorkerRange {
    int begin;
    int end;
};

#pragma pack(push, 1)
struct M2VertexRef {
    float    pos[3];
    uint8_t  boneWeights[4];
    uint8_t  boneIndices[4];
    float    normal[3];
    float    uv[2];
    float    unk[2];
};

struct DoodadVertexOut {
    float    pos[3];
    float    normal[3];
    uint32_t color;
    float    uv[2];
};
#pragma pack(pop)

static MappedBatch  g_fills[kMaxGrassFills];
static int          g_fillCount = 0;
static WorkerRange  g_ranges[kMaxWorkers];
static volatile LONG g_activeWorkers = 0;

static int    g_workers = 0;
static HANDLE g_wake[kMaxWorkers] = {};
static HANDLE g_threads[kMaxWorkers] = {};
static volatile LONG g_shutdown = 0;

static void*          g_mainTlsBlock = nullptr;
static unsigned short g_fpuCw = 0x7F;
static unsigned int   g_mxcsr = 0x1F80;

static bool          g_installed = false;
static volatile LONG g_retired = 0;
static volatile LONG g_faults = 0;

// Statistics
static uint64_t g_statFrames = 0;
static uint64_t g_statParallelFrames = 0;
static uint64_t g_statTotalBatches = 0;
static uint64_t g_statParallelBatches = 0;
static uint64_t g_statVerifiedBatches = 0;

#pragma check_stack(off)
__declspec(noinline) __declspec(safebuffers) static void FillIndicesFast(uint32_t* batch, uint16_t* outIndices) {
    const uint32_t addCount = batch[6];
    const uint8_t* adds = reinterpret_cast<const uint8_t*>(batch[7]);
    const uint32_t doodadList = *reinterpret_cast<const uint32_t*>(kDoodadListAddr);

    uint32_t cursor = 0;
    uint16_t baseVertex = 0;

    for (uint32_t i = 0; i < addCount; ++i) {
        const uint32_t doodadId = *reinterpret_cast<const uint32_t*>(adds + 44 * i + 4);
        const uint32_t data = *reinterpret_cast<const uint32_t*>(doodadList + 4 * doodadId);
        const uint32_t model = *reinterpret_cast<const uint32_t*>(data + 4);
        const uint32_t shared = *reinterpret_cast<const uint32_t*>(model + 44);
        const uint32_t skinData = *reinterpret_cast<const uint32_t*>(shared + 368);

        const uint32_t indexCount = *reinterpret_cast<const uint32_t*>(skinData + 12);
        const uint16_t* srcIndices = *reinterpret_cast<const uint16_t* const*>(skinData + 16);
        const uint32_t vertexCount = *reinterpret_cast<const uint32_t*>(skinData + 4);

        for (uint32_t j = 0; j < indexCount; ++j) {
            outIndices[cursor++] = baseVertex + srcIndices[j];
        }
        baseVertex += static_cast<uint16_t>(vertexCount);
    }
}

__declspec(noinline) __declspec(safebuffers) static void FillVerticesFast(uint32_t* batch, char* outVertices, bool isRgba, bool vertexShadersReady) {
    const uint32_t addCount = batch[6];
    const uint8_t* adds = reinterpret_cast<const uint8_t*>(batch[7]);
    const uint32_t doodadList = *reinterpret_cast<const uint32_t*>(kDoodadListAddr);
    DoodadVertexOut* pOut = reinterpret_cast<DoodadVertexOut*>(outVertices);

    const MatrixRot_fn fnMatrixRot = reinterpret_cast<MatrixRot_fn>(kMatrixRotAddr);
    static const float kAxisZ[3] = { 0.0f, 0.0f, 1.0f };
    static const float kBasisIdentity[9] = {
        1.0f, 0.0f, 0.0f,
        0.0f, 1.0f, 0.0f,
        0.0f, 0.0f, 1.0f
    };

    for (uint32_t i = 0; i < addCount; ++i) {
        const uint8_t* add = adds + 44 * i;
        const uint32_t doodadId = *reinterpret_cast<const uint32_t*>(add + 4);
        const uint32_t data = *reinterpret_cast<const uint32_t*>(doodadList + 4 * doodadId);
        const uint32_t model = *reinterpret_cast<const uint32_t*>(data + 4);
        const uint32_t shared = *reinterpret_cast<const uint32_t*>(model + 44);
        const uint32_t m2Data = *reinterpret_cast<const uint32_t*>(shared + 336);
        const uint32_t skinData = *reinterpret_cast<const uint32_t*>(shared + 368);
        const uint32_t vertexCount = *reinterpret_cast<const uint32_t*>(skinData + 4);
        const uint16_t* skinIndices = *reinterpret_cast<const uint16_t* const*>(skinData + 8);
        const M2VertexRef* m2Vertices = *reinterpret_cast<const M2VertexRef* const*>(m2Data + 64);

        uint32_t color = *reinterpret_cast<const uint32_t*>(add + 40);
        if (!vertexShadersReady) {
            const uint32_t hi = (color >> 24) & 0xFF;
            const uint32_t factor = 76 * hi + 45874;
            const uint32_t b2 = (((color >> 16) & 0xFF) * factor) >> 16;
            const uint32_t b1 = (((color >> 8) & 0xFF) * factor) >> 16;
            const uint32_t b0 = ((color & 0xFF) * factor) >> 16;
            color = b0 | (b1 << 8) | (b2 << 16) | 0xFF000000;
        }
        if (isRgba) {
            color = (color & 0xFF00FF00) | ((color & 0x00FF0000) >> 16) | ((color & 0x000000FF) << 16);
        }

        const float angle = *reinterpret_cast<const float*>(add + 20);
        const float scale = *reinterpret_cast<const float*>(add + 24);
        const float posX  = *reinterpret_cast<const float*>(add + 8);
        const float posY  = *reinterpret_cast<const float*>(add + 12);
        const float posZ  = *reinterpret_cast<const float*>(add + 16);
        const float normX = *reinterpret_cast<const float*>(add + 28);
        const float normY = *reinterpret_cast<const float*>(add + 32);
        const float normZ = *reinterpret_cast<const float*>(add + 36);

        const uint32_t defPtr = *reinterpret_cast<const uint32_t*>(data);
        const bool use3DMatrix = ((*reinterpret_cast<const uint8_t*>(defPtr + 8) & 1) != 0);

        if (!use3DMatrix) {
            float cosA, sinA;
            __asm {
                fld angle
                fsincos
                fstp cosA
                fstp sinA
            }

            for (uint32_t v = 0; v < vertexCount; ++v) {
                const uint16_t m2Idx = skinIndices[v];
                const M2VertexRef& m2 = m2Vertices[m2Idx];
                const float x = m2.pos[0];
                const float y = m2.pos[1];
                const float z = m2.pos[2];

                float outX, outY, outZ;
                __asm {
                    fld x
                    fmul cosA
                    fld y
                    fmul sinA
                    fsubp st(1), st
                    fmul scale
                    fadd posX
                    fstp outX

                    fld y
                    fmul cosA
                    fld x
                    fmul sinA
                    faddp st(1), st
                    fmul scale
                    fadd posY
                    fstp outY

                    fld z
                    fmul scale
                    fadd posZ
                    fstp outZ
                }

                pOut->pos[0] = outX;
                pOut->pos[1] = outY;
                pOut->pos[2] = outZ;
                pOut->normal[0] = normX;
                pOut->normal[1] = normY;
                pOut->normal[2] = normZ;
                pOut->color = color;
                pOut->uv[0] = m2.uv[0];
                pOut->uv[1] = m2.uv[1];
                pOut++;
            }
        } else {
            const float v19 = normZ;
            const float v20 = -normY;
            const float len = 1.0f / sqrtf(v20 * v20 + v19 * v19);
            const float v24 = v19 * len;
            const float v26 = len * v20;
            const float v99 = normY * v26 - normZ * v24;
            const float v100 = -v26 * normX;
            const float v101 = -v24 * normX;
            const float params[9] = { v99, v100, v101, 0.0f, v24, v26, normX, normY, normZ };

            float tempMtx[9];
            float mtx[9];
            fnMatrixRot(mtx, tempMtx, params, angle, kAxisZ);

            for (uint32_t v = 0; v < vertexCount; ++v) {
                const uint16_t m2Idx = skinIndices[v];
                const M2VertexRef& m2 = m2Vertices[m2Idx];
                const float x = m2.pos[0];
                const float y = m2.pos[1];
                const float z = m2.pos[2];

                const float rX = mtx[3] * y + mtx[0] * x + mtx[6] * z;
                const float rY = mtx[4] * y + mtx[1] * x + mtx[7] * z;
                const float rZ = mtx[8] * z + mtx[2] * x + mtx[5] * y;

                pOut->pos[0] = rX * scale + posX;
                pOut->pos[1] = rY * scale + posY;
                pOut->pos[2] = rZ * scale + posZ;
                pOut->normal[0] = normX;
                pOut->normal[1] = normY;
                pOut->normal[2] = normZ;
                pOut->color = color;
                pOut->uv[0] = m2.uv[0];
                pOut->uv[1] = m2.uv[1];
                pOut++;
            }
        }
    }
}

// Runs a contiguous slice of batch vertex and index generation without stack cookies
__declspec(noinline) __declspec(safebuffers) static void RunSlice(int begin, int end) {
    const uint32_t gxDevice = *reinterpret_cast<const uint32_t*>(kGxDevicePtrAddr);
    const bool isRgba = (gxDevice && *reinterpret_cast<const uint32_t*>(gxDevice + 552) == 1);
    const bool vertexShadersReady = (*reinterpret_cast<const uint32_t*>(kShadersReadyAddr) != 0);

    for (int i = begin; i < end; ++i) {
        MappedBatch& f = g_fills[i];
        if (f.vertices) {
            FillVerticesFast(f.batch, f.vertices, isRgba, vertexShadersReady);
        }
        if (f.indices) {
            FillIndicesFast(f.batch, f.indices);
        }
    }
}
#pragma check_stack()

static DWORD WINAPI WorkerMain(LPVOID p) {
    const int idx = (int)(intptr_t)p;

    // Attach main thread's ObjectManager TLS block so any engine thread-local reads work cleanly
    const DWORD tlsIdx = *(const DWORD*)kTlsIndexAddr;
    void** workerTls = (void**)__readfsdword(0x2C);
    if (workerTls && tlsIdx < 128 && g_mainTlsBlock) {
        workerTls[tlsIdx] = g_mainTlsBlock;
    }

    while (!g_shutdown) {
        WaitForSingleObject(g_wake[idx], INFINITE);
        if (g_shutdown) break;

        const unsigned short cw = g_fpuCw;
        __asm fldcw cw
        _mm_setcsr(g_mxcsr);

        __try {
            RunSlice(g_ranges[idx].begin, g_ranges[idx].end);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            InterlockedIncrement(&g_faults);
            InterlockedExchange(&g_retired, 1);
        }

        InterlockedDecrement(&g_activeWorkers);
    }
    return 0;
}

static bool StartWorkers(int n) {
    for (int i = 0; i < n; ++i) {
        g_wake[i] = CreateEventA(nullptr, FALSE, FALSE, nullptr);
        if (!g_wake[i]) return false;
        g_threads[i] = CreateThread(nullptr, 256 * 1024, WorkerMain, (LPVOID)(intptr_t)i,
                                    STACK_SIZE_PARAM_IS_A_RESERVATION, nullptr);
        if (!g_threads[i]) return false;
        SetThreadPriority(g_threads[i], THREAD_PRIORITY_ABOVE_NORMAL);
        g_workers = i + 1;
    }
    return true;
}

static void QueueBatchFill(uint32_t* batch) {
    const uint32_t texture = batch[0];
    if (!texture) return;

    const TextureGetGxTex_fn fnTextureGet = reinterpret_cast<TextureGetGxTex_fn>(kTextureGetGxTexAddr);
    if (!fnTextureGet(texture, 0, 0)) return;

    uint32_t vertexBuf = batch[3];
    uint32_t indexBuf  = batch[4];

    if (!vertexBuf || *reinterpret_cast<const uint32_t*>(*reinterpret_cast<const uint32_t*>(vertexBuf + 8) + 12) == 2) {
        const AcquireBuffers_fn fnAcquire = reinterpret_cast<AcquireBuffers_fn>(kAcquireBuffersAddr);
        fnAcquire(batch[1], &batch[3], batch[2], &batch[4]);
        vertexBuf = batch[3];
        indexBuf  = batch[4];
    }

    if (!vertexBuf || !indexBuf) return;

    const bool needVertices = (!*reinterpret_cast<const uint8_t*>(vertexBuf + 28) || !*reinterpret_cast<const uint8_t*>(vertexBuf + 29));
    const bool needIndices  = (!*reinterpret_cast<const uint8_t*>(indexBuf + 28)  || !*reinterpret_cast<const uint8_t*>(indexBuf + 29));

    if (!needVertices && !needIndices) return;
    if (g_fillCount >= kMaxGrassFills) return;

    // Preload models on the main thread so workers never block or call WaitForLoad
    const uint32_t addCount = batch[6];
    const uint8_t* adds = reinterpret_cast<const uint8_t*>(batch[7]);
    const uint32_t doodadList = *reinterpret_cast<const uint32_t*>(kDoodadListAddr);
    const WaitForLoad_fn fnWaitForLoad = reinterpret_cast<WaitForLoad_fn>(kWaitForLoadAddr);

    for (uint32_t i = 0; i < addCount; ++i) {
        const uint32_t doodadId = *reinterpret_cast<const uint32_t*>(adds + 44 * i + 4);
        const uint32_t data = *reinterpret_cast<const uint32_t*>(doodadList + 4 * doodadId);
        const uint32_t model = *reinterpret_cast<const uint32_t*>(data + 4);
        if (!(*reinterpret_cast<const uint8_t*>(model + 16) & 1)) {
            fnWaitForLoad(reinterpret_cast<void*>(model), nullptr);
        }
    }

    // Lock buffers on the main thread
    const uint32_t gxDevice = *reinterpret_cast<const uint32_t*>(kGxDevicePtrAddr);
    if (!gxDevice) return;
    uint32_t* const vtbl = *reinterpret_cast<uint32_t**>(gxDevice);
    const BufLock_fn fnLock = reinterpret_cast<BufLock_fn>(vtbl[54]);
    const BufUnlock_fn fnUnlock = reinterpret_cast<BufUnlock_fn>(vtbl[55]);

    char* vertices = nullptr;
    uint16_t* indices = nullptr;

    if (needVertices) {
        vertices = reinterpret_cast<char*>(fnLock(gxDevice, vertexBuf));
        if (!vertices) {
            fnUnlock(gxDevice, vertexBuf, 0);
        }
    }

    if (needIndices) {
        indices = reinterpret_cast<uint16_t*>(fnLock(gxDevice, indexBuf));
        if (!indices) {
            fnUnlock(gxDevice, indexBuf, 0);
        }
    }

    if (!vertices && !indices) return;

    MappedBatch& fill = g_fills[g_fillCount++];
    fill.batch = batch;
    fill.vertices = vertices;
    fill.indices = indices;
    fill.vertexBuf = vertices ? vertexBuf : 0;
    fill.indexBuf = indices ? indexBuf : 0;
}

static void PrepareGrassFills() {
    g_fillCount = 0;
    uintptr_t inst = *reinterpret_cast<const uintptr_t*>(kSortTableListAddr + 8);
    if (!inst || (inst & 1)) return;

    const uint32_t linkOffset = *reinterpret_cast<const uint32_t*>(kSortTableListAddr);

    while (inst && !(inst & 1)) {
        for (int b = 0; b < 4; ++b) {
            uint32_t* batch = reinterpret_cast<uint32_t*>(inst + 4 + 36 * b);
            QueueBatchFill(batch);
        }
        inst = *reinterpret_cast<const uintptr_t*>(inst + linkOffset + 4);
    }

    const int total = g_fillCount;
    if (total == 0) return;

    ++g_statFrames;
    g_statTotalBatches += static_cast<uint64_t>(total);

    const uint32_t gxDevice = *reinterpret_cast<const uint32_t*>(kGxDevicePtrAddr);
    uint32_t* const vtbl = *reinterpret_cast<uint32_t**>(gxDevice);
    const BufUnlock_fn fnUnlock = reinterpret_cast<BufUnlock_fn>(vtbl[55]);

    if (total >= 2 && g_workers > 0) {
        ++g_statParallelFrames;
        g_statParallelBatches += static_cast<uint64_t>(total);

        // Capture FPU control word and MXCSR
        unsigned short cw = 0x7F;
        __asm fstcw cw
        g_fpuCw = cw;
        g_mxcsr = _mm_getcsr();

        const int numChunks = (g_workers + 1 < total) ? (g_workers + 1) : total;
        const int workersToWake = numChunks - 1;
        const int baseChunk = total / numChunks;
        const int extra = total % numChunks;

        int cursor = 0;
        for (int i = 0; i < workersToWake; ++i) {
            const int len = baseChunk + (i < extra ? 1 : 0);
            g_ranges[i].begin = cursor;
            g_ranges[i].end = cursor + len;
            cursor += len;
        }

        const int myBegin = cursor;
        const int myEnd = total;

        if (workersToWake > 0) {
            InterlockedExchange(&g_activeWorkers, workersToWake);
            for (int i = 0; i < workersToWake; ++i) {
                SetEvent(g_wake[i]);
            }
        }

        // Calling thread executes its own chunk
        if (myBegin < myEnd) {
            RunSlice(myBegin, myEnd);
        }

        // Hybrid spin-yield wait for workers
        if (workersToWake > 0) {
            while (g_activeWorkers > 0) {
                YieldProcessor();
            }
        }
    } else {
        // Run serial on calling thread
        RunSlice(0, total);
    }

    // Main thread unlocks all mapped buffers and marks them valid
    for (int i = 0; i < total; ++i) {
        MappedBatch& fill = g_fills[i];
        if (fill.vertexBuf) {
            fnUnlock(gxDevice, fill.vertexBuf, 0);
            *reinterpret_cast<uint8_t*>(fill.vertexBuf + 28) = 1;
        }
        if (fill.indexBuf) {
            fnUnlock(gxDevice, fill.indexBuf, 0);
            *reinterpret_cast<uint8_t*>(fill.indexBuf + 28) = 1;
        }
    }

    g_statVerifiedBatches += static_cast<uint64_t>(total);
}

// Hooked replacement for CDetailDoodad::InitializeRenderState at 0x007B2D30
static int __cdecl Hooked_InitializeRenderState() {
    const int res = pOrigInitRenderState();
    if (!g_installed || g_retired || !Config::g_settings.OptGrassParallel) {
        return res;
    }

    PrepareGrassFills();
    return res;
}

} // namespace

namespace GrassParallel {

void Init() {
    Install();
}

void Install() {
    if (g_installed) return;

    // Verify prologue bytes of sub_7B2D30 before hooking
    const uint8_t expected[9] = { 0x55, 0x8B, 0xEC, 0x8B, 0x0D, 0x88, 0xDF, 0xC5, 0x00 };
    if (memcmp(reinterpret_cast<const void*>(kInitRenderStateAddr), expected, sizeof(expected)) != 0) {
        Log("[GrassParallel] Prologue mismatch at 0x%08X; hook aborted", kInitRenderStateAddr);
        return;
    }

    // Capture main thread's ObjectManager TLS block
    const DWORD tlsIdx = *reinterpret_cast<const DWORD*>(kTlsIndexAddr);
    void** mainTls = reinterpret_cast<void**>(__readfsdword(0x2C));
    if (mainTls && tlsIdx < 128) {
        g_mainTlsBlock = mainTls[tlsIdx];
    }

    // Provision worker threads
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    const int cores = static_cast<int>(si.dwNumberOfProcessors);
    int targetWorkers = (cores > 2) ? (cores - 2) : 1;
    if (targetWorkers > kMaxWorkers) targetWorkers = kMaxWorkers;

    if (!StartWorkers(targetWorkers)) {
        Log("[GrassParallel] Failed to start %d worker threads", targetWorkers);
        Shutdown();
        return;
    }

    MH_STATUS st = MH_CreateHook(reinterpret_cast<LPVOID>(kInitRenderStateAddr),
                                 reinterpret_cast<LPVOID>(Hooked_InitializeRenderState),
                                 reinterpret_cast<LPVOID*>(&pOrigInitRenderState));
    if (st != MH_OK) {
        Log("[GrassParallel] MH_CreateHook failed: %d", st);
        Shutdown();
        return;
    }

    st = MH_EnableHook(reinterpret_cast<LPVOID>(kInitRenderStateAddr));
    if (st != MH_OK) {
        Log("[GrassParallel] MH_EnableHook failed: %d", st);
        MH_RemoveHook(reinterpret_cast<LPVOID>(kInitRenderStateAddr));
        Shutdown();
        return;
    }

    WowOpt_RecordHookOwner(kInitRenderStateAddr, "GrassParallel");
    g_installed = true;
    Log("[GrassParallel] Installed hook on CDetailDoodad::InitializeRenderState (0x%08X, %d workers)",
        kInitRenderStateAddr, g_workers);
}

void Shutdown() {
    if (g_installed) {
        MH_DisableHook(reinterpret_cast<LPVOID>(kInitRenderStateAddr));
        MH_RemoveHook(reinterpret_cast<LPVOID>(kInitRenderStateAddr));
        g_installed = false;
    }

    InterlockedExchange(&g_shutdown, 1);
    for (int i = 0; i < g_workers; ++i) {
        if (g_wake[i]) SetEvent(g_wake[i]);
    }

    for (int i = 0; i < g_workers; ++i) {
        if (g_threads[i]) {
            WaitForSingleObject(g_threads[i], 1000);
            CloseHandle(g_threads[i]);
            g_threads[i] = nullptr;
        }
        if (g_wake[i]) {
            CloseHandle(g_wake[i]);
            g_wake[i] = nullptr;
        }
    }
    g_workers = 0;
}

void LogStats() {
    if (!g_installed) return;
    Log("[GrassParallel] Stats: %llu frames (%llu parallel), %llu batches (%llu parallel), %ld faults%s",
        g_statFrames, g_statParallelFrames, g_statTotalBatches, g_statParallelBatches,
        g_faults, g_retired ? " [RETIRED]" : "");
}

} // namespace GrassParallel
