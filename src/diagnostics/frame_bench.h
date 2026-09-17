#pragma once

// ============================================================================
// Description: Frame-time distribution benchmark - the instrument that lets one
//              build be compared against another.
//
// This project has around fifty optimization toggles and no way to tell whether
// any of them helps. Every feature has been justified by theory; the README's
// "Performance Metrics" section contains no numbers. Two of the largest findings
// this month were cases where our own code made the game slower, and both were
// found by measurement rather than review.
//
// So: record every presented frame, and report the distribution in a form two
// runs can be diffed on. Percentiles, not an average - an average hides exactly
// the stutters players complain about. Loading screens are excluded, because a
// single zone load would dominate the tail and make runs incomparable.
//
// The config fingerprint in the report is what makes an A/B honest: it is a hash
// of the whole settings block, so a log can be checked to have actually run the
// configuration it claims to.
// ============================================================================

namespace FrameBench {

// Where frame boundaries are being taken from. Only ever one source per session -
// mixing a true present hook with a coarser tick would make runs incomparable, so
// the source is named in the report.
enum class Source {
    None,
    D3D9Present,     // IDirect3DDevice9::Present - the boundary for D3D9 clients
    SwapHook,        // sub_69E220 - the client's OpenGL present path
};

void Init();

// Called once per presented frame. Cost is one QueryPerformanceCounter and one
// histogram increment.
void OnPresent(Source src);

// Writes the distribution to the log. Safe to call repeatedly; each call reports
// the whole session so far.
void Report(const char* reason);

// Dumps the flight recorder if the frame just measured was slow enough to mark.
// Separate from OnPresent because the recorder is fed later in the same frame
// boundary: marking from inside OnPresent dumps a ring whose newest entry is the
// frame before the slow one, which is the frame nobody asked about.
void FlushAutoMark();

// 95th percentile frame time over the last few seconds, or 0 before enough frames
// have been seen.
//
// Anything that reacts to how the game is running needs this rather than an
// instantaneous frame rate. The scalers in this project each smooth 1000/elapsed
// with their own EMA and switch on thresholds crossed by a single frame, which is
// how one of them ended up changing shadow quality several times a minute while a
// player walked around. What players notice is the tail, and the tail is what this
// returns.
double RecentP95Ms();

// Throws away the recent window. For a caller that has just been told the
// frames in it were not gameplay - the far side of a loading screen - so that
// what it reads next is only frames from after the boundary.
void ResetRecent();

// Whether the recent window is full again. A caller that acts on the tail has
// to wait for this after a reset, or it judges the first seconds after a load
// against a handful of samples.
bool RecentWindowFull();

// Smoothed frame time in milliseconds, or 0.0 before any frame has been measured.
// Constant time, unlike RecentP95Ms, so it is safe to consult on a per-frame or
// per-Sleep path.
double SmoothedFrameMs();

// The running median frame time, refreshed from the histogram every few hundred
// frames. Read by the sampling profiler to notice a frame-rate cap: a median
// sitting on a display interval means its verdict is describing a wait.
double MedianMs();

// The session's 95th percentile, computed in the same walk as the median so the
// two can be compared. A frame-rate cap holds every frame at the interval, so a
// capped session has these two almost equal; a client that is merely slow at the
// same median has a long tail. Reading the median alone is what made the profiler
// call an uncapped session capped.
double SessionP95Ms();

// A measured stretch inside the session, for a benchmark run. Every frame from
// BeginWindow to EndWindow is reported as its own distribution under `name`,
// without touching the session figures or the periodic report's interval. The
// same exclusions apply: loading screens and gaps are left out and counted.
void BeginWindow(const char* name);
// Logs the window and closes it. False when no window was open.
bool EndWindow();
bool WindowOpen();

} // namespace FrameBench
