#pragma once

// Mid-function billboard quad vertex generation in sub_97BE80 (0x0097C7B3).
// Replaces 365 instructions of serialized x87 trigonometry and quad vertex
// generation with bit-exact double evaluation and branchless AABB updates.
namespace ParticleQuad {
bool Init();
void Shutdown();
void LogStats();
}
