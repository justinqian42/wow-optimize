#pragma once

// The per-particle track evaluation, sub_979D60. See particle_track_eval_sse2.cpp.
namespace ParticleTrackEval {
bool Init();
void Shutdown();
void LogStats();
}
