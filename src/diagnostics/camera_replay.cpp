// ============================================================================
// Module: camera_replay.cpp
//
// A benchmark that can be run twice the same way.
//
// FrameBench records every presented frame, and two sessions still cannot be
// compared: a player walks somewhere else and looks somewhere else, and the
// frame rate follows what is on screen far more than any setting does. So every
// comparison in this project has either waited on a tester's impression or put
// two runs side by side that were not the same run.
//
// This records what the camera looks at and plays it back. Moving the
// character is not an option, because movement goes to the server, so the
// replay is of the view from where the character stands: record from a spot,
// play back from the same spot facing the same way, and every frame of the
// playback shows the same part of the world in the same order. FrameBench
// measures the playback as its own window.
//
// What is written, and where. CGWorldFrame's per-frame pass, sub_4FA5F0, calls
// sub_607B00(0, camera) - its only caller - to update the world camera, and only
// afterwards reads the camera's position at +8 and hands it to the world update
// and the renderer. The camera is *(dword_B7436C + 0x7E20), which is what
// sub_4F5960 returns to every Lua camera function. The values replayed are the
// three the client's own SetView, sub_603330, restores when it switches to a
// saved view immediately - +280, +284 and +288 - with the smoothing targets it
// writes beside them at +488, +608 and +560, and the two it zeroes at +304 and
// +584 (0x00603617 to 0x00603657). +284 and +288 are yaw and pitch in radians:
// CommentatorGetCamera returns them multiplied by 57.29578. +280 is the third
// saved-view value and is replayed without a claim about what it measures. +64
// is the field of view CommentatorSetCamera writes. All of it is written before
// the update runs, so the camera derives its position from these values the way
// it does from its own, and everything downstream sees one consistent view.
//
// Playback follows the recording's clock rather than its frame count. A faster
// build draws more frames of the same motion in the same time, and that is what
// a frame time distribution should be compared over.
//
// Not replayed: anything that moves on its own. Other players, NPCs, weather
// and particles are whatever they are at the time, so repeat each side of a
// comparison and compare the spread, not a single run.
//
// Not confirmed in a running client yet: that the update keeps these values
// rather than overwriting them from input or from the character's facing.
// The first playback says so either way - the view either follows the
// recording or it does not.
// ============================================================================

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "MinHook.h"
#include "version.h"
#include "config.h"
#include "frame_bench.h"
#include "camera_replay.h"

extern "C" void Log(const char* fmt, ...);
namespace LuaOpt { bool IsLoadingMode(); }

