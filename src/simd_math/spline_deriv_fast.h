#pragma once

// ============================================================================
// Module: spline_deriv_fast.h
//
// Fast spline curve first-derivative (tangent/velocity) evaluation
// (sub_4C3A70, 146 bytes).
// In WoW 3.3.5a, sub_4C3A70 evaluates cubic spline velocity and tangent vectors
// across unit movement, projectiles, flight paths, and camera tracking.
// ============================================================================

namespace SplineDerivFast {

void Init();
void Shutdown();
void LogStats();

}  // namespace SplineDerivFast

