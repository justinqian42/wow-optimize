#pragma once

// Detects DXVK / vk9 / dgVoodoo2 (Vulkan translation of D3D9). Other
// modules consult IsActive() to skip work the Vulkan driver already does.

#include <windows.h>
#include <cstdint>

namespace DXVKBridge {

bool Init();
void Shutdown();

bool IsActive();
struct Stats {
    bool        active;
    const char* detectionReason;
};
void GetStats(Stats* out);

} // namespace DXVKBridge
