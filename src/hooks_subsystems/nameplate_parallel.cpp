// ============================================================================
// Module: nameplate_parallel
//
// In CGWorldFrame::OnWorldRender (sub_4F8EA0), the client enumerates all visible
// objects and runs UnitPostAnimateProc (0x004F6A40) on each unit:
//
//     ClntObjMgrEnumVisibleObjects(UnitPostAnimateProc, this);
//
// Which calls CGUnit_C::PostAnimate (0x0072B350):
//
//     float pos[3] = {0, 0, 0};
//     if (this->ShouldShowNamePlate(frame, pos))    // 0x0072B060
//         this->NamePlateAddOrUpdate(pos, frame);   // 0x007256C0
//     else
//         this->DestroyNamePlate();                 // 0x00725840
//
// ShouldShowNamePlate is a pure read-only query that checks hostility, CVars,
// health, distance to camera, and performs 3D WorldToScreen matrix projection
// to compute the screen-space anchor. It does not mutate any game object or UI.
//
// In dense raids, battlegrounds, or Dalaran with 50-150 visible units, evaluating
// ShouldShowNamePlate sequentially takes 0.5 - 1.5 ms on the main thread every frame.
//
// This module intercepts the call to ClntObjMgrEnumVisibleObjects at 0x004F90AF.
// When active:
//   1. Visible units are enumerated into a preallocated static buffer.
//   2. Units with loading or attached models remain sequential to avoid model graph races.
//   3. When >= 4 independent units are present, ShouldShowNamePlate is evaluated
//      in parallel across our worker pool in contiguous slices without atomic contention.
//   4. Worker threads synchronize ObjectManager TLS and FPU/MXCSR with the main thread.
//   5. NamePlateAddOrUpdate and DestroyNamePlate are executed strictly on the main thread
//      in original order, guaranteeing 100% thread safety for all UI frame trees.
//
// Off by default (Graphics_Sound/NameplateParallel), experimental.
// ============================================================================

#include "nameplate_parallel.h"
#include <windows.h>
#include <intrin.h>
#include <emmintrin.h>
#include <cstdint>
#include <cstring>
#include "config.h"
#include "MinHook.h"
#include "version.h"

extern "C" void Log(const char* fmt, ...);

