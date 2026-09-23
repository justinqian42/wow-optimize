// ============================================================================
// Module: collision_face_clip_sse2.h
// Description: Accelerates collision mesh face query and polygon clipping
//              against bounding planes in sub_75C5A0 (0x14F bytes).
// ============================================================================

#ifndef WOW_OPT_COLLISION_FACE_CLIP_SSE2_H
#define WOW_OPT_COLLISION_FACE_CLIP_SSE2_H

namespace CollisionFaceClip {

bool Init();
void Shutdown();
void LogStats();

}  // namespace CollisionFaceClip

#endif  // WOW_OPT_COLLISION_FACE_CLIP_SSE2_H
