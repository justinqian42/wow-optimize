#pragma once

// ============================================================================
// SSE2 replacement for sub_78F6A0, the terrain horizon occlusion builder -
// 2.46% of main-thread execution in a tester's profile.
// ============================================================================

#ifndef HORIZON_OCCLUSION_SSE2_H
#define HORIZON_OCCLUSION_SSE2_H

namespace HorizonOcclusion {

bool Init();
void Shutdown();
void LogStats();

} // namespace HorizonOcclusion

#endif // HORIZON_OCCLUSION_SSE2_H
