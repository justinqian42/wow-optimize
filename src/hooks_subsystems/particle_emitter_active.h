#pragma once

// ============================================================================
// Module: particle_emitter_active.h
//
// Accelerates particle emitter hierarchy activity detection in sub_97B9E0
// (sampled up to 8,803 times per snapshot, ~1.0-1.5% of frame time during
// intense particle simulation and M2 model updates).
//
// Inlines the active particle check and flattens recursive emitter tree
// traversal into a bounded iterative depth-first search with zero function
// call overhead and zero stack cookies on the hot path.
// ============================================================================

namespace ParticleEmitterActive {
    bool Init();
    void Shutdown();
    void LogStats();
}
