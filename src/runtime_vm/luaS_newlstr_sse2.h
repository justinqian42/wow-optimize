#pragma once

namespace LuaSNewlstr {
    bool Init();
    void Shutdown();
    // Printed from the periodic report. Everything here used to be printed from
    // Shutdown alone, which this DLL never reaches.
    void LogStats();
}
