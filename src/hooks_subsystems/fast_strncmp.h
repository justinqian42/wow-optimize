#pragma once

#include <cstdint>

bool InstallFastStrncmp();
void UninstallFastStrncmp();
void GetFastStrncmpStats(uint64_t* calls);
