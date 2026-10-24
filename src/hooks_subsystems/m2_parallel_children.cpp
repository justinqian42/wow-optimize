// ============================================================================
// Module: m2_parallel_children
//
// In an M2 scene, attached child models (weapons, shields, helm, shoulders,
// cloaks, belts) are animated sequentially inside CM2Model::AnimateAttachmentsMT
// (0x0082E550) during each parent model's animate pass.
//
// In Dalaran or a raid, every player character carries between 4 and 8 attached
// models. When a parent model finishes its own bone transformations, it executes
// the attachment visibility tracks and then loops through m_attachList:
//
//     for (CM2Model* child = this->m_attachList; child; child = child->m_attachNext) {
//         matrix = boneMatrices[attachment->m_bone] * Translate(attachment->m_position);
//         child->AnimateMT(&matrix, ...);
//     }
//
// Each child model is an independent CM2Model instance with its own bone tracks,
// geometry, and particle emitters. The children read parent properties (matrices,
// diffuse/emissive colors, alphas) read-only and mutate only their own state.
//
// This module hooks CM2Model::AnimateAttachmentsMT at 0x0082E550. When an M2 model
// has >= 2 active attached children, the parent's attachment tracks are updated
// via the engine's original code, and then the child AnimateMT calls are distributed
// across a dedicated worker thread pool.
//
// Re-entrancy and safety:
//   - If the worker pool is currently busy (e.g., nested call or another thread),
//     it falls back immediately to sequential execution on the calling thread.
//   - If the model has < 2 active children, it delegates directly to original.
//   - Each worker synchronizes its x87 FPU control word and MXCSR with the calling
//     thread to guarantee bit-identical rounding and precision.
//   - No __try on the hot path; no local array buffers to avoid /GS stack cookies.
//
// Off by default (Graphics_Sound/M2ParallelChildren), experimental.
// ============================================================================

#include "m2_parallel_children.h"
#include <windows.h>
#include <intrin.h>
#include <emmintrin.h>
#include <cstdint>
#include <cstring>
#include "config.h"
#include "MinHook.h"
#include "version.h"

extern "C" void Log(const char* fmt, ...);

