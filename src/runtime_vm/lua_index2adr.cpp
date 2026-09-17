// ============================================================================
// Description: Out-of-line definition for WowIndex2Adr to prevent MSVC
//              calling convention promotions (Whole Program Optimization).
// Safety & Threading: Thread-safe.
// ============================================================================

#include "lua_index2adr.h"

// Define as extern "C" __declspec(noinline) so the compiler is forced to use
// the standard __cdecl stack-based calling convention when calling it from
// any .cpp file. Whole Program Optimization cannot change this.
extern "C" __declspec(noinline) __declspec(naked) uintptr_t __cdecl WowIndex2Adr(int idx, uintptr_t L) {
    __asm {
        mov eax, [esp + 4]   // idx
        mov ecx, [esp + 8]   // L
        mov edx, 0x0084D9C0  // index2adr target address
        call edx
        ret
    }
}
