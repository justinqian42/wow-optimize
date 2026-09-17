#pragma once

#include <cstdint>

// index2adr (sub_84D9C0) resolves a Lua stack/pseudo index to its TValue*.
// It is a __usercall leaf: the index arrives in EAX, the lua_State* in ECX, and
// the result is returned in EAX. MSVC cannot express __usercall, so this naked
// thunk marshals an ordinary __cdecl(idx, L) call into the EAX/ECX the engine
// reads. Invoking 0x84D9C0 through a plain __cdecl function pointer (which passes
// both args on the stack) leaves EAX/ECX holding unrelated garbage, so the engine
// dereferences a bogus pointer and faults at index2adr+4 (mov edx,[ecx+0x10]).
extern "C" uintptr_t __cdecl WowIndex2Adr(int idx, uintptr_t L);
