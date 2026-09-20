#pragma once

// The shader constant shadow compare, sub_6833E0. See shader_const_dedup_sse2.cpp.
namespace ShaderConstDedup {
bool Init();
void Shutdown();
void LogStats();
}
