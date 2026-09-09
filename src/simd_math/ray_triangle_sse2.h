// ============================================================================
// Module: ray_triangle_sse2.h
// Description: SSE2 double transcription of the client's ray-triangle test.
// Safety & Threading: Main thread, same as the function it replaces.
// ============================================================================

#pragma once

namespace RayTriangle {

bool Init();
void Shutdown();
void LogStats();

} // namespace RayTriangle