namespace M2ParallelChildren {
namespace {

constexpr uintptr_t kTargetFn         = 0x0082E550; // CM2Model::AnimateAttachmentsMT
constexpr uintptr_t kMatrixTranslate  = 0x004C1B30; // C44Matrix::Translate
constexpr uintptr_t kAnimateSimple    = 0x0082E140; // CM2Model::AnimateMTSimple
constexpr uintptr_t kAnimateFull      = 0x0082F0F0; // CM2Model::AnimateMT

const unsigned char kExpectedPrologue[16] = {
    0x55, 0x8B, 0xEC, 0x83, 0xEC, 0x4C, 0x53, 0x8B,
    0xD9, 0x8B, 0x43, 0x2C, 0x8B, 0x80, 0x50, 0x01
};

typedef void (__fastcall* AnimateAttachments_fn)(void* self, void* edx);
typedef void (__fastcall* MatrixTranslate_fn)(void* mat, void* dummy, const float* pos);
typedef void (__fastcall* AnimateModel_fn)(void* child, void* dummy, const float* view,
                                          const float* diff, const float* emiss,
                                          float effAlpha, float alpha);

static AnimateAttachments_fn orig_AnimateAttachmentsMT = nullptr;

constexpr int kMaxWorkers = 7;
constexpr int kMaxTasks   = 32;

struct ChildTask {
    void*        child;
    float        matrix[16];
    const float* diffuse;
    const float* emissive;
    float        effectiveAlpha;
    float        alpha;
    bool         isSimple;
};

struct TaskRange {
    int begin;
    int end;
};

// Global task staging buffer to eliminate stack frame cookies (/GS) on the hot path
static ChildTask  g_tasks[kMaxTasks];
static TaskRange  g_ranges[kMaxWorkers];
static volatile LONG g_activeWorkers = 0;
static volatile LONG g_poolInUse  = 0;

static int    g_numWorkers = 0;
static HANDLE g_threads[kMaxWorkers];
static HANDLE g_wake[kMaxWorkers];
static volatile LONG g_shutdown = 0;

static volatile unsigned short g_fpuCw = 0x7F;
static volatile unsigned int   g_mxcsr = 0x1F80;

static bool g_installed = false;
static bool g_retired   = false;

// Statistics
static volatile LONG g_statCalls       = 0;
static volatile LONG g_statMultiChild  = 0;
static volatile LONG g_statTasksRun    = 0;
static volatile LONG g_statFallbacks   = 0;

inline void FastMatrixTranslate(float* m, const float* pos) {
    const double p0 = (double)pos[0];
    const double p1 = (double)pos[1];
    const double p2 = (double)pos[2];

    m[12] = (float)((double)m[12] + ((double)m[0] * p0 + ((double)m[4] * p1 + (double)m[8] * p2)));
    m[13] = (float)((double)m[13] + ((double)m[1] * p0 + ((double)m[5] * p1 + (double)m[9] * p2)));
    m[14] = (float)((double)m[14] + ((double)m[2] * p0 + ((double)m[6] * p1 + (double)m[10] * p2)));
}

inline void ExecuteChildTask(const ChildTask& t) {
    AnimateModel_fn fn = (AnimateModel_fn)(t.isSimple ? kAnimateSimple : kAnimateFull);
    fn(t.child, nullptr, t.matrix, t.diffuse, t.emissive, t.effectiveAlpha, t.alpha);
}

static DWORD WINAPI WorkerThreadProc(LPVOID param) {
    const int idx = (int)(intptr_t)param;

    while (!g_shutdown) {
        WaitForSingleObject(g_wake[idx], INFINITE);
        if (g_shutdown) break;

        // Synchronize FPU CW and MXCSR with calling thread
        unsigned short cw = g_fpuCw;
        __asm fldcw cw
        _mm_setcsr(g_mxcsr);

        const int begin = g_ranges[idx].begin;
        const int end = g_ranges[idx].end;
        for (int i = begin; i < end; ++i) {
            ExecuteChildTask(g_tasks[i]);
        }

        InterlockedDecrement(&g_activeWorkers);
    }
    return 0;
}

static bool StartWorkers(int count) {
    for (int i = 0; i < count; ++i) {
        g_wake[i] = CreateEventA(nullptr, FALSE, FALSE, nullptr);
        if (!g_wake[i]) return false;

        g_threads[i] = CreateThread(nullptr, 256 * 1024, WorkerThreadProc, (LPVOID)(intptr_t)i,
                                    STACK_SIZE_PARAM_IS_A_RESERVATION, nullptr);
        if (!g_threads[i]) return false;

        SetThreadPriority(g_threads[i], THREAD_PRIORITY_ABOVE_NORMAL);
        g_numWorkers = i + 1;
    }
    return true;
}

void __fastcall Hooked_AnimateAttachmentsMT(void* self, void* edx) {
    InterlockedIncrement(&g_statCalls);

    if (!self || g_retired || !Config::g_settings.OptM2ParallelChildren) {
        orig_AnimateAttachmentsMT(self, edx);
        return;
    }

    void* attachList = *(void**)((uintptr_t)self + 0x58);
    // If no attached children or only a single child, run client's original directly
    if (!attachList || !(*(void**)((uintptr_t)attachList + 0x60))) {
        orig_AnimateAttachmentsMT(self, edx);
        return;
    }

    // Try to acquire the worker pool
    if (InterlockedCompareExchange(&g_poolInUse, 1, 0) != 0) {
        // Pool is busy (nested call or concurrent model on another thread) -> run serial
        InterlockedIncrement(&g_statFallbacks);
        orig_AnimateAttachmentsMT(self, edx);
        return;
    }

    // Run client's attachment track animation by temporarily detaching m_attachList.
    // In sub_82E550, m_attachList (+0x58) is only read starting at 0x0082E5E1 after
    // all attachment tracks have finished updating.
    *(void**)((uintptr_t)self + 0x58) = nullptr;
    orig_AnimateAttachmentsMT(self, edx);
    *(void**)((uintptr_t)self + 0x58) = attachList;

    // Collect child animation tasks
    LONG taskCount = 0;
    void* boneMatrices = *(void**)((uintptr_t)self + 0x98);

    for (void* child = attachList; child && taskCount < kMaxTasks; child = *(void**)((uintptr_t)child + 0x60)) {
        const uint32_t attachIndex = *(uint32_t*)((uintptr_t)child + 0x54);
        ChildTask& t = g_tasks[taskCount];

        if (attachIndex == 0xFFFF) {
            const uint32_t childFlags = *(uint32_t*)((uintptr_t)child + 0x10);
            if (!(childFlags & 0x40000)) continue;
            if (!boneMatrices) continue;
            memcpy(t.matrix, boneMatrices, 64);
        } else {
            uint8_t* attachments = *(uint8_t**)((uintptr_t)self + 0x4C);
            if (!attachments || !attachments[attachIndex * 12 + 8]) continue;

            void* shared = *(void**)((uintptr_t)self + 0x2C);
            if (!shared) continue;
            void* data = *(void**)((uintptr_t)shared + 0x150);
            if (!data) continue;
            uint8_t* attachData = *(uint8_t**)((uintptr_t)data + 0xF4);
            if (!attachData) continue;
            uint8_t* attachment = attachData + attachIndex * 40;

            const uint16_t bone = *(uint16_t*)(attachment + 4);
            if (!boneMatrices) continue;
            memcpy(t.matrix, (uint8_t*)boneMatrices + (bone * 64), 64);

            const float* pos = (const float*)(attachment + 8);
            FastMatrixTranslate(t.matrix, pos);
        }

        t.child = child;
        t.diffuse = (const float*)((uintptr_t)self + 0x1A0);
        t.emissive = (const float*)((uintptr_t)self + 0x1AC);
        t.effectiveAlpha = *(float*)((uintptr_t)self + 0x198);
        t.alpha = *(float*)((uintptr_t)self + 0x19C);
        t.isSimple = (*(uint32_t*)((uintptr_t)child + 0x10) & 0x1000) != 0;
        ++taskCount;
    }

    if (taskCount < 2) {
        // Less than 2 valid tasks: run inline on calling thread
        for (LONG i = 0; i < taskCount; ++i) {
            ExecuteChildTask(g_tasks[i]);
        }
        InterlockedExchange(&g_poolInUse, 0);
        return;
    }

    InterlockedIncrement(&g_statMultiChild);
    InterlockedAdd(&g_statTasksRun, taskCount);

    // Capture FPU control word and MXCSR
    unsigned short cw = 0x7F;
    __asm fnstcw cw
    g_fpuCw = cw;
    g_mxcsr = _mm_getcsr();

    // Partition tasks across workers and calling thread in contiguous ranges
    int totalThreads = g_numWorkers + 1;
    if (totalThreads > taskCount) totalThreads = (int)taskCount;
    const int workersToUse = totalThreads - 1;

    const int base = (int)taskCount / totalThreads;
    const int rem = (int)taskCount % totalThreads;
    int cur = 0;
    for (int i = 0; i < workersToUse; ++i) {
        const int count = base + (i < rem ? 1 : 0);
        g_ranges[i].begin = cur;
        g_ranges[i].end = cur + count;
        cur += count;
    }
    const int mainBegin = cur;
    const int mainEnd = (int)taskCount;

    g_activeWorkers = workersToUse;
    for (int i = 0; i < workersToUse; ++i) {
        SetEvent(g_wake[i]);
    }

    // Calling thread executes its contiguous slice
    for (int i = mainBegin; i < mainEnd; ++i) {
        ExecuteChildTask(g_tasks[i]);
    }

    // Hybrid spin-wait for participating workers without kernel transition overhead
    unsigned spins = 0;
    while (g_activeWorkers > 0) {
        if (++spins < 2000) YieldProcessor(); else SwitchToThread();
    }

    InterlockedExchange(&g_poolInUse, 0);
}

} // namespace

bool Init() {
    if (!Config::g_settings.OptM2ParallelChildren) return true;

    if (RunningUnderTranslation()) {
        Log("[M2ParallelChildren] NOT active: worker threads stay off under translation layer.");
        return false;
    }

    SYSTEM_INFO si;
    GetSystemInfo(&si);
    int workers = (int)si.dwNumberOfProcessors > 2 ? (int)si.dwNumberOfProcessors - 2 : 1;
    if (workers > kMaxWorkers) workers = kMaxWorkers;
    if (workers < 1) {
        Log("[M2ParallelChildren] NOT active: %lu logical CPU(s); needs at least 3 cores.",
            si.dwNumberOfProcessors);
        return false;
    }

    if (memcmp((const void*)kTargetFn, kExpectedPrologue, sizeof(kExpectedPrologue)) != 0) {
        Log("[M2ParallelChildren] NOT hooked: prologue bytes at 0x%08X do not match.",
            (unsigned)kTargetFn);
        return false;
    }

    if (!StartWorkers(workers)) {
        Log("[M2ParallelChildren] NOT active: failed to spawn worker threads.");
        Shutdown();
        return false;
    }

    if (MH_CreateHook((void*)kTargetFn, (void*)Hooked_AnimateAttachmentsMT,
                      (void**)&orig_AnimateAttachmentsMT) != MH_OK ||
        MH_EnableHook((void*)kTargetFn) != MH_OK) {
        Log("[M2ParallelChildren] NOT hooked: MinHook failed at 0x%08X.", (unsigned)kTargetFn);
        Shutdown();
        return false;
    }

    g_installed = true;
    Log("[M2ParallelChildren] Hooked 0x%08X with %d worker threads (experimental).",
        (unsigned)kTargetFn, g_numWorkers);
    return true;
}

void Shutdown() {
    if (g_installed) {
        MH_DisableHook((void*)kTargetFn);
        g_installed = false;
    }

    g_shutdown = 1;
    for (int i = 0; i < g_numWorkers; ++i) {
        if (g_wake[i]) SetEvent(g_wake[i]);
    }

    for (int i = 0; i < g_numWorkers; ++i) {
        if (g_threads[i]) {
            WaitForSingleObject(g_threads[i], 1000);
            CloseHandle(g_threads[i]);
            g_threads[i] = nullptr;
        }
        if (g_wake[i]) { CloseHandle(g_wake[i]); g_wake[i] = nullptr; }
    }
    g_numWorkers = 0;
}

void LogStats() {
    if (!g_installed) return;
    Log("[M2ParallelChildren] calls: %ld, multi-child passes: %ld, child tasks: %ld, serial fallbacks: %ld",
        g_statCalls, g_statMultiChild, g_statTasksRun, g_statFallbacks);
}

}  // namespace M2ParallelChildren
