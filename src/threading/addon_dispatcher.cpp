// ============================================================================
// Module: addon_dispatcher.cpp
// Description: Dispatches addon Update calls asynchronously to thread pools.
// Safety & Threading: Lock-free worker safe.
// ============================================================================

#include "addon_dispatcher.h"
#include "MinHook.h"
#include "lua_optimize.h"
#include <cstdio>
#include <cstring>
#include <intrin.h>
#include <vector>

extern "C" void Log(const char* fmt, ...);

// ================================================================
// Addon Callback Structure
// ================================================================
struct AddonCallback {
    void* frame;          // Frame pointer
    void* callback;       // Callback function pointer
    double elapsed;       // Elapsed time since last call
    int priority;         // Callback priority (0 = normal, 1 = high)
};

// ================================================================
// Input Queue (8192 entries, ring buffer)
// ================================================================
static constexpr int QUEUE_SIZE = 8192;
static constexpr int QUEUE_MASK = QUEUE_SIZE - 1;

struct QueueEntry {
    AddonCallback data;
    volatile LONG ready;  // 1 = ready to process, 0 = empty
};

static QueueEntry g_queue[QUEUE_SIZE] = {};
static volatile LONG g_queueHead = 0;  // Consumer index (worker threads)
static volatile LONG g_queueTail = 0;  // Producer index (main thread)

// ================================================================
// Output Queue (Circular queue with SRWLOCK protection for main thread execution)
// ================================================================
struct SRWLockGuard {
    SRWLOCK* srw;
    SRWLockGuard(SRWLOCK* l) : srw(l) {
        AcquireSRWLockExclusive(srw);
    }
    ~SRWLockGuard() {
        ReleaseSRWLockExclusive(srw);
    }
};

static AddonCallback g_outputQueue[QUEUE_SIZE] = {};
static int g_outputHead = 0;
static int g_outputTail = 0;
static SRWLOCK g_outputSRW = SRWLOCK_INIT;

static bool QueueOutputCallback(const AddonCallback* cb) {
    SRWLockGuard lock(&g_outputSRW);
    int nextTail = (g_outputTail + 1) & QUEUE_MASK;
    if (nextTail == g_outputHead) {
        return false;
    }
    g_outputQueue[g_outputTail] = *cb;
    g_outputTail = nextTail;
    return true;
}

static bool DequeueOutputCallback(AddonCallback* cb) {
    SRWLockGuard lock(&g_outputSRW);
    if (g_outputHead == g_outputTail) {
        return false;
    }
    *cb = g_outputQueue[g_outputHead];
    g_outputHead = (g_outputHead + 1) & QUEUE_MASK;
    return true;
}

// ================================================================
// Lua 5.1 stack interaction helper structures and inline functions
// ================================================================
struct lua_State;
struct RawTValue {
    union {
        void*     gc;
        uintptr_t ptr;
        double    n;
        uint32_t  raw[2];
    } value;
    int       tt;
    uint32_t  taint;
};

static inline RawTValue* GetStackTopFast(lua_State* L) {
    return *(RawTValue**)((uintptr_t)L + 0x0C);
}

static inline void SetStackTopFast(lua_State* L, RawTValue* top) {
    *(RawTValue**)((uintptr_t)L + 0x0C) = top;
}


// ================================================================
// Batch Processing State
// ================================================================
static std::vector<AddonCallback> g_currentBatch;
static SRWLOCK g_batchLock = SRWLOCK_INIT;

// ================================================================
// Statistics (atomic counters)
// ================================================================
static volatile LONG g_callbacksQueued = 0;
static volatile LONG g_callbacksProcessed = 0;
static volatile LONG g_callbacksDropped = 0;
static volatile LONG g_batchesProcessed = 0;
static double g_totalProcessTimeMs = 0.0;
static double g_totalBatchSize = 0.0;
static SRWLOCK g_statsLock = SRWLOCK_INIT;

// ================================================================
// Worker Thread Pool State
// ================================================================
static constexpr int WORKER_THREAD_COUNT = 4;
static HANDLE g_workerThreads[WORKER_THREAD_COUNT] = {};
static volatile bool g_workerShutdown = false;
static HANDLE g_workerEvent = NULL;
static double g_qpcFreqMs = 0.0;

