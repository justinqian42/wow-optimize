#pragma once

// Inline replacements for trivially-inlinable Lua C-API push/query
// functions. Each is verified against the stock 3.3.5a binary disassembly.
//
//   lua_pushnil           (0x84E280, 31B) — top[0..3]=0, taint, advance
//   lua_pushinteger        (0x84E2D0, 36B) — (double)n → top, advance
//   lua_pushboolean        (0x84E4D0, 41B) — int(bool) → top, advance
//   lua_pushlightuserdata  (0x84E500, 36B) — ptr → top, tt=2, advance
//   lua_type               (0x84DEB0, 31B) — resolve idx → tt
//   lua_isfunction          (0x84DEF0, 27B) — resolve idx → tt==6
//   lua_isstring           (0x84DF60, 45B) — resolve idx → tt==4||tt==3
//   lua_tothread           (0x84E1F0, 28B) — resolve idx → tt==8?val:0
//
// All hooks use SEH guards + teardown-state detection + disable flag.

bool InstallLuaStackFast();
void ShutdownLuaStackFast();