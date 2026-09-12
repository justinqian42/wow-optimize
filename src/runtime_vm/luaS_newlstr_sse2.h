#pragma once

// ============================================================================
// Module: luaS_newlstr_sse2.h
// Description: Accelerates Lua runtime calls in `luaS_newlstr_sse2.h`.
// Safety & Threading: Thread-safe under Lua VM execution constraints.
// ============================================================================










namespace LuaSNewlstr {
    bool Init();
    void Shutdown();
    // Printed from the periodic report. Everything here used to be printed from
    // Shutdown alone, which this DLL never reaches.
    void LogStats();
}
