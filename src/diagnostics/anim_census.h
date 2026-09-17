#pragma once

// ============================================================================
// Counts what the M2 animation update actually does per frame: how many models
// it walks, how many bones those carry, and how long it takes. A measurement,
// not an optimisation - it exists to decide whether animation level-of-detail
// is worth its risk, which nothing in this project can currently answer.
// ============================================================================

#ifndef ANIM_CENSUS_H
#define ANIM_CENSUS_H

namespace AnimCensus {

bool Init();
void Shutdown();

// Called once per frame from the main-thread maintenance tick.
void OnFrame();

// Printed from the periodic report.
void LogStats();

} // namespace AnimCensus

#endif // ANIM_CENSUS_H