namespace NameplateParallel {
namespace {

constexpr uintptr_t kCallSite           = 0x004F90AF; // call ClntObjMgrEnumVisibleObjects in sub_4F8EA0
constexpr uintptr_t kPushSite           = 0x004F90AA; // push 004F6A40h (UnitPostAnimateProc)
constexpr uintptr_t kEnumVisibleFn      = 0x004D4B30; // ClntObjMgrEnumVisibleObjects
constexpr uintptr_t kUnitPostAnimateProc= 0x004F6A40; // UnitPostAnimateProc
constexpr uintptr_t kShouldShow         = 0x0072B060; // CGUnit_C::ShouldShowNamePlate
constexpr uintptr_t kNamePlateAdd       = 0x007256C0; // CGUnit_C::NamePlateAddOrUpdate
constexpr uintptr_t kDestroyPlate       = 0x00725840; // CGUnit_C::DestroyNamePlate
constexpr uintptr_t kPostAnimate        = 0x0072B350; // CGUnit_C::PostAnimate
constexpr uintptr_t kObjPtrFn           = 0x004D4DB0; // ClntObjMgrObjectPtr
constexpr uintptr_t kTlsIndexAddr       = 0x00D439BC; // TlsIndex for ObjectMgrClient

typedef int   (__cdecl* EnumVisible_fn)(int (__cdecl* cb)(uint32_t, uint32_t, void*), void* frame);
typedef void* (__cdecl* ObjectPtr_fn)(uint32_t guidLow, uint32_t guidHigh, int typeMask);
typedef int   (__thiscall* ShouldShowNamePlate_fn)(void* unit, void* frame, float* pos);
typedef int   (__thiscall* NamePlateAddOrUpdate_fn)(void* unit, float* pos, void* frame);
typedef int   (__thiscall* DestroyNamePlate_fn)(void* unit);
typedef int   (__thiscall* PostAnimate_fn)(void* unit, void* frame);

constexpr int kMaxWorkers      = 7;
constexpr int kMaxVisibleUnits = 512;
constexpr int kMinParallel     = 4;

struct NameplatePrep {
    void*   unit;
    int32_t defer;
    int32_t show;
    float   position[3];
};

struct WorkerRange {
    int begin;
    int end;
};

static NameplatePrep g_units[kMaxVisibleUnits];
static int           g_unitCount = 0;
static WorkerRange   g_ranges[kMaxWorkers];
static volatile LONG g_activeWorkers = 0;

static int    g_workers = 0;
static HANDLE g_wake[kMaxWorkers] = {};
static HANDLE g_threads[kMaxWorkers] = {};
static volatile LONG g_shutdown = 0;

static void*          g_mainTlsBlock = nullptr;
static unsigned short g_fpuCw = 0x7F;
static unsigned int   g_mxcsr = 0x1F80;
static void*          g_currentFrame = nullptr;

static bool          g_installed = false;
static volatile LONG g_retired = 0;
static volatile LONG g_faults = 0;

// Statistics
static uint64_t g_statFrames = 0;
static uint64_t g_statParallelFrames = 0;
static uint64_t g_statTotalUnits = 0;
static uint64_t g_statParallelUnits = 0;
static uint64_t g_statDeferredUnits = 0;

static bool ModelStaysSerial(void* model) {
    if (!model) return false;
    // model + 0x48: m_attachParent, model + 0x58: m_attachList, model + 0x24: m_scene
    if (*(void**)((uintptr_t)model + 0x48) || *(void**)((uintptr_t)model + 0x58) || !*(void**)((uintptr_t)model + 0x24)) {
        return true;
    }
    // model + 0x10: m_fFlags; bit 0 indicates loaded
    const uint32_t flags = *(uint32_t*)((uintptr_t)model + 0x10);
    if (!(flags & 1)) {
        return true;
    }
    return false;
}

static int __cdecl CollectVisibleUnit(uint32_t guidLow, uint32_t guidHigh, void* /*param*/) {
    void* object = ((ObjectPtr_fn)kObjPtrFn)(guidLow, guidHigh, 1);
    if (!object) return 1;

    // Verify TYPEMASK_UNIT (bit 3 of object type flags at descriptors + 8)
    const uintptr_t desc = *(uintptr_t*)((uintptr_t)object + 8);
    if (!desc || !(*(const uint32_t*)(desc + 8) & 8)) return 1;

    if (g_unitCount < kMaxVisibleUnits) {
        NameplatePrep& p = g_units[g_unitCount++];
        p.unit = object;
        p.defer = 0;
        p.show = 0;
        p.position[0] = 0.0f;
        p.position[1] = 0.0f;
        p.position[2] = 0.0f;

        void* model = *(void**)((uintptr_t)object + 0xB8);
        if (ModelStaysSerial(model)) {
            p.defer = 1;
        }
    }
    return 1;
}

// Runs a contiguous slice of ShouldShowNamePlate without locks or stack frame cookies
static void RunSlice(int begin, int end, void* frame) {
    const ShouldShowNamePlate_fn fn = (ShouldShowNamePlate_fn)kShouldShow;
    for (int i = begin; i < end; ++i) {
        NameplatePrep& p = g_units[i];
        if (p.defer) continue;
        p.show = fn(p.unit, frame, p.position);
    }
}

static DWORD WINAPI WorkerMain(LPVOID p) {
    const int idx = (int)(intptr_t)p;

    // Attach main thread's ObjectManager TLS block so ClntObjMgrObjectPtr works seamlessly
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
            RunSlice(g_ranges[idx].begin, g_ranges[idx].end, g_currentFrame);
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

// Hooked call replacement at 0x004F90AF in CGWorldFrame::OnWorldRender
static int __cdecl Hooked_EnumVisibleObjects(int (__cdecl* callback)(uint32_t, uint32_t, void*), void* frame) {
    const EnumVisible_fn orig = (EnumVisible_fn)kEnumVisibleFn;
    if (!g_installed || g_retired || !Config::g_settings.OptNameplateParallel ||
        (uintptr_t)callback != kUnitPostAnimateProc || !frame) {
        return orig(callback, frame);
    }

    g_unitCount = 0;
    orig(CollectVisibleUnit, nullptr);
    const int total = g_unitCount;
    if (total == 0) return 1;

    // Mark shared model instances as deferred so they stay serial
    for (int i = 0; i < total; ++i) {
        if (g_units[i].defer) continue;
        void* m1 = *(void**)((uintptr_t)g_units[i].unit + 0xB8);
        if (!m1) continue;
        for (int j = i + 1; j < total; ++j) {
            if (!g_units[j].defer && *(void**)((uintptr_t)g_units[j].unit + 0xB8) == m1) {
                g_units[i].defer = 1;
                g_units[j].defer = 1;
            }
        }
    }

    int readyCount = 0;
    for (int i = 0; i < total; ++i) {
        if (!g_units[i].defer) ++readyCount;
    }

    ++g_statFrames;
    g_statTotalUnits += (uint64_t)total;
    g_statDeferredUnits += (uint64_t)(total - readyCount);

    if (readyCount >= kMinParallel && g_workers > 0) {
        ++g_statParallelFrames;
        g_statParallelUnits += (uint64_t)readyCount;

        unsigned short cw;
        __asm fnstcw cw
        g_fpuCw = cw;
        g_mxcsr = _mm_getcsr();
        g_currentFrame = frame;

        // Partition units across active workers and main thread in contiguous slices
        int totalThreads = g_workers + 1;
        if (totalThreads > total) totalThreads = total;
        const int workersToUse = totalThreads - 1;

        const int base = total / totalThreads;
        const int rem = total % totalThreads;
        int cur = 0;
        for (int i = 0; i < workersToUse; ++i) {
            const int count = base + (i < rem ? 1 : 0);
            g_ranges[i].begin = cur;
            g_ranges[i].end = cur + count;
            cur += count;
        }
        const int mainBegin = cur;
        const int mainEnd = total;

        g_activeWorkers = workersToUse;
        for (int i = 0; i < workersToUse; ++i) {
            SetEvent(g_wake[i]);
        }

        RunSlice(mainBegin, mainEnd, frame);

        // Hybrid spin-wait without kernel transition
        unsigned spins = 0;
        while (g_activeWorkers > 0) {
            if (++spins < 2000) YieldProcessor(); else SwitchToThread();
        }

        // Apply UI mutations on the main thread in original order
        const NamePlateAddOrUpdate_fn addFn = (NamePlateAddOrUpdate_fn)kNamePlateAdd;
        const DestroyNamePlate_fn destroyFn = (DestroyNamePlate_fn)kDestroyPlate;
        const PostAnimate_fn postFn = (PostAnimate_fn)kPostAnimate;

        for (int i = 0; i < total; ++i) {
            NameplatePrep& p = g_units[i];
            if (p.defer) {
                postFn(p.unit, frame);
            } else if (p.show) {
                addFn(p.unit, p.position, frame);
            } else {
                destroyFn(p.unit);
            }
        }
    } else {
        // Under minimum parallel threshold: execute standard PostAnimate serially on main thread
        const PostAnimate_fn postFn = (PostAnimate_fn)kPostAnimate;
        for (int i = 0; i < total; ++i) {
            postFn(g_units[i].unit, frame);
        }
    }

    return 1;
}

static bool CallTargets(uintptr_t at, uintptr_t want) {
    const uint8_t* p = (const uint8_t*)at;
    if (p[0] != 0xE8) return false;
    const uintptr_t target = at + 5 + *(const int32_t*)(p + 1);
    return target == want;
}

static bool WriteCall(uintptr_t at, const void* to) {
    DWORD old = 0;
    if (!VirtualProtect((void*)at, 5, PAGE_EXECUTE_READWRITE, &old)) return false;
    *(int32_t*)(at + 1) = (int32_t)((uintptr_t)to - (at + 5));
    DWORD ignored = 0;
    VirtualProtect((void*)at, 5, old, &ignored);
    FlushInstructionCache(GetCurrentProcess(), (void*)at, 5);
    return true;
}

} // namespace

void Init() {
    if (!Config::g_settings.OptNameplateParallel) return;

    if (RunningUnderTranslation()) {
        Log("[NameplateParallel] NOT active: worker threads stay off under translation layer.");
        return;
    }

    // Verify call site at 0x004F90AF (call ClntObjMgrEnumVisibleObjects)
    if (!CallTargets(kCallSite, kEnumVisibleFn)) {
        Log("[NameplateParallel] NOT patched: call at 0x%08X is not to 0x%08X.", (unsigned)kCallSite, (unsigned)kEnumVisibleFn);
        return;
    }

    // Verify preceding push at 0x004F90AA (push 004F6A40h)
    const uint8_t* pushInstr = (const uint8_t*)kPushSite;
    if (pushInstr[0] != 0x68 || *(const uintptr_t*)(pushInstr + 1) != kUnitPostAnimateProc) {
        Log("[NameplateParallel] NOT patched: push at 0x%08X is not push 0x%08X.", (unsigned)kPushSite, (unsigned)kUnitPostAnimateProc);
        return;
    }

    if (!WowOpt_ClientPatchAllowed((const void*)kCallSite)) {
        Log("[NameplateParallel] NOT patched: No Client Patches is on.");
        return;
    }

    // Capture main thread's ObjectManager TLS pointer
    const DWORD tlsIdx = *(const DWORD*)kTlsIndexAddr;
    void** mainTls = (void**)__readfsdword(0x2C);
    if (mainTls && tlsIdx < 128) {
        g_mainTlsBlock = mainTls[tlsIdx];
    }
    if (!g_mainTlsBlock) {
        Log("[NameplateParallel] NOT active: main thread TLS block could not be resolved.");
        return;
    }

    SYSTEM_INFO si;
    GetSystemInfo(&si);
    int want = (int)si.dwNumberOfProcessors > 2 ? (int)si.dwNumberOfProcessors - 2 : 1;
    if (want > kMaxWorkers) want = kMaxWorkers;
    if (want < 1) {
        Log("[NameplateParallel] NOT active: insufficient logical CPUs.");
        return;
    }

    if (!StartWorkers(want)) {
        Log("[NameplateParallel] NOT active: failed to spawn %d worker threads.", want);
        Shutdown();
        return;
    }

    if (!WriteCall(kCallSite, (const void*)&Hooked_EnumVisibleObjects)) {
        Log("[NameplateParallel] NOT active: 0x%08X could not be made writable.", (unsigned)kCallSite);
        Shutdown();
        return;
    }

    WowOpt_RecordHookOwner(kCallSite, (const void*)&Hooked_EnumVisibleObjects);
    g_installed = true;
    Log("[NameplateParallel] ACTIVE: parallel nameplate projection armed on %d worker thread(s) at 0x%08X.",
        g_workers, (unsigned)kCallSite);
}

void Shutdown() {
    if (g_installed) {
        WriteCall(kCallSite, (const void*)kEnumVisibleFn);
        g_installed = false;
    }

    g_shutdown = 1;
    for (int i = 0; i < g_workers; ++i) {
        if (g_wake[i]) SetEvent(g_wake[i]);
    }

    for (int i = 0; i < g_workers; ++i) {
        if (g_threads[i]) {
            WaitForSingleObject(g_threads[i], 1000);
            CloseHandle(g_threads[i]);
            g_threads[i] = nullptr;
        }
        if (g_wake[i]) { CloseHandle(g_wake[i]); g_wake[i] = nullptr; }
    }
    g_workers = 0;
}

void LogStats() {
    if (!Config::g_settings.OptNameplateParallel || !g_installed) return;
    if (g_statFrames == 0) {
        Log("[NameplateParallel] running: 0 frames rendered so far.");
        return;
    }
    const double avgUnits = (double)g_statTotalUnits / (double)g_statFrames;
    Log("[NameplateParallel] %s: %llu frames (%llu parallel), avg %.1f units/frame (total: %llu units, %llu parallel, %llu deferred). Faults: %ld.",
        g_retired ? "RETIRED (fault on worker)" : "running",
        g_statFrames, g_statParallelFrames, avgUnits,
        g_statTotalUnits, g_statParallelUnits, g_statDeferredUnits,
        (long)g_faults);
}

} // namespace NameplateParallel