// ================================================================
// Hook State
// ================================================================
static bool g_initialized = false;

// ================================================================
// Memory Validation Helpers
// ================================================================
static bool IsReadable(uintptr_t addr) {
    if (addr == 0) return false;
    MEMORY_BASIC_INFORMATION mbi;
    if (VirtualQuery((void*)addr, &mbi, sizeof(mbi)) == 0) return false;
    if (mbi.State != MEM_COMMIT) return false;
    return !(mbi.Protect & PAGE_NOACCESS) && !(mbi.Protect & PAGE_GUARD);
}

// ================================================================
// Callback Processing (Worker Thread)
// ================================================================
static void ProcessCallback(const AddonCallback* callback) {
    // Validate pointers before queueing
    if (!IsReadable((uintptr_t)callback->frame) || 
        !IsReadable((uintptr_t)callback->callback)) {
        return;
    }

    // Pre-fetch structures to keep cache lines hot
    __try {
        volatile char dummy1 = *(volatile char*)callback->frame;
        volatile char dummy2 = *(volatile char*)callback->callback;
        (void)dummy1;
        (void)dummy2;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        return; // Guard against page faults during pre-fetch
    }

    // Queue validated callback to output queue for main thread execution
    QueueOutputCallback(callback);
}

// ================================================================
// Worker Thread Procedure
// ================================================================
static DWORD WINAPI WorkerThreadProc(LPVOID) {
    Log("[AddonDispatcher] Worker thread started (TID: %d)", GetCurrentThreadId());

    while (!g_workerShutdown) {
        // Wait for events (1ms timeout to check shutdown flag)
        WaitForSingleObject(g_workerEvent, 1);

        // Process all available callbacks
        while (true) {
            LONG head = g_queueHead;
            LONG tail = InterlockedCompareExchange(&g_queueTail, 0, 0);
            if (head == tail) {
                break; // Queue empty
            }

            LONG nextHead = head + 1;
            if (InterlockedCompareExchange(&g_queueHead, nextHead, head) == head) {
                int slot = head & QUEUE_MASK;
                QueueEntry* entry = &g_queue[slot];

                // Spin-wait briefly if the producer has reserved the slot but not yet set ready=1
                int spins = 0;
                while (!entry->ready && !g_workerShutdown && spins < 1000) {
                    _mm_pause();
                    spins++;
                }

                if (entry->ready) {
                    ProcessCallback(&entry->data);
                    InterlockedExchange(&entry->ready, 0);
                }
            }
        }
    }

    Log("[AddonDispatcher] Worker thread exiting");
    return 0;
}

// ================================================================
// Batch Collection and Dispatch
// ================================================================
static void CollectCallback(void* frame, void* callback, double elapsed) {
    if (!g_initialized) return;
    if (LuaOpt::IsReloading() || LuaOpt::IsSwapping()) return;

    // Add to current batch
    AcquireSRWLockExclusive(&g_batchLock);
    
    AddonCallback cb;
    cb.frame = frame;
    cb.callback = callback;
    cb.elapsed = elapsed;
    cb.priority = 0;
    
    g_currentBatch.push_back(cb);
    
    ReleaseSRWLockExclusive(&g_batchLock);
}

static void DispatchBatch() {
    if (!g_initialized) return;

    AcquireSRWLockExclusive(&g_batchLock);
    
    if (g_currentBatch.empty()) {
        ReleaseSRWLockExclusive(&g_batchLock);
        return;
    }

    LARGE_INTEGER start, end;
    QueryPerformanceCounter(&start);

    // Queue all callbacks from batch
    for (const auto& callback : g_currentBatch) {
        LONG tail = InterlockedIncrement(&g_queueTail) - 1;
        int slot = tail & QUEUE_MASK;

        QueueEntry* queueEntry = &g_queue[slot];

        // Check if slot is still being processed (queue overflow)
        if (!queueEntry->ready) {
            queueEntry->data = callback;
            InterlockedExchange(&queueEntry->ready, 1);
            InterlockedIncrement(&g_callbacksQueued);
        } else {
            InterlockedIncrement(&g_callbacksDropped);
        }
    }

    // Signal worker threads
    SetEvent(g_workerEvent);

    QueryPerformanceCounter(&end);
    double processTimeMs = (double)(end.QuadPart - start.QuadPart) / g_qpcFreqMs;

    // Update stats
    AcquireSRWLockExclusive(&g_statsLock);
    g_totalProcessTimeMs += processTimeMs;
    g_totalBatchSize += (double)g_currentBatch.size();
    ReleaseSRWLockExclusive(&g_statsLock);

    InterlockedIncrement(&g_batchesProcessed);

    // Clear batch
    g_currentBatch.clear();
    
    ReleaseSRWLockExclusive(&g_batchLock);
}

