#pragma once
#include <windows.h>

namespace SpellEffectCulling {
    bool Init();
    void Shutdown();
    void OnFrame();
    void LogStats();
}
