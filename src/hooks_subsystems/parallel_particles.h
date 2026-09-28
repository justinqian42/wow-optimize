#pragma once

// Particle vertex fill split across worker threads. See parallel_particles.cpp.
namespace ParallelParticles {

bool Init();
void Shutdown();
void LogStats();

// True on one of this module's worker threads. A hook that client code can
// reach from the particle fill, and that keeps state which is only safe on the
// main thread, asks this before touching it.
bool OnWorkerThread();

}  // namespace ParallelParticles
