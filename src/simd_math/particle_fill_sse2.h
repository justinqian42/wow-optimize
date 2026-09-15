#pragma once

// The per-particle vertex fill in the client's particle emitter, sub_6C4440.
// See particle_fill_sse2.cpp.
namespace ParticleFill {
bool Init();
void Shutdown();
void LogStats();
}
