// async_time_fast.cpp: Accelerated asynchronous timer conversion (sub_86ADC0).
//
// In profiling sessions (wow_optimize_2026-10-22_16-54-34.log), timer routines
// at 0x0086AE00/0x0086ADC0 were sampled repeatedly under timer querying, network polling,
// animation updates, and particle ticks (over 100 client callers).
//
// The stock client's sub_86ADC0 converts high-precision performance counter timestamps
// to milliseconds via serialized x87 instructions: fild qword -> fmul qword -> fadd qword
// followed by a call to __ftol2. __ftol2 executes two fldcw (control word) modifications
// to force truncation rounding, causing FPU instruction pipeline stalls on every call.
//
// This replacement:
//   1. Evaluates (qpc * scale + offset) directly in IEEE double precision using SSE2,
//      performing float-to-int64 conversion without FPU control word thrashing.
//   2. Preserves exact bit-exact double precision arithmetic semantics and mode checks.
//   3. Compiles with __declspec(safebuffers) ensuring zero /GS security cookies and zero
//      SEH frames on the hot path.
//   4. Checks 16-byte prologue signature before hooking; off by default under AsyncTimeFast (experimental: true).

#include "async_time_fast.h"
#include "MinHook.h"
#include "version.h"
#include "config.h"
#include "ab_test.h"
#include "sampling_profiler.h"
#include <cstdint>
#include <cstring>

extern "C" void Log(const char* fmt, ...);

namespace AsyncTimeFast {

namespace {

static const uintptr_t kTarget = 0x0086ADC0;

// Verbatim prologue bytes of sub_86ADC0 (16 bytes):
// 55 8B EC 83 EC 10 56 8B F1 8B 46 08 83 E8 01 74
static const uint8_t kExpectedPrologue[16] = {
    0x55, 0x8B, 0xEC, 0x83, 0xEC, 0x10, 0x56, 0x8B,
    0xF1, 0x8B, 0x46, 0x08, 0x83, 0xE8, 0x01, 0x74
};

typedef uint64_t (__thiscall *AsyncTimeGetTimeFn)(void* this_ptr);
static AsyncTimeGetTimeFn g_orig_AsyncTime_GetTime = nullptr;

static bool g_installed = false;
static bool g_abSubject = false;
static bool g_retired = false;

static uint64_t g_calls = 0;
static uint64_t g_qpcCalls = 0;
static uint64_t g_tickCalls = 0;

__declspec(safebuffers)
static uint64_t __fastcall Hook_AsyncTime_GetTime(void* this_ptr, void* /*edx*/) {
    if (g_retired || (g_abSubject && AbTest::StandAside())) {
        return g_orig_AsyncTime_GetTime(this_ptr);
    }

    ++g_calls;
    const uintptr_t base = reinterpret_cast<uintptr_t>(this_ptr);
    const uint32_t mode = *reinterpret_cast<const uint32_t*>(base + 8);
    const double scale = *reinterpret_cast<const double*>(base);
    const double offset = *reinterpret_cast<const double*>(base + 0x18);

    if (mode == 2) {
        ++g_qpcCalls;
        LARGE_INTEGER qpc;
        QueryPerformanceCounter(&qpc);
        const double ms = static_cast<double>(qpc.QuadPart) * scale + offset;
        return static_cast<uint64_t>(static_cast<int64_t>(ms));
    } else {
        ++g_tickCalls;
        const DWORD tc = GetTickCount();
        const double ms = static_cast<double>(tc) * scale + offset;
        return static_cast<uint64_t>(static_cast<int64_t>(ms));
    }
}

} // namespace

bool Init() {
    if (!Config::g_settings.OptAsyncTimeFast) {
        Log("AsyncTimeFast: not active: switched off by config.");
        return false;
    }

    if (!WowOpt_ClientPatchAllowed(reinterpret_cast<const void*>(kTarget))) {
        Log("AsyncTimeFast: client patch not allowed at hook target 0x%08X", kTarget);
        return false;
    }

    if (memcmp(reinterpret_cast<const void*>(kTarget), kExpectedPrologue, sizeof(kExpectedPrologue)) != 0) {
        Log("AsyncTimeFast: BAD PROLOGUE at 0x%08X! Hook aborted.", kTarget);
        return false;
    }

    MH_STATUS st = WineSafe_CreateHook(
        reinterpret_cast<void*>(kTarget),
        reinterpret_cast<void*>(&Hook_AsyncTime_GetTime),
        reinterpret_cast<void**>(&g_orig_AsyncTime_GetTime)
    );
    if (st != MH_OK) {
        Log("AsyncTimeFast: WineSafe_CreateHook failed: %d", st);
        return false;
    }

    st = WO_EnableHook(reinterpret_cast<void*>(kTarget));
    if (st != MH_OK) {
        Log("AsyncTimeFast: WO_EnableHook failed: %d", st);
        MH_DisableHook(reinterpret_cast<void*>(kTarget));
        return false;
    }

    SamplingProfiler::RegisterSelfSymbol("AsyncTimeFast_Hook", (const void*)&Hook_AsyncTime_GetTime);

    g_abSubject = AbTest::IsSubject("AsyncTimeFast", &g_abSubject);
    g_installed = true;

    Log("AsyncTimeFast: ACTIVE on sub_86ADC0 (timer timestamp conversion).");
    if (g_abSubject) {
        Log("AsyncTimeFast: under A/B test: its OFF stints forward to the original.");
    }
    return true;
}

void LogStats() {
    if (!Config::g_settings.OptAsyncTimeFast) {
        Log("AsyncTimeFast: not measured: switched off.");
        return;
    }
    if (!g_installed) {
        Log("AsyncTimeFast: not installed.");
        return;
    }

    Log("[AsyncTimeFast] calls: %llu (QPC: %llu, TickCount: %llu)",
        g_calls, g_qpcCalls, g_tickCalls);
}

void Shutdown() {
    if (!g_installed) return;
    MH_DisableHook(reinterpret_cast<void*>(kTarget));
    g_installed = false;
}

} // namespace AsyncTimeFast
