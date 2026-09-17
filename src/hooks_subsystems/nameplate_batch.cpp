#include "nameplate_batch.h"
#include "lua_optimize.h"
#include "version.h"
#include "MinHook.h"
#include <cstdio>
#include <cstring>
#include <intrin.h>
#include <cmath>

extern "C" void Log(const char* fmt, ...);

// Function Pointers for Detoured WoW Subsystems
typedef void* (__cdecl *ClntObjMgrObjectPtr_fn)(uint64_t guid, int type);
static ClntObjMgrObjectPtr_fn ClntObjMgrObjectPtr = (ClntObjMgrObjectPtr_fn)0x004D4DB0;

typedef const char* (__thiscall *GetUnitNameAndGuild_fn)(void* unit, const char** guildNameOut, int flag);
static GetUnitNameAndGuild_fn GetUnitNameAndGuild = (GetUnitNameAndGuild_fn)0x004FD0E0;

typedef void* (__thiscall *CSimpleFrame_GetColor_fn)(void* frame, DWORD* colorOut);
static CSimpleFrame_GetColor_fn CSimpleFrame_GetColor = (CSimpleFrame_GetColor_fn)0x00487AB0;

typedef void (__thiscall *CSimpleFrame_SetColor_fn)(void* frame, DWORD* color);
static CSimpleFrame_SetColor_fn CSimpleFrame_SetColor = (CSimpleFrame_SetColor_fn)0x00487A10;

typedef void (__thiscall *CSimpleFontString_SetText_fn)(void* fontString, const char* text, int flag);
static CSimpleFontString_SetText_fn CSimpleFontString_SetText = (CSimpleFontString_SetText_fn)0x00483910;

// Hook Function types & original pointers
typedef void (__fastcall *NameplateUpdate_fn)(void* ecx, void* edx, float dt);
static NameplateUpdate_fn orig_NameplateUpdate = nullptr;

typedef void (__fastcall *NameplatePosition_fn)(void* ecx, void* edx, float* cameraPos, int unit);
static NameplatePosition_fn orig_NameplatePosition = nullptr;

// Fixed-size distance cache (replaces std::unordered_map to avoid heap allocs + reduce lock contention)
static constexpr int DISTANCE_CACHE_SIZE = 256;
static constexpr int DISTANCE_CACHE_MASK = DISTANCE_CACHE_SIZE - 1;
struct DistanceCacheEntry {
    void* key;
    float distance;
};
static DistanceCacheEntry g_distanceCache[DISTANCE_CACHE_SIZE] = {};
static SRWLOCK g_distanceSRW = SRWLOCK_INIT;

// WoW descriptor offsets
static constexpr int UNIT_FIELD_HEALTH = 18;
static constexpr int UNIT_FIELD_MAXHEALTH = 26;


// Constants
static constexpr int QUEUE_SIZE = 4096;
static constexpr int QUEUE_MASK = QUEUE_SIZE - 1;
static constexpr int WORKER_THREAD_COUNT = 2;

// Statistics (atomic counters)
static volatile LONG g_tasksQueued = 0;
static volatile LONG g_tasksProcessed = 0;
static volatile LONG g_tasksDropped = 0;
static volatile LONG g_resultsProcessed = 0;
static volatile LONG g_exceptionsHandled = 0;

static volatile LONG g_nameplatesPerSecond = 0;
static volatile LONG g_avgProcessingTimeUs = 0;
static volatile LONG g_queueUtilizationPct = 0;
static volatile LONG g_workerCpuUsagePct = 0;
static volatile LONG g_mainThreadTimeSavedMs = 0;

static volatile LONG g_fpsBeforeOptimization = 0;
static volatile LONG g_fpsAfterOptimization = 0;
static volatile LONG g_fpsImprovementPct = 0;

static volatile LONG g_inputQueueDepth = 0;
static volatile LONG g_outputQueueDepth = 0;
static volatile LONG g_maxInputQueueDepth = 0;
static volatile LONG g_maxOutputQueueDepth = 0;

