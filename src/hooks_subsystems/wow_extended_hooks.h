#pragma once

#ifndef WOW_EXTENDED_HOOKS_H
#define WOW_EXTENDED_HOOKS_H

namespace WowExtendedHooks {
    bool InstallAll();
    void ShutdownAll();
    void DumpStats();
}

#endif