namespace CameraReplay {
namespace {

// __cdecl: the call at 0x004FA7B7 is followed by add esp, 8.
typedef int (__cdecl* CameraUpdate_fn)(int, void*);
CameraUpdate_fn orig_CameraUpdate = nullptr;

const uintptr_t kCameraUpdate   = 0x00607B00;
const uintptr_t kWorldFrame     = 0x00B7436C;
const uintptr_t kWorldCameraOff = 0x7E20;

const uintptr_t kFov         = 64;
const uintptr_t kViewA       = 280;
const uintptr_t kYaw         = 284;
const uintptr_t kPitch       = 288;
const uintptr_t kZeroA       = 304;
const uintptr_t kViewATarget = 488;
const uintptr_t kPitchTarget = 560;
const uintptr_t kZeroB       = 584;
const uintptr_t kYawTarget   = 608;

constexpr float kPi = 3.14159265f;

struct Sample {
    float tMs;
    float viewA;
    float yaw;
    float pitch;
    float fov;
};

// Two minutes at 600 frames a second. Allocated on first use, above 2GB when
// the client allows it, so a switched-off module costs the low half nothing.
const DWORD kMaxSamples  = 72000;
const DWORD kFileMagic   = 0x52434F57;  // "WOCR"
const DWORD kFileVersion = 1;

Sample* g_track = nullptr;
DWORD   g_trackCount = 0;
bool    g_trackFromFile = false;
char    g_trackPath[MAX_PATH] = "";

bool          g_installed = false;
int           g_key = 0;
bool          g_keyWasDown = false;
LARGE_INTEGER g_freq = {};

enum class Mode { Idle, Recording, Playing };
Mode          g_mode = Mode::Idle;
LARGE_INTEGER g_modeStart = {};
void*         g_modeCamera = nullptr;
DWORD         g_playCursor = 0;

unsigned    g_recordings = 0;
unsigned    g_playsStarted = 0;
unsigned    g_playsFinished = 0;
unsigned    g_playsCut = 0;
unsigned    g_framesWritten = 0;
unsigned    g_wrapSnaps = 0;
const char* g_lastCutReason = nullptr;

double ElapsedMs() {
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    return (double)(now.QuadPart - g_modeStart.QuadPart) * 1000.0 /
           (double)g_freq.QuadPart;
}

void* ActiveCamera() {
    __try {
        const uintptr_t worldFrame = *(uintptr_t*)kWorldFrame;
        if (!worldFrame) return nullptr;
        return *(void**)(worldFrame + kWorldCameraOff);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}

bool OurWindowIsForeground() {
    HWND fg = GetForegroundWindow();
    if (!fg) return false;
    DWORD pid = 0;
    GetWindowThreadProcessId(fg, &pid);
    return pid == GetCurrentProcessId();
}

// Beside wow_opt.ini: in WTF when that folder exists, which is where
// Config::Load puts the ini, otherwise next to the executable.
void ResolveTrackPath() {
    char dir[MAX_PATH];
    const DWORD n = GetModuleFileNameA(NULL, dir, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) {
        lstrcpynA(g_trackPath, "wow_opt_camera_track.bin", MAX_PATH);
        return;
    }
    char* slash = strrchr(dir, '\\');
    if (slash) slash[1] = '\0';
    char wtf[MAX_PATH];
    _snprintf(wtf, sizeof(wtf) - 1, "%sWTF", dir);
    wtf[sizeof(wtf) - 1] = '\0';
    const DWORD attr = GetFileAttributesA(wtf);
    if (attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY)) {
        _snprintf(g_trackPath, sizeof(g_trackPath) - 1, "%s\\wow_opt_camera_track.bin", wtf);
    } else {
        _snprintf(g_trackPath, sizeof(g_trackPath) - 1, "%swow_opt_camera_track.bin", dir);
    }
    g_trackPath[sizeof(g_trackPath) - 1] = '\0';
}

bool EnsureTrackStorage() {
    if (g_track) return true;
    g_track = (Sample*)VirtualAlloc(nullptr, sizeof(Sample) * kMaxSamples,
                                    MEM_COMMIT | MEM_RESERVE | MEM_TOP_DOWN,
                                    PAGE_READWRITE);
    if (!g_track) {
        Log("[CameraReplay] could not commit %u KB for the track, error %lu",
            (unsigned)(sizeof(Sample) * kMaxSamples / 1024), GetLastError());
    }
    return g_track != nullptr;
}

bool SaveTrack() {
    HANDLE h = CreateFileA(g_trackPath, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return false;
    const DWORD header[3] = { kFileMagic, kFileVersion, g_trackCount };
    const DWORD bytes = g_trackCount * sizeof(Sample);
    DWORD written = 0;
    bool ok = WriteFile(h, header, sizeof(header), &written, NULL) &&
              written == sizeof(header);
    if (ok) ok = WriteFile(h, g_track, bytes, &written, NULL) && written == bytes;
    CloseHandle(h);
    return ok;
}

// A file that is not exactly what SaveTrack writes, or whose clock runs
// backwards, is refused rather than played.
bool LoadTrack() {
    HANDLE h = CreateFileA(g_trackPath, GENERIC_READ, FILE_SHARE_READ, NULL,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD header[3] = {};
    DWORD got = 0;
    bool ok = ReadFile(h, header, sizeof(header), &got, NULL) && got == sizeof(header) &&
              header[0] == kFileMagic && header[1] == kFileVersion &&
              header[2] >= 2 && header[2] <= kMaxSamples;
    if (ok) {
        const DWORD bytes = header[2] * sizeof(Sample);
        ok = ReadFile(h, g_track, bytes, &got, NULL) && got == bytes;
    }
    CloseHandle(h);
    if (!ok) return false;
    for (DWORD i = 1; i < header[2]; i++) {
        if (!(g_track[i].tMs >= g_track[i - 1].tMs)) return false;
    }
    g_trackCount = header[2];
    return true;
}

void EndPlayback(bool finished, const char* reason) {
    g_mode = Mode::Idle;
    if (finished) {
        ++g_playsFinished;
        Log("[CameraReplay] playback finished: %lu recorded frames over %.1f s, "
            "%u frames written this session. The measurement follows.",
            g_trackCount, g_track[g_trackCount - 1].tMs / 1000.0, g_framesWritten);
    } else {
        ++g_playsCut;
        g_lastCutReason = reason;
        Log("[CameraReplay] playback cut short: %s. The window below covers only the "
            "part that played, so do not compare it with a full run.", reason);
    }
    FrameBench::EndWindow();
}

void StopRecording(const char* why) {
    g_mode = Mode::Idle;
    if (g_trackCount < 2) {
        Log("[CameraReplay] recording stopped (%s) with %lu sample(s), too few to "
            "play back; nothing saved", why, g_trackCount);
        g_trackCount = 0;
        return;
    }
    const bool saved = SaveTrack();
    Log("[CameraReplay] recording stopped (%s): %lu frames over %.1f s, %s %s. "
        "Press the key to play it back from this spot, facing this way.",
        why, g_trackCount, g_track[g_trackCount - 1].tMs / 1000.0,
        saved ? "saved to" : "NOT saved - the file could not be written -",
        g_trackPath);
    g_trackFromFile = false;
}

void Cut(const char* reason) {
    if (g_mode == Mode::Playing) {
        EndPlayback(false, reason);
    } else if (g_mode == Mode::Recording) {
        StopRecording(reason);
    }
}

void StartRecording() {
    void* camera = ActiveCamera();
    if (!camera) {
        Log("[CameraReplay] not recording: there is no world camera yet");
        return;
    }
    if (!EnsureTrackStorage()) return;
    g_trackCount = 0;
    g_modeCamera = camera;
    QueryPerformanceCounter(&g_modeStart);
    g_mode = Mode::Recording;
    ++g_recordings;
    Log("[CameraReplay] recording: move the camera, then press Shift and the key "
        "again to stop. At most %lu frames are kept.", kMaxSamples);
}

void StartPlayback() {
    void* camera = ActiveCamera();
    if (!camera) {
        Log("[CameraReplay] not playing: there is no world camera yet");
        return;
    }
    if (!EnsureTrackStorage()) return;
    if (g_trackCount < 2) {
        if (!LoadTrack()) {
            Log("[CameraReplay] not playing: no track recorded this session and "
                "none readable at %s. Press Shift and the key to record one.",
                g_trackPath);
            return;
        }
        g_trackFromFile = true;
    }
    g_modeCamera = camera;
    g_playCursor = 0;
    ++g_playsStarted;
    char name[64];
    _snprintf(name, sizeof(name) - 1, "camera replay, %.1f s track",
              g_track[g_trackCount - 1].tMs / 1000.0);
    name[sizeof(name) - 1] = '\0';
    FrameBench::BeginWindow(name);
    QueryPerformanceCounter(&g_modeStart);
    g_mode = Mode::Playing;
    Log("[CameraReplay] playing %lu frames over %.1f s%s. Press the key again to "
        "stop early.", g_trackCount, g_track[g_trackCount - 1].tMs / 1000.0,
        g_trackFromFile ? " from the saved file" : "");
}

void RecordSample(uintptr_t camera) {
    if (g_trackCount >= kMaxSamples) {
        StopRecording("the track is full");
        return;
    }
    Sample s;
    s.tMs = (float)ElapsedMs();
    __try {
        s.viewA = *(float*)(camera + kViewA);
        s.yaw   = *(float*)(camera + kYaw);
        s.pitch = *(float*)(camera + kPitch);
        s.fov   = *(float*)(camera + kFov);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        StopRecording("the camera could not be read");
        return;
    }
    g_track[g_trackCount++] = s;
}

void WritePlayback(uintptr_t camera) {
    const double t = ElapsedMs();
    if (t >= (double)g_track[g_trackCount - 1].tMs) {
        EndPlayback(true, nullptr);
        return;
    }
    while (g_playCursor + 2 < g_trackCount && (double)g_track[g_playCursor + 1].tMs <= t) {
        ++g_playCursor;
    }
    const Sample& a = g_track[g_playCursor];
    const Sample& b = g_track[g_playCursor + 1];
    const double span = (double)b.tMs - (double)a.tMs;
    float f = span > 0.0 ? (float)((t - (double)a.tMs) / span) : 0.0f;
    if (f < 0.0f) f = 0.0f;
    if (f > 1.0f) f = 1.0f;

    // Across the point where yaw wraps, interpolating would sweep the long way
    // round for one frame, so it holds the earlier value instead. Both recorded
    // values are the client's own; nothing outside its range is ever written.
    float yaw = a.yaw + (b.yaw - a.yaw) * f;
    const float dyaw = b.yaw - a.yaw;
    if (dyaw > kPi || dyaw < -kPi) {
        yaw = a.yaw;
        ++g_wrapSnaps;
    }
    const float viewA = a.viewA + (b.viewA - a.viewA) * f;
    const float pitch = a.pitch + (b.pitch - a.pitch) * f;
    const float fov   = a.fov + (b.fov - a.fov) * f;

    __try {
        *(float*)(camera + kViewA)       = viewA;
        *(float*)(camera + kViewATarget) = viewA;
        *(float*)(camera + kPitch)       = pitch;
        *(float*)(camera + kPitchTarget) = pitch;
        *(float*)(camera + kYaw)         = yaw;
        *(float*)(camera + kYawTarget)   = yaw;
        *(float*)(camera + kZeroA)       = 0.0f;
        *(float*)(camera + kZeroB)       = 0.0f;
        *(float*)(camera + kFov)         = fov;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        EndPlayback(false, "the camera could not be written");
        return;
    }
    ++g_framesWritten;
}

int __cdecl Hooked_CameraUpdate(int a1, void* camera) {
    if (g_mode == Mode::Idle || !camera) return orig_CameraUpdate(a1, camera);
    if (camera != g_modeCamera) {
        Cut("the world camera changed, which is a reload or a new world");
        return orig_CameraUpdate(a1, camera);
    }
    if (g_mode == Mode::Playing) WritePlayback((uintptr_t)camera);
    const int result = orig_CameraUpdate(a1, camera);
    if (g_mode == Mode::Recording) RecordSample((uintptr_t)camera);
    return result;
}

}  // namespace

void Init() {
    if (!Config::g_settings.OptCameraReplay) return;

    if (!QueryPerformanceFrequency(&g_freq) || g_freq.QuadPart <= 0) {
        Log("[CameraReplay] NOT active: no performance counter");
        return;
    }

    bool prologueOk = false;
    __try {
        const unsigned char* p = (const unsigned char*)kCameraUpdate;
        prologueOk = p[0] == 0x55 && p[1] == 0x8B && p[2] == 0xEC &&
                     p[3] == 0x56 && p[4] == 0x8B && p[5] == 0x75 && p[6] == 0x0C;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        prologueOk = false;
    }
    if (!prologueOk) {
        Log("[CameraReplay] NOT active: the camera update at 0x%08X does not start "
            "with the bytes of build 12340", (unsigned)kCameraUpdate);
        return;
    }

    if (MH_CreateHook((void*)kCameraUpdate, (void*)Hooked_CameraUpdate,
                      (void**)&orig_CameraUpdate) != MH_OK) {
        Log("[CameraReplay] NOT active: the camera update could not be hooked");
        return;
    }
    if (WO_EnableHook((void*)kCameraUpdate) != MH_OK) {
        MH_RemoveHook((void*)kCameraUpdate);
        Log("[CameraReplay] NOT active: the camera update hook could not be enabled");
        return;
    }

    g_key = Config::g_settings.CameraReplayKey;
    ResolveTrackPath();
    g_installed = true;
    Log("[CameraReplay] ACTIVE: stand still, press Shift and virtual key 0x%02X to "
        "start and stop recording the camera, then press 0x%02X alone to play it "
        "back from the same spot while FrameBench measures the playback. Track "
        "file: %s", (unsigned)g_key, (unsigned)g_key, g_trackPath);
}

void OnFrame() {
    if (!g_installed || !g_key) return;

    if (g_mode != Mode::Idle && LuaOpt::IsLoadingMode()) {
        Cut("a loading screen started");
    }

    const bool down = (GetAsyncKeyState(g_key) & 0x8000) != 0;
    const bool pressed = down && !g_keyWasDown;
    g_keyWasDown = down;
    // Two clients started side by side both see a global key state, so only the
    // one whose window is in front acts on it.
    if (!pressed || !OurWindowIsForeground()) return;

    const bool shift = (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
    if (shift) {
        if (g_mode == Mode::Recording) {
            StopRecording("the key was pressed");
        } else {
            if (g_mode == Mode::Playing) EndPlayback(false, "recording was started");
            StartRecording();
        }
    } else {
        if (g_mode == Mode::Playing) {
            EndPlayback(false, "the key was pressed again");
        } else if (g_mode == Mode::Recording) {
            Log("[CameraReplay] still recording - press Shift with the key to stop "
                "before playing back");
        } else {
            StartPlayback();
        }
    }
}

void LogStats() {
    if (!Config::g_settings.OptCameraReplay) return;
    if (!g_installed) {
        Log("[CameraReplay] not installed - the reason is at the top of this log");
        return;
    }
    Log("[CameraReplay] %u recording(s); %u playback(s) started, %u ran to the end, "
        "%u cut short%s%s; %u camera frames written, %u held across a yaw wrap. "
        "Track in memory: %lu frames.",
        g_recordings, g_playsStarted, g_playsFinished, g_playsCut,
        g_lastCutReason ? ", last because " : "",
        g_lastCutReason ? g_lastCutReason : "",
        g_framesWritten, g_wrapSnaps, g_trackCount);
}

}  // namespace CameraReplay
