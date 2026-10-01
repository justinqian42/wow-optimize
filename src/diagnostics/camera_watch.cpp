// ============================================================================
// Why this exists. Two testers on two clients reported the camera zooming in
// to the character by itself, and neither could say when. A key press taken at
// the moment is the usual answer and one of them could not produce it: the log
// of a forty minute session had no press in it, so the log could not place a
// single event, and every conclusion drawn from it was about a time the file
// does not contain.
//
// What the client does. sub_606F90 is the follow camera's per-frame update.
// The camera's distance from the character is the float at +280 (the same slot
// the client's own SetView restores as a saved view value). Further down the
// function sub_605D60 works out how far the camera may be from the character
// without being inside geometry, and when the distance is more than a ninth of
// a yard past that, the function writes the shorter value straight into +280
// and sets bit 0x04000000 of the flags at +152. A wall behind the character
// does exactly that, so a pull-in is not by itself a fault: it is what the
// client does when its collision query says something is in the way. A scroll
// of the mouse wheel does not look like it, because the zoom is animated and
// moves the distance a little each frame.
//
// So this counts pull-ins and splits them by whether the A/B test had its
// replacements on or off in that frame. An effect that belongs to the
// replacements shows as a rate that differs between the two halves; walls show
// as the same rate in both. Neither is a verdict on one event.
//
// Nothing here writes to the client.
// ============================================================================

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <cstdint>

#include "camera_watch.h"
#include "ab_test.h"
#include "flight_recorder.h"
#include "config.h"

extern "C" void Log(const char* fmt, ...);

namespace CameraWatch {
namespace {

const uintptr_t kWorldFrame     = 0x00B7436C;
const uintptr_t kWorldCameraOff = 0x7E20;
const uintptr_t kDistance       = 280;     // float: distance behind the character
const uintptr_t kFlags          = 152;     // dword; 0x04000000 is set on a pull-in

// One frame's drop that is not a wheel zoom. A wheel notch moves the distance
// well under a yard a frame at any frame rate this client reaches.
constexpr float kMinDrop      = 1.5f;
constexpr float kMinBefore    = 2.5f;      // nothing to pull in from at point blank
constexpr float kFarEnough    = 0.9f;      // recovered when back within this share
constexpr int   kMaxLogged    = 40;
constexpr int   kMaxMarked    = 12;

float    g_prev       = -1.0f;
bool     g_inEvent    = false;
float    g_eventFrom  = 0.0f;
DWORD    g_eventStart = 0;

unsigned long g_frames[2]   = {};          // [0] replacements off, [1] on or no test
unsigned long g_events[2]   = {};
unsigned long g_unread      = 0;
unsigned long g_logged      = 0;
float         g_worstDrop   = 0.0f;
DWORD         g_longestMs   = 0;

bool ReadCamera(float* dist, unsigned* flags) {
    __try {
        const uintptr_t wf = *(const uintptr_t*)kWorldFrame;
        if (wf < 0x10000 || wf > 0xFFE00000) return false;
        const uintptr_t cam = *(const uintptr_t*)(wf + kWorldCameraOff);
        if (cam < 0x10000 || cam > 0xFFE00000) return false;
        *dist  = *(const float*)(cam + kDistance);
        *flags = *(const unsigned*)(cam + kFlags);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

}  // namespace

void OnFrame() {
    if (!Config::g_settings.OptFlightRecorder) return;

    float d = 0.0f;
    unsigned flags = 0;
    if (!ReadCamera(&d, &flags) || !(d == d) || d < 0.0f || d > 1000.0f) {
        ++g_unread;
        g_prev = -1.0f;
        g_inEvent = false;
        return;
    }

    const int half = (AbTest::Running() && !AbTest::FeatureOn()) ? 0 : 1;
    ++g_frames[half];

    if (g_inEvent) {
        if (d >= g_eventFrom * kFarEnough) {
            const DWORD ms = GetTickCount() - g_eventStart;
            if (ms > g_longestMs) g_longestMs = ms;
            if (g_logged <= (unsigned long)kMaxLogged)
                Log("[CameraWatch]   back to %.1f yd after %lu ms", d, (unsigned long)ms);
            g_inEvent = false;
        }
    } else if (g_prev >= kMinBefore && g_prev - d >= kMinDrop) {
        ++g_events[half];
        if (g_prev - d > g_worstDrop) g_worstDrop = g_prev - d;
        g_inEvent    = true;
        g_eventFrom  = g_prev;
        g_eventStart = GetTickCount();
        if (g_logged < (unsigned long)kMaxLogged) {
            ++g_logged;
            Log("[CameraWatch] camera pulled in from %.1f to %.1f yd in one frame "
                "(flags %08X, %s)", g_prev, d, flags,
                !AbTest::Running() ? "no A/B test running"
                                   : half ? "replacements ON" : "replacements OFF");
            if (g_logged <= (unsigned long)kMaxMarked)
                FlightRecorder::Mark("camera pulled in");
        }
    }
    g_prev = d;
}

void LogStats() {
    if (!Config::g_settings.OptFlightRecorder) return;
    const unsigned long total = g_frames[0] + g_frames[1];
    if (!total) {
        Log("[CameraWatch] not measured: no frame saw a readable camera (%lu unreadable).",
            g_unread);
        return;
    }
    Log("[CameraWatch] %lu pull-in(s) of %.1f yd or more in a single frame: %lu with the "
        "replacements ON over %lu frames, %lu with them OFF over %lu frames. Worst drop "
        "%.1f yd, longest %lu ms. %lu frame(s) could not read the camera.",
        g_events[0] + g_events[1], (double)kMinDrop, g_events[1], g_frames[1],
        g_events[0], g_frames[0], (double)g_worstDrop, (unsigned long)g_longestMs,
        g_unread);
    if (g_frames[0] && g_frames[1]) {
        Log("[CameraWatch]   per 100000 frames: ON %.1f, OFF %.1f. Walls give the same "
            "rate in both halves; a rate that differs is what to follow. A handful of "
            "events is too few to say either way.",
            100000.0 * g_events[1] / (double)g_frames[1],
            100000.0 * g_events[0] / (double)g_frames[0]);
    } else {
        Log("[CameraWatch]   no A/B test split this session, so the count above is every "
            "pull-in and cannot be told from walls.");
    }
}

}  // namespace CameraWatch
