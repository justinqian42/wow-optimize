#pragma once

// Records the world camera's view and plays it back under a FrameBench window,
// so two builds or two settings are measured over the same camera motion. See
// camera_replay.cpp.
namespace CameraReplay {

// Installs the camera update hook when CameraReplay is on.
void Init();

// Polls the key and ends a recording or playback that can no longer continue.
// Called from the presented-frame boundary.
void OnFrame();

void LogStats();

}  // namespace CameraReplay
