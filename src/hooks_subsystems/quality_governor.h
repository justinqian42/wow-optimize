#pragma once

// ============================================================================
// Description: Lowers a graphics setting when frames are consistently slow, and
//              puts the player's own value back when they are not.
// Safety & Threading: Main thread only. Observes CVar writes from the CVar_Set
//              detour, which also runs on the main thread.
// ============================================================================

namespace QualityGovernor {

bool Init();
void Shutdown();

// From the periodic report. Shutdown does not run in this client, so the only
// thing this module ever said about itself was the line for each change it
// made - and nothing at all in a session where it made none.
void LogStats();

// Called once per frame boundary. Decides nothing most of the time: it is bounded
// by a dwell period and only acts on a sustained condition.
void OnFrame();

// Every CVar write the client makes passes through here, from the existing
// CVar_Set detour. This is how the governor learns what the player chose - the
// previous attempt at this feature had no way to read a CVar, so it overwrote the
// player's setting and could never put it back. Writes the governor makes itself
// are ignored, or it would mistake its own reduction for a new preference.
void NoteCVarWrite(const char* name, const char* value);

// The CVar object itself, so the governor has something to write to later. It
// cannot look one up; it only ever learns about the object by watching a write
// go past.
void NoteCVarObject(void* cvar, const char* name);

} // namespace QualityGovernor