// ================================================================
// Lock-Free Queues (4096 entries each, ring buffer)
// ================================================================
// Committed on Init, not in BSS: this feature is off by default and the two
// queues together reserved over a megabyte for every player regardless.
static NameplateMT::NameplateTask*   g_inputQueue  = nullptr;
static NameplateMT::NameplateResult* g_outputQueue = nullptr;

static bool EnsureQueues() {
    if (!g_inputQueue) {
        g_inputQueue = (NameplateMT::NameplateTask*)VirtualAlloc(
            nullptr, sizeof(NameplateMT::NameplateTask) * QUEUE_SIZE,
            MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    }
    if (!g_outputQueue) {
        g_outputQueue = (NameplateMT::NameplateResult*)VirtualAlloc(
            nullptr, sizeof(NameplateMT::NameplateResult) * QUEUE_SIZE,
            MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    }
    return g_inputQueue && g_outputQueue;
}
static volatile LONG g_inputHead = 0;  // Consumer index (worker threads)
static volatile LONG g_inputTail = 0;  // Producer index (main thread)
static volatile LONG g_outputHead = 0; // Consumer index (main thread)
static volatile LONG g_outputTail = 0; // Producer index (worker threads)

// Worker Thread State
static HANDLE g_workerThreads[WORKER_THREAD_COUNT] = {NULL};
static volatile bool g_workerShutdown = false;
static HANDLE g_workerEvent = NULL;
static LARGE_INTEGER g_qpcFreq = {0};
static bool g_initialized = false;

// Memory Validation Helpers
static bool IsReadable(uintptr_t addr) {
    if (addr == 0) return false;
    MEMORY_BASIC_INFORMATION mbi;
    if (VirtualQuery((void*)addr, &mbi, sizeof(mbi)) == 0) return false;
    if (mbi.State != MEM_COMMIT) return false;
    return !(mbi.Protect & PAGE_NOACCESS) && !(mbi.Protect & PAGE_GUARD);
}

static bool IsExecutable(uintptr_t addr) {
    if (addr == 0) return false;
    MEMORY_BASIC_INFORMATION mbi;
    if (VirtualQuery((void*)addr, &mbi, sizeof(mbi)) == 0) return false;
    if (mbi.State != MEM_COMMIT) return false;
    return (mbi.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ |
                            PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)) != 0;
}

// Lock-Free Queue Operations

struct SRWLockGuard {
    SRWLOCK* srw;
    SRWLockGuard(SRWLOCK* l) : srw(l) {
        AcquireSRWLockExclusive(srw);
    }
    ~SRWLockGuard() {
        ReleaseSRWLockExclusive(srw);
    }
};

static SRWLOCK s_inputSRW = SRWLOCK_INIT;
static SRWLOCK s_outputSRW = SRWLOCK_INIT;

// Queue a nameplate task for processing (main thread)
static bool QueueNameplateTask(const NameplateMT::NameplateTask* task) {
    SRWLockGuard lock(&s_inputSRW);
    
    LONG tail = g_inputTail;
    LONG head = g_inputHead;
    
    if (((uint32_t)tail - (uint32_t)head) >= (uint32_t)QUEUE_SIZE) {
        InterlockedIncrement(&g_tasksDropped);
        return false;
    }
    
    int slot = tail & QUEUE_MASK;
    memcpy(&g_inputQueue[slot], task, sizeof(NameplateMT::NameplateTask));
    
    g_inputTail++;
    InterlockedIncrement(&g_tasksQueued);
    
    // Update queue depth statistics
    LONG depth = (LONG)((uint32_t)tail - (uint32_t)head + 1);
    InterlockedExchange(&g_inputQueueDepth, depth);
    if (depth > g_maxInputQueueDepth) {
        InterlockedExchange(&g_maxInputQueueDepth, depth);
    }
    
    SetEvent(g_workerEvent);
    return true;
}

// Dequeue a nameplate task for processing (worker thread)
static bool DequeueNameplateTask(NameplateMT::NameplateTask* task) {
    SRWLockGuard lock(&s_inputSRW);
    
    LONG head = g_inputHead;
    LONG tail = g_inputTail;
    
    if (head == tail) return false; // Queue empty
    
    int slot = head & QUEUE_MASK;
    memcpy(task, &g_inputQueue[slot], sizeof(NameplateMT::NameplateTask));
    
    g_inputHead++;
    return true;
}

// Queue a nameplate result for main thread consumption (worker thread)
static bool QueueNameplateResult(const NameplateMT::NameplateResult* result) {
    SRWLockGuard lock(&s_outputSRW);
    
    LONG tail = g_outputTail;
    LONG head = g_outputHead;
    
    if (((uint32_t)tail - (uint32_t)head) >= (uint32_t)QUEUE_SIZE) {
        InterlockedIncrement(&g_tasksDropped);
        return false;
    }
    
    int slot = tail & QUEUE_MASK;
    memcpy(&g_outputQueue[slot], result, sizeof(NameplateMT::NameplateResult));
    
    g_outputTail++;
    
    // Update queue depth statistics
    LONG depth = (LONG)((uint32_t)tail - (uint32_t)head + 1);
    InterlockedExchange(&g_outputQueueDepth, depth);
    if (depth > g_maxOutputQueueDepth) {
        InterlockedExchange(&g_maxOutputQueueDepth, depth);
    }
    
    return true;
}

// Dequeue a nameplate result for UI application (main thread)
static bool DequeueNameplateResult(NameplateMT::NameplateResult* result) {
    SRWLockGuard lock(&s_outputSRW);
    
    LONG head = g_outputHead;
    LONG tail = g_outputTail;
    
    if (head == tail) return false; // Queue empty
    
    int slot = head & QUEUE_MASK;
    memcpy(result, &g_outputQueue[slot], sizeof(NameplateMT::NameplateResult));
    
    g_outputHead++;
    InterlockedIncrement(&g_resultsProcessed);
    return true;
}

// Nameplate Processing Functions (Worker Thread)

// Process health update task
static void ProcessHealthUpdate(const NameplateMT::NameplateTask* task, NameplateMT::NameplateResult* result) {
    __try {
        // Calculate health percentage
        float healthPercent = 0.0f;
        if (task->healthMax > 0) {
            healthPercent = (float)task->healthCurrent / (float)task->healthMax;
        }
        
        // Determine health bar color based on percentage
        DWORD healthBarColor = 0xFF00FF00; // Green
        if (healthPercent < 0.25f) {
            healthBarColor = 0xFFFF0000; // Red
        } else if (healthPercent < 0.50f) {
            healthBarColor = 0xFFFF8000; // Orange
        } else if (healthPercent < 0.75f) {
            healthBarColor = 0xFFFFFF00; // Yellow
        }
        
        result->healthBarColor = healthBarColor;
        result->healthPercent = healthPercent;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        InterlockedIncrement(&g_exceptionsHandled);
        result->healthBarColor = 0xFFFFFFFF; // White (error)
        result->healthPercent = 0.0f;
    }
}

// Process text update task
static void ProcessTextUpdate(const NameplateMT::NameplateTask* task, NameplateMT::NameplateResult* result) {
    __try {
        // Format nameplate text: "Name <Guild>"
        char formatted[128] = {0};
        
        if (task->guildName[0] != '\0') {
            _snprintf_s(formatted, sizeof(formatted), _TRUNCATE, 
                       "%s <%s>", task->unitName, task->guildName);
        } else {
            _snprintf_s(formatted, sizeof(formatted), _TRUNCATE, 
                       "%s", task->unitName);
        }
        
        memcpy(result->formattedText, formatted, sizeof(result->formattedText));
        result->textColor = 0xFFFFFFFF; // White
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        InterlockedIncrement(&g_exceptionsHandled);
        memset(result->formattedText, 0, sizeof(result->formattedText));
        result->textColor = 0xFFFF0000; // Red (error)
    }
}

// Process color update task
static void ProcessColorUpdate(const NameplateMT::NameplateTask* task, NameplateMT::NameplateResult* result) {
    __try {
        // Blend class color with threat color based on threat level
        DWORD finalColor = task->classColor;
        
        if (task->threatLevel > 0.0f) {
            // Blend with threat color (red) based on threat level
            float threatBlend = task->threatLevel;
            if (threatBlend > 1.0f) threatBlend = 1.0f;
            
            BYTE classR = (task->classColor >> 16) & 0xFF;
            BYTE classG = (task->classColor >> 8) & 0xFF;
            BYTE classB = task->classColor & 0xFF;
            
            BYTE threatR = (task->threatColor >> 16) & 0xFF;
            BYTE threatG = (task->threatColor >> 8) & 0xFF;
            BYTE threatB = task->threatColor & 0xFF;
            
            BYTE finalR = (BYTE)(classR * (1.0f - threatBlend) + threatR * threatBlend);
            BYTE finalG = (BYTE)(classG * (1.0f - threatBlend) + threatG * threatBlend);
            BYTE finalB = (BYTE)(classB * (1.0f - threatBlend) + threatB * threatBlend);
            
            finalColor = 0xFF000000 | (finalR << 16) | (finalG << 8) | finalB;
        }
        
        result->finalColor = finalColor;
        result->borderColor = finalColor; // Same as final color for now
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        InterlockedIncrement(&g_exceptionsHandled);
        result->finalColor = 0xFFFFFFFF; // White (error)
        result->borderColor = 0xFFFFFFFF;
    }
}

// Process visibility update task
static void ProcessVisibilityUpdate(const NameplateMT::NameplateTask* task, NameplateMT::NameplateResult* result) {
    __try {
        // Determine visibility based on flags and distance
        BOOL shouldShow = TRUE;
        float alpha = 1.0f;
        
        // Check visibility flags
        if ((task->flags & 0x01) == 0) {
            shouldShow = FALSE;
        }
        
        // Fade based on distance (beyond 40 yards)
        if (task->distance > 40.0f) {
            alpha = 1.0f - ((task->distance - 40.0f) / 20.0f);
            if (alpha < 0.0f) alpha = 0.0f;
            if (alpha < 0.1f) shouldShow = FALSE;
        }
        
        result->shouldShow = shouldShow;
        result->alpha = alpha;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        InterlockedIncrement(&g_exceptionsHandled);
        result->shouldShow = TRUE; // Safe default
        result->alpha = 1.0f;
    }
}

// Worker Thread Procedure
static DWORD WINAPI WorkerThreadProc(LPVOID threadIndex) {
    DWORD workerIndex = (DWORD)(uintptr_t)threadIndex;
    Log("[NameplateMT] Worker thread %d started (TID: %d)", workerIndex, GetCurrentThreadId());

    while (!g_workerShutdown) {
        if (LuaOpt::IsReloading()) { Sleep(10); continue; }

        // Wait for work or timeout (100ms to check shutdown flag)
        WaitForSingleObject(g_workerEvent, 100);
        
        NameplateMT::NameplateTask task;
        while (DequeueNameplateTask(&task)) {
            LARGE_INTEGER startTime;
            QueryPerformanceCounter(&startTime);
            
            NameplateMT::NameplateResult result = {};
            result.nameplate = task.nameplate;
            result.type = task.type;
            
            // Process based on task type
            switch (task.type) {
                case NameplateMT::TASK_HEALTH_UPDATE:
                    ProcessHealthUpdate(&task, &result);
                    break;
                case NameplateMT::TASK_TEXT_UPDATE:
                    ProcessTextUpdate(&task, &result);
                    break;
                case NameplateMT::TASK_COLOR_UPDATE:
                    ProcessColorUpdate(&task, &result);
                    break;
                case NameplateMT::TASK_VISIBILITY_UPDATE:
                    ProcessVisibilityUpdate(&task, &result);
                    break;
            }
            
            LARGE_INTEGER endTime;
            QueryPerformanceCounter(&endTime);
            result.processingTimeUs = (DWORD)((endTime.QuadPart - startTime.QuadPart) * 1000000 / g_qpcFreq.QuadPart);
            
            // Queue result for main thread
            QueueNameplateResult(&result);
            
            InterlockedIncrement(&g_tasksProcessed);
        }
    }

    Log("[NameplateMT] Worker thread %d exiting", workerIndex);
    return 0;
}

// Detoured Hook Functions
static void SaveDistanceToCache(void* ecx, float distance) {
    uint32_t hash = ((uint32_t)(uintptr_t)ecx >> 4) & DISTANCE_CACHE_MASK;
    AcquireSRWLockExclusive(&g_distanceSRW);
    g_distanceCache[hash].key = ecx;
    g_distanceCache[hash].distance = distance;
    ReleaseSRWLockExclusive(&g_distanceSRW);
}

void __fastcall Hooked_NameplatePosition(void* ecx, void* edx, float* cameraPos, int unit) {
    orig_NameplatePosition(ecx, edx, cameraPos, unit);
    
    if (ecx && cameraPos && IsReadable((uintptr_t)ecx) && IsReadable((uintptr_t)cameraPos)) {
        float distance = -1.0f;
        __try {
            float* posState = *(float**)((char*)ecx + 0xC38);
            if (posState && IsReadable((uintptr_t)posState)) {
                float dx = cameraPos[0] - posState[184];
                float dy = cameraPos[1] - posState[185];
                float dz = cameraPos[2] - posState[186];
                distance = sqrtf(dx * dx + dy * dy + dz * dz);
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {
            distance = -1.0f;
        }
        
        if (distance >= 0.0f) {
            SaveDistanceToCache(ecx, distance);
        }
    }
}


static float GetCachedNameplateDistance(void* ecx) {
    uint32_t hash = ((uint32_t)(uintptr_t)ecx >> 4) & DISTANCE_CACHE_MASK;
    AcquireSRWLockShared(&g_distanceSRW);
    float d = (g_distanceCache[hash].key == ecx) ? g_distanceCache[hash].distance : 0.0f;
    ReleaseSRWLockShared(&g_distanceSRW);
    return d;
}

void __fastcall Hooked_NameplateUpdate(void* ecx, void* edx, float dt) {
    orig_NameplateUpdate(ecx, edx, dt);

    if (LuaOpt::IsLoadingMode() || LuaOpt::IsReloading() || LuaOpt::IsSwapping()) return;

    if (!ecx || !IsReadable((uintptr_t)ecx)) return;

    // Skip updating hidden or culled nameplates to avoid layout/dirtying storm
    bool isVisible = (*(uint8_t*)((uintptr_t)ecx + 204) & 0x20) != 0;
    if (!isVisible) return;

    __try {
        uint64_t guid = *(uint64_t*)((char*)ecx + 680);
        if (!guid) return;

        // Throttle non-target updates to prevent queue overflow
        uint64_t targetGuid = *(uint64_t*)0x00BD07B0;
        bool isTarget = (guid == targetGuid);
        if (!isTarget) {
            static uint32_t s_frameCounter = 0;
            s_frameCounter++;
            uintptr_t addrVal = (uintptr_t)ecx;
            if (((addrVal >> 4) + s_frameCounter) % 5 != 0) {
                return;
            }
        }

        void* unit = ClntObjMgrObjectPtr(guid, 8);
        if (!unit || !IsReadable((uintptr_t)unit)) return;

        uintptr_t unitPtr = (uintptr_t)unit;
        if (unitPtr < 0x10000 || unitPtr > 0xFFE00000) return;

        void* descriptors = *(void**)(unitPtr + 0xD0);
        if (!descriptors || !IsReadable((uintptr_t)descriptors)) return;

        int currentHealth = *(int*)((char*)descriptors + UNIT_FIELD_HEALTH * 4);
        int maxHealth = *(int*)((char*)descriptors + UNIT_FIELD_MAXHEALTH * 4);
        int level = *(int*)((char*)descriptors + 54 * 4);

        const char* guildName = nullptr;
        const char* name = GetUnitNameAndGuild(unit, &guildName, 1);
        if (!name) name = "Unknown";

        // Read current health bar color
        DWORD classColor = 0xFFFFFFFF;
        void* healthBar = *(void**)((char*)ecx + 732);
        if (healthBar && IsReadable((uintptr_t)healthBar)) {
            DWORD tempColor = 0;
            CSimpleFrame_GetColor(healthBar, &tempColor);
            classColor = tempColor;
        }

        // Get distance using helper
        float distance = GetCachedNameplateDistance(ecx);

        // Populate tasks
        NameplateMT::NameplateTask task = {};
        task.nameplate = ecx;
        task.priority = 2;
        task.timestamp = GetTickCount();

        // TASK_HEALTH_UPDATE
        task.type = NameplateMT::TASK_HEALTH_UPDATE;
        task.healthCurrent = currentHealth;
        task.healthMax = maxHealth;
        QueueNameplateTask(&task);

        // TASK_TEXT_UPDATE
        task.type = NameplateMT::TASK_TEXT_UPDATE;
        strncpy_s(task.unitName, name, sizeof(task.unitName) - 1);
        if (guildName) {
            strncpy_s(task.guildName, guildName, sizeof(task.guildName) - 1);
        } else {
            task.guildName[0] = '\0';
        }
        task.unitLevel = level;
        QueueNameplateTask(&task);

        // TASK_COLOR_UPDATE
        task.type = NameplateMT::TASK_COLOR_UPDATE;
        task.classColor = classColor;
        task.threatColor = 0xFFFF0000;
        task.threatLevel = 0.0f;
        QueueNameplateTask(&task);

        // TASK_VISIBILITY_UPDATE
        task.type = NameplateMT::TASK_VISIBILITY_UPDATE;
        task.flags = 1; // Visible
        task.distance = distance;
        QueueNameplateTask(&task);
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        InterlockedIncrement(&g_exceptionsHandled);
    }
}

// Public API Implementation
namespace NameplateMT {

bool Init() {
    // Emergency disable check
    #if TEST_DISABLE_NAMEPLATE_MT
    Log("[NameplateMT] Disabled via TEST_DISABLE_NAMEPLATE_MT flag");
    return false;
    #endif

    if (!EnsureQueues()) {
        Log("[NameplateMT] Could not commit the task queues - disabled");
        return false;
    }

    Log("[NameplateMT] Init ");

    // Initialize QPC frequency for time measurements
    QueryPerformanceFrequency(&g_qpcFreq);

    // Initialize queue pointers
    g_inputHead = 0;
    g_inputTail = 0;
    g_outputHead = 0;
    g_outputTail = 0;

    // Initialize SRWLOCKs explicitly
    InitializeSRWLock(&s_inputSRW);
    InitializeSRWLock(&s_outputSRW);
    InitializeSRWLock(&g_distanceSRW);

    // Create worker event
    g_workerEvent = CreateEvent(NULL, FALSE, FALSE, NULL);
    if (!g_workerEvent) {
        Log("[NameplateMT] ERROR: Failed to create worker event");
        return false;
    }

    // Create worker threads
    g_workerShutdown = false;
    for (int i = 0; i < WORKER_THREAD_COUNT; i++) {
        g_workerThreads[i] = CreateThread(NULL, 0, WorkerThreadProc, (LPVOID)(uintptr_t)i, 0, NULL);
        if (!g_workerThreads[i]) {
            Log("[NameplateMT] ERROR: Failed to create worker thread %d", i);
            Shutdown();
            return false;
        }
        
        // Set worker thread priority
        SetThreadPriority(g_workerThreads[i], THREAD_PRIORITY_BELOW_NORMAL);
    }
    // Install hooks using MinHook
    if (MH_CreateHook((LPVOID)0x0098E9F0, (LPVOID)Hooked_NameplateUpdate, (LPVOID*)&orig_NameplateUpdate) != MH_OK) {
        Log("[NameplateMT] ERROR: Failed to create Hooked_NameplateUpdate");
        Shutdown();
        return false;
    }
    if (MH_EnableHook((LPVOID)0x0098E9F0) != MH_OK) {
        Log("[NameplateMT] ERROR: Failed to enable Hooked_NameplateUpdate");
        Shutdown();
        return false;
    }

    if (MH_CreateHook((LPVOID)0x007256C0, (LPVOID)Hooked_NameplatePosition, (LPVOID*)&orig_NameplatePosition) != MH_OK) {
        Log("[NameplateMT] ERROR: Failed to create Hooked_NameplatePosition");
        Shutdown();
        return false;
    }
    if (MH_EnableHook((LPVOID)0x007256C0) != MH_OK) {
        Log("[NameplateMT] ERROR: Failed to enable Hooked_NameplatePosition");
        Shutdown();
        return false;
    }

    g_initialized = true;
    Log("[NameplateMT] [ OK ] Worker threads created (count: %d, queue size: %d, hooks installed)", 
        WORKER_THREAD_COUNT, QUEUE_SIZE);
    return true;
}

void Shutdown() {
    if (!g_initialized) return;

    Log("[NameplateMT] Shutdown");

    // Signal worker threads to exit
    g_workerShutdown = true;
    if (g_workerEvent) SetEvent(g_workerEvent);

    // Wait for worker threads (5 second timeout)
    for (int i = 0; i < WORKER_THREAD_COUNT; i++) {
        if (g_workerThreads[i]) {
            DWORD waitResult = WaitForSingleObject(g_workerThreads[i], 5000);
            if (waitResult == WAIT_TIMEOUT) {
                Log("[NameplateMT] WARNING: Worker thread %d did not exit, terminating", i);
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

    // Disable and remove hooks
    MH_DisableHook((LPVOID)0x0098E9F0);
    MH_RemoveHook((LPVOID)0x0098E9F0);
    MH_DisableHook((LPVOID)0x007256C0);
    MH_RemoveHook((LPVOID)0x007256C0);

    // Clear distance cache
    {
        AcquireSRWLockExclusive(&g_distanceSRW);
        memset(g_distanceCache, 0, sizeof(g_distanceCache));
        ReleaseSRWLockExclusive(&g_distanceSRW);
    }

    // Log final stats
    Log("[NameplateMT] Final stats: Queued=%d, Processed=%d, Dropped=%d, Results=%d",
        g_tasksQueued, g_tasksProcessed, g_tasksDropped, g_resultsProcessed);

    g_initialized = false;
}

static void HandleNameplateReload() {
    NameplateMT::NameplateResult result;
    while (DequeueNameplateResult(&result)) {}
    
    NameplateMT::NameplateTask task;
    while (DequeueNameplateTask(&task)) {}
    
    AcquireSRWLockExclusive(&g_distanceSRW);
    memset(g_distanceCache, 0, sizeof(g_distanceCache));
    ReleaseSRWLockExclusive(&g_distanceSRW);
}

void OnFrame(DWORD mainThreadId) {
    if (!g_initialized) return;
    if (GetCurrentThreadId() != mainThreadId) return;

    if (LuaOpt::IsLoadingMode() || LuaOpt::IsReloading() || LuaOpt::IsSwapping()) {
        HandleNameplateReload();
        return;
    }

    NameplateMT::NameplateResult result;
    int processedCount = 0;
    static constexpr int MAX_RESULTS_PER_FRAME = 32;
    
    // Dequeue results and apply them safely on the main thread
    // Cap processing to avoid stalling the main thread in top-down camera views
    while (processedCount < MAX_RESULTS_PER_FRAME && DequeueNameplateResult(&result)) {
        void* nameplate = result.nameplate;
        if (!nameplate || !IsReadable((uintptr_t)nameplate)) continue;
        
        __try {
            switch (result.type) {
                case NameplateMT::TASK_HEALTH_UPDATE: {
                    void* healthBar = *(void**)((char*)nameplate + 732);
                    if (healthBar && IsReadable((uintptr_t)healthBar)) {
                        uintptr_t* vtable = *(uintptr_t**)healthBar;
                        if (vtable && IsReadable((uintptr_t)vtable)) {
                            typedef void (__thiscall *CSimpleStatusBar_SetValue_fn)(void* thisPtr, float value);
                            CSimpleStatusBar_SetValue_fn SetValue = (CSimpleStatusBar_SetValue_fn)vtable[57];
                            if (SetValue && IsExecutable((uintptr_t)SetValue)) {
                                SetValue(healthBar, result.healthPercent);
                            }
                        }
                    }
                    break;
                }
                case NameplateMT::TASK_TEXT_UPDATE: {
                    void* nameText = *(void**)((char*)nameplate + 720);
                    
                    if (nameText && IsReadable((uintptr_t)nameText)) {
                        CSimpleFontString_SetText(nameText, result.formattedText, 0);
                    }
                    break;
                }
                case NameplateMT::TASK_COLOR_UPDATE: {
                    void* healthBar = *(void**)((char*)nameplate + 732);
                    void* borderFrame = *(void**)((char*)nameplate + 708);
                    
                    if (healthBar && IsReadable((uintptr_t)healthBar)) {
                        DWORD col = result.finalColor;
                        CSimpleFrame_SetColor(healthBar, &col);
                    }
                    if (borderFrame && IsReadable((uintptr_t)borderFrame)) {
                        DWORD col = result.borderColor;
                        CSimpleFrame_SetColor(borderFrame, &col);
                    }
                    break;
                }
                case NameplateMT::TASK_VISIBILITY_UPDATE: {
                    // Let WoW handle visibility and alpha transitions natively to avoid 
                    // rendering state conflicts and camera top-down camera stuttering/freezes.
                    break;
                }
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {
            InterlockedIncrement(&g_exceptionsHandled);
        }
        
        processedCount++;
    }

    // Update stats
    if (processedCount > 0) {
        static DWORD lastStatsTime = GetTickCount();
        static LONG fpsFrames = 0;
        fpsFrames += processedCount;
        DWORD now = GetTickCount();
        if (now - lastStatsTime >= 1000) {
            InterlockedExchange(&g_nameplatesPerSecond, fpsFrames);
            fpsFrames = 0;
            lastStatsTime = now;
        }
    }
}

Stats GetStats() {
    Stats s;
    s.tasksQueued = g_tasksQueued;
    s.tasksProcessed = g_tasksProcessed;
    s.tasksDropped = g_tasksDropped;
    s.resultsProcessed = g_resultsProcessed;
    s.exceptionsHandled = g_exceptionsHandled;
    
    s.nameplatesPerSecond = g_nameplatesPerSecond;
    s.avgProcessingTimeUs = g_avgProcessingTimeUs;
    s.queueUtilizationPct = g_queueUtilizationPct;
    s.workerCpuUsagePct = g_workerCpuUsagePct;
    s.mainThreadTimeSavedMs = g_mainThreadTimeSavedMs;
    
    s.fpsBeforeOptimization = g_fpsBeforeOptimization;
    s.fpsAfterOptimization = g_fpsAfterOptimization;
    s.fpsImprovementPct = g_fpsImprovementPct;
    
    s.inputQueueDepth = g_inputQueueDepth;
    s.outputQueueDepth = g_outputQueueDepth;
    s.maxInputQueueDepth = g_maxInputQueueDepth;
    s.maxOutputQueueDepth = g_maxOutputQueueDepth;

    return s;
}

} // namespace NameplateMT
