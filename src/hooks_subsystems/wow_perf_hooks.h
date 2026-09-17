#pragma once

#ifndef WOW_PERF_HOOKS_H
#define WOW_PERF_HOOKS_H

namespace WowPerfHooks {
    bool InstallAll();
    void ShutdownAll();
    void DumpStats();
}

#endif