#pragma once

#ifndef WOW_SUBSYSTEM_HOOKS_H
#define WOW_SUBSYSTEM_HOOKS_H

namespace WowSubsystemHooks {
    bool InstallAll();
    void ShutdownAll();
    void DumpStats();
}

#endif
