#pragma once

// The texel loops of the cloud texture build, sub_7EFD00. See
// sky_cloud_texels.cpp.
//
// The hook that reaches the build lives in sky_texture_reuse.cpp, because two
// modules cannot hook one address. Before and After bracket the client's call
// from there; Wanted says whether this session still has passes to compare, so
// that module can skip the snapshot when it does not.
namespace SkyCloudTexels {
bool Init();
void Shutdown();
void LogStats();

bool Wanted();
void Before(void* self);
void After(void* self);
}
