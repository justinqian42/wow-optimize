#pragma once

// Notices the client pulling the world camera in towards the character, and
// says whether the A/B test had the replacements on or off at the time. See
// camera_watch.cpp.
namespace CameraWatch {

// Hooks the client's world trace when the A/B test is on, to re-run a doubtful
// camera result with replacements standing aside. See camera_watch.cpp.
void Init();

// Called from the presented-frame boundary, after the camera has been updated.
void OnFrame();

void LogStats();

}  // namespace CameraWatch
