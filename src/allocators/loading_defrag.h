#pragma once

#ifndef LOADING_DEFRAG_H
#define LOADING_DEFRAG_H

#include <windows.h>
#include <atomic>

namespace LoadingDefrag {

// The raw flag behind IsLoadingActive(), exposed so a hot path can read it
// without a cross-module call. IsLoadingActive() additionally expires it after
// a watchdog interval; a direct read therefore reports "loading" for longer in
// the pathological case where the end event is lost, which leaves fast paths
// deferring to the client. That is the safe direction, and it is the only
// difference between the two.
extern std::atomic<bool> g_loadingActive;

// Initialize the loading defragmenter background thread and data structures
bool Init();

// Shutdown the background thread and clean up resources
void Shutdown();

// Notify the module about changes in the game's loading screen state
void NotifyLoadingState(bool isLoading);

// Check if the loading screen is currently active
bool IsLoadingActive();

// Call every frame from the main thread (e.g. to update zone text)
void OnFrame();

} // namespace LoadingDefrag

#endif // LOADING_DEFRAG_H
