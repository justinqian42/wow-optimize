// ============================================================================
// Description: lua_setlocal fast path - PERMANENTLY DISABLED
// Safety & Threading: N/A - feature disabled due to unknown target address
// ============================================================================

#include <windows.h>
#include <cstdint>
#include "MinHook.h"
#include "version.h"
#include "crash_dumper.h"

extern "C" void Log(const char* fmt, ...);

// ============================================================================
// PERMANENTLY DISABLED - DO NOT RE-ENABLE
//
// ROOT CAUSE ANALYSIS (IDA Pro verified):
// - 0x84F210 is luaL_where (debug location formatter), NOT lua_setlocal
//   Confirmed: 2-arg __cdecl(L, flag) calling lua_getstack+lua_getinfo("Sl")
//   formatting "%s:%d: ". Hooking it as 3-arg lua_setlocal corrupted stack.
//
// - Real lua_getlocal is at 0x84FF30 (3-arg, confirmed working)
// - Real lua_setlocal address is UNKNOWN after exhaustive IDA search:
//   * Checked 0x84FE80-0x850C00 range (all debug helpers, taint checks, getinfo)
//   * No function matches lua_setlocal's expected signature (L, ar, n) with
//     write-to-stack semantics
//   * May be inlined into the debug library dispatcher or use __usercall
//     convention that IDA couldn't auto-detect
//
// IMPACT: lua_setlocal is rarely called by addons (only debug libraries like
// DebugLib use it). The performance impact of not optimizing it is negligible.
// ============================================================================

bool InstallLuaSetLocalFast() {
    Log("[SetLocal] PERMANENTLY DISABLED: real lua_setlocal address unknown");
    Log("[SetLocal] 0x84F210 = luaL_where (confirmed via IDA), NOT lua_setlocal");
    CrashDumper::RegisterFeature("SetLocal");
    CrashDumper::FeatureSetActive("SetLocal", false);
    return false;
}