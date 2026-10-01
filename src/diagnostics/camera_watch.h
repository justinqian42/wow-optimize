#pragma once

// Notices the client pulling the world camera in towards the character, and
// says whether the A/B test had the replacements on or off at the time. See
// camera_watch.cpp.
namespace CameraWatch {

// Called from the presented-frame boundary, after the camera has been updated.
void OnFrame();

void LogStats();

}  // namespace CameraWatch
