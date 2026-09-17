#pragma once

#include <cstdint>

bool InstallRegexCache();
void ShutdownRegexCache();
void RegexCache_Clear();

const uint8_t* RegexCache_Get(const char* pattern, int patternLen, unsigned int options, const unsigned char* tableptr, int* outCompiledLen);
void RegexCache_Put(const char* pattern, int patternLen, unsigned int options, const unsigned char* tableptr, const uint8_t* compiled, int compiledLen);
void RegexCache_LogStats(void);
