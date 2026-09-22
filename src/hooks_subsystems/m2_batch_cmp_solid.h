#pragma once

// Solid/opaque M2 batch sort comparator (sub_81EAD0, 0x233 bytes).
// Defers expensive 3-hop pointer chains (rec -> +4 -> +2Ch -> +150h) until
// after scalar record fields have been compared.
// Pure comparison function returning -1, 0, 1.

namespace M2BatchCmpSolid {

bool Init();
void Shutdown();
void LogStats();

}  // namespace M2BatchCmpSolid
