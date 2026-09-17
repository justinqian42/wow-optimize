#pragma once

#ifndef WOW_OPT_HOOKS_H
#define WOW_OPT_HOOKS_H

namespace WowOptHooks {
    bool InstallAll();
    void ShutdownAll();
    void DumpStats();
}

#endif