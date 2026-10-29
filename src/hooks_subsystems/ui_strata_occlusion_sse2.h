#pragma once

// ============================================================================
// Module: ui_strata_occlusion_sse2.h
//
// High-performance vectorized AABB occlusion culling for CFrameStrata::CheckOcclusion
// (sub_494E60) and CFrameStrata::FrameOccluded (sub_494D20).
//
// In stock WoW 3.3.5a build 12340, whenever UI frames move, show, or change level,
// CFrameStrata::CheckOcclusion performs an O(N^2) scan traversing intrusive frame
// lists, executing thousands of virtual CLayoutFrame::GetRect calls, x87 FPU stack
// status transfers (fnstsw ax), and parent pointer chases.
//
// This replacement flattens active strata frames into a compact cache-coherent
// array, extracts valid layout bounding boxes in a single pass without virtual
// dispatch, and evaluates 2D AABB intersections using vectorized SSE2 movemasks.
// ============================================================================

namespace UIStrataOcclusionFast {

bool Init();
void LogStats();
void Shutdown();

} // namespace UIStrataOcclusionFast
