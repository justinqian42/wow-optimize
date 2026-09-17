#pragma once

#pragma region System Dependencies
#include <stdint.h>
#pragma endregion

#pragma region API Control Interfaces

void InitNetworkGuidSSE2();


uint32_t FastUnpackGuidSSE2(const uint8_t* buffer, uint32_t remaining, uint64_t* out_guid);


bool InstallNetworkGuidSSE2Hooks();
#pragma endregion
