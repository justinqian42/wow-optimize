#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
extern "C" void Log(const char* fmt, ...);

bool InstallIOCache() {
    Log("[IOCache] Disabled: dispatcher has writes/critical sections");
    return true;
}
void UninstallIOCache() {}