// ================================================================
// Public API Implementation
// ================================================================
namespace AddonDispatcher {

bool Init() {
    Log("[AddonDispatcher] Init ");

    // Reset output queue pointers
    g_outputHead = 0;
    g_outputTail = 0;

    // Initialize SRWLOCK explicitly
    InitializeSRWLock(&g_outputSRW);

    // Initialize QPC frequency
    LARGE_INTEGER freq;
    QueryPerformanceFrequency(&freq);
    g_qpcFreqMs = (double)freq.QuadPart / 1000.0;

    // Create worker event
    g_workerEvent = CreateEvent(NULL, FALSE, FALSE, NULL);
    if (!g_workerEvent) {
        Log("[AddonDispatcher] ERROR: Failed to create worker event");
        return false;
    }

    // Create worker thread pool
    g_workerShutdown = false;
    for (int i = 0; i < WORKER_THREAD_COUNT; i++) {
        g_workerThreads[i] = CreateThread(NULL, 0, WorkerThreadProc, NULL, 0, NULL);
        if (!g_workerThreads[i]) {
            Log("[AddonDispatcher] ERROR: Failed to create worker thread %d", i);
            Shutdown();
            return false;
        }
        // Set worker thread priority
        SetThreadPriority(g_workerThreads[i], THREAD_PRIORITY_BELOW_NORMAL);
    }

    g_initialized = true;
    Log("[AddonDispatcher] [ OK ] Worker thread pool created (%d threads, queue size: %d)", 
        WORKER_THREAD_COUNT, QUEUE_SIZE);
    return true;
}

void Shutdown() {
    if (!g_initialized) return;

    Log("[AddonDispatcher] Shutdown");

    // Signal worker threads to exit
    g_workerShutdown = true;
    if (g_workerEvent) SetEvent(g_workerEvent);

    // Wait for worker threads (5 second timeout each)
    for (int i = 0; i < WORKER_THREAD_COUNT; i++) {
        if (g_workerThreads[i]) {
            DWORD waitResult = WaitForSingleObject(g_workerThreads[i], 5000);
            if (waitResult == WAIT_TIMEOUT) {
                Log("[AddonDispatcher] WARNING: Worker thread %d did not exit, terminating", i);
                TerminateThread(g_workerThreads[i], 1);
            }
            CloseHandle(g_workerThreads[i]);
            g_workerThreads[i] = NULL;
        }
    }

    // Cleanup event
    if (g_workerEvent) {
        CloseHandle(g_workerEvent);
        g_workerEvent = NULL;
    }

    // Clear batch
    AcquireSRWLockExclusive(&g_batchLock);
    g_currentBatch.clear();
    ReleaseSRWLockExclusive(&g_batchLock);

    // Clear output queue
    {
        SRWLockGuard lock(&g_outputSRW);
        g_outputHead = 0;
        g_outputTail = 0;
    }

    // Log final stats
    Log("[AddonDispatcher] Final stats: Queued=%d, Processed=%d, Dropped=%d, Batches=%d",
        g_callbacksQueued, g_callbacksProcessed, g_callbacksDropped, g_batchesProcessed);

    g_initialized = false;
}

void OnFrame(DWORD mainThreadId) {
    if (!g_initialized) return;
    if (GetCurrentThreadId() != mainThreadId) return;

    if (LuaOpt::IsReloading() || LuaOpt::IsSwapping()) {
        // Discard all pending callbacks during reload
        AddonCallback cb;
        while (DequeueOutputCallback(&cb)) {}
        
        AcquireSRWLockExclusive(&g_batchLock);
        g_currentBatch.clear();
        ReleaseSRWLockExclusive(&g_batchLock);
        
        // Also clean the input queue
        LONG tail = g_queueTail;
        LONG head = g_queueHead;
        while (head != tail) {
            int slot = head & QUEUE_MASK;
            g_queue[slot].ready = 0;
            head = head + 1;
        }
        InterlockedExchange(&g_queueHead, tail);
        return;
    }

    // Dequeue and execute validated callbacks sequentially on the main thread
    AddonCallback cb;
    lua_State* L = *(lua_State**)0x00D3F78C;
    
    typedef int  (__cdecl *lua_gettop_fn)(lua_State* L);
    typedef void (__cdecl *lua_settop_fn)(lua_State* L, int idx);
    lua_gettop_fn p_lua_gettop = (lua_gettop_fn)0x0084DBD0;
    lua_settop_fn p_lua_settop = (lua_settop_fn)0x0084DBF0;

    while (DequeueOutputCallback(&cb)) {
        if (L && IsReadable((uintptr_t)cb.frame) && IsReadable((uintptr_t)cb.callback)) {
            int topBefore = 0;
            __try { topBefore = p_lua_gettop(L); } __except(EXCEPTION_EXECUTE_HANDLER) {}
            __try {
                // Push callback (closure)
                RawTValue* top = GetStackTopFast(L);
                top->tt = 6; // LUA_TFUNCTION
                top->value.gc = cb.callback;
                SetStackTopFast(L, top + 1);
                
                // Push self (frame table) using virtual function PushSelf
                uintptr_t* vtable = *(uintptr_t**)cb.frame;
                typedef void (__thiscall *CSimpleFrame_PushSelf_fn)(void* frame, lua_State* L);
                CSimpleFrame_PushSelf_fn PushSelf = (CSimpleFrame_PushSelf_fn)vtable[4];
                PushSelf(cb.frame, L);
                
                // Push elapsed (double)
                typedef void (__cdecl *lua_pushnumber_fn)(lua_State* L, double n);
                lua_pushnumber_fn p_lua_pushnumber = (lua_pushnumber_fn)0x0084E2A0;
                p_lua_pushnumber(L, cb.elapsed);
                
                // Call via lua_pcall(L, 2, 0, 0)
                typedef int (__cdecl *lua_pcall_fn)(lua_State* L, int nargs, int nresults, int errfunc);
                lua_pcall_fn p_lua_pcall = (lua_pcall_fn)0x0084EC50;
                p_lua_pcall(L, 2, 0, 0);
            }
            __except (EXCEPTION_EXECUTE_HANDLER) {
                // Exception during push or pcall — stack is dirty
            }
            // CRITICAL: Restore stack to pre-callback state.
            // lua_pcall on error pushes 1 error message, and exceptions during
            // the manual push phase leave 1-3 values. Without this cleanup,
            // the next frame's lua_gettop(L)!=0 check fires Fatal Condition #134.
            __try { p_lua_settop(L, topBefore); } __except(EXCEPTION_EXECUTE_HANDLER) {}
        }
        InterlockedIncrement(&g_callbacksProcessed);
    }

    // Dispatch current batch at end of frame
    DispatchBatch();
}

Stats GetStats() {
    Stats s;
    s.callbacksQueued = g_callbacksQueued;
    s.callbacksProcessed = g_callbacksProcessed;
    s.callbacksDropped = g_callbacksDropped;
    s.batchesProcessed = g_batchesProcessed;
    
    LONG head = g_queueHead;
    LONG tail = g_queueTail;
    LONG depth = (tail - head) & 0x7FFFFFFF;
    if (depth > QUEUE_SIZE) depth = QUEUE_SIZE;
    s.queueDepth = depth;

    AcquireSRWLockShared(&g_statsLock);
    s.totalProcessTimeMs = g_totalProcessTimeMs;
    s.avgBatchSize = (g_batchesProcessed > 0) ? 
       (g_totalBatchSize / (double)g_batchesProcessed) : 0.0;
    ReleaseSRWLockShared(&g_statsLock);

    return s;
}

} // namespace AddonDispatcher
