// ============================================================================
// Description: Rebuilds a Proto from the bytes the client's own lua_dump wrote.
// ============================================================================
// The client dumps bytecode and cannot load it. Stock Lua 5.1 f_parser does a
// luaZ_lookahead and picks luaU_undump or luaY_parser by the LUA_SIGNATURE
// byte; this client's f_parser (0x00856190) has neither the lookahead nor the
// call, and goes straight to luaY_parser. lundump.c is not in the build. So a
// dumped chunk has nothing to load it, and the proto cache beside this file
// keeps live Protos instead - which works within a session and cannot survive
// the process.
//
// This is the missing direction. It reads the client's own dump format and
// builds the Proto with the client's own allocator and GC linkage, so what
// comes out is a Proto the client made, holding strings from the client's own
// intern table.
// ---------------------------------------------------------------------------
// Every client entry point this calls, and how each was established
//
//   luaF_newproto   0x0085CF40   Proto* (L)
//       Found from open_func (0x0085F410), which calls it and then writes
//       ls->source into +36 and 2 into +79. It allocates 80 bytes - stock Lua
//       5.1's Proto is 76 on x86, and this client is +4-shifted throughout -
//       and zeroes all twenty-one fields.
//   luaM_realloc_   0x0085D6F0   void* (L, block, osize, nsize)
//       Read at the address: it calls G(L)->frealloc(G(L)->ud, block, osize,
//       nsize) through G+12/G+16, throws LUA_ERRMEM on a null result with a
//       non-zero size, and adjusts totalbytes at G+68. It does not step the GC.
//   luaC_link       0x0085BAB0   void (L, obj, tt)
//       Called by luaF_newproto with tt 9, which is LUA_TPROTO.
//   luaS_newlstr    0x00856C80   TString* (L, s, len)
//       Already used elsewhere in this project; confirmed again at 0x00861B01
//       where luaY_parser interns the chunk name.
//
// The taint word is the fifth thing and it is not a function. Every TValue
// store in this client stamps +12 from *(DWORD*)dword_D4139C - lua_pushnumber
// at 0x0084E2A0 is four instructions and does exactly that, and f_parser does
// the same for the closure it pushes. A constant built here is stamped the same
// way, so it carries the taint current at the moment the chunk is handed over,
// which is precisely what a parse running at that moment would have written.
// ---------------------------------------------------------------------------
// The format, read off the writer rather than off stock Lua
//
//   DumpString    0x0085D1D0   size_t len+1, then len+1 bytes including the
//                              NUL. A null string writes a zero size.
//   DumpFunction  0x0085D500   source (null when it equals the parent's, which
//                              is the `cmp eax, [ebp+arg_4]` at 0x0085D50F),
//                              linedefined +64, lastlinedefined +68, nups +76,
//                              numparams +77, is_vararg +78, maxstacksize +79,
//                              then sizecode +48 and sizecode*4 bytes of code.
//   DumpConstants 0x0085D260   sizek +44, then per constant a tag byte and its
//                              payload: nothing for nil, one byte for a
//                              boolean, eight for a number, a string otherwise.
//                              Then sizep +56 and each nested function.
//   DumpDebug     0x0085D390   sizelineinfo +52 and that many ints; sizelocvars
//                              +60 and per entry a name, a startpc and an
//                              endpc twelve bytes apart; sizeupvalues +40 and
//                              that many names.
// ---------------------------------------------------------------------------
// Two passes, because a half-built Proto is not recoverable
//
// Load walks the whole buffer once without allocating anything, checking every
// length against the bytes actually remaining and against a ceiling. Only a
// buffer that survives that walk is built. A truncated or corrupt file is
// therefore refused before the first allocation, and the caller compiles from
// source as if nothing had been offered.
//
// The one failure the second pass can still hit is the allocator throwing
// LUA_ERRMEM, which longjmps clear past this code to the protected call above.
// That is out of memory, it is already fatal for the compile that asked, and no
// C++ object with a destructor is alive across any client call here.
// ---------------------------------------------------------------------------
// Sizes are published as the array fills, never before
//
// A Proto is GC-linked from the moment luaF_newproto returns, so traverseproto
// can in principle walk it while it is still being filled. It null-checks
// source, upvalues, nested protos and locvar names, but it marks every one of
// the sizek constants unconditionally. So the constant array is zeroed - tag 0
// is nil - before sizek is published, and the arrays whose contents are marked
// pointers publish their size one element at a time as each element lands.
//
// Nothing on the path from luaF_newproto to the finished Proto steps the GC, so
// none of this can currently be exercised. It costs nothing and it means the
// object is consistent at every instant rather than only at the end.
// ============================================================================

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <cstdint>
#include <cstring>
#include <cstdio>

#include "lua_undump.h"
#include "version.h"

extern "C" void Log(const char* fmt, ...);

namespace LuaUndump {

namespace {

// --- The client -------------------------------------------------------------

constexpr uintptr_t kLuaFNewProto  = 0x0085CF40;
constexpr uintptr_t kLuaMRealloc   = 0x0085D6F0;
constexpr uintptr_t kLuaSNewLStr   = 0x00856C80;
constexpr uintptr_t kTaintCurrent  = 0x00D4139C;  // holds a pointer to it

typedef void* (__cdecl* luaF_newproto_fn)(void* L);
typedef void* (__cdecl* luaM_realloc_fn)(void* L, void* block, size_t osize, size_t nsize);
typedef void* (__cdecl* luaS_newlstr_fn)(void* L, const char* s, size_t len);

luaF_newproto_fn p_luaF_newproto = (luaF_newproto_fn)kLuaFNewProto;
luaM_realloc_fn  p_luaM_realloc  = (luaM_realloc_fn) kLuaMRealloc;
luaS_newlstr_fn  p_luaS_newlstr  = (luaS_newlstr_fn) kLuaSNewLStr;

// Proto, +4-shifted like everything else in this client. luaF_newproto zeroes
// exactly these and nothing else, which is how the set was checked.
constexpr unsigned kP_k               = 12;
constexpr unsigned kP_code            = 16;
constexpr unsigned kP_p               = 20;
constexpr unsigned kP_lineinfo        = 24;
constexpr unsigned kP_locvars         = 28;
constexpr unsigned kP_upvalues        = 32;
constexpr unsigned kP_source          = 36;
constexpr unsigned kP_sizeupvalues    = 40;
constexpr unsigned kP_sizek           = 44;
constexpr unsigned kP_sizecode        = 48;
constexpr unsigned kP_sizelineinfo    = 52;
constexpr unsigned kP_sizep           = 56;
constexpr unsigned kP_sizelocvars     = 60;
constexpr unsigned kP_linedefined     = 64;
constexpr unsigned kP_lastlinedefined = 68;
constexpr unsigned kP_nups            = 76;
constexpr unsigned kP_numparams       = 77;
constexpr unsigned kP_isvararg        = 78;
constexpr unsigned kP_maxstacksize    = 79;

// TValue: the value at 0, the tag at 8, the taint at 12. Stride 16.
constexpr unsigned kTV_tt    = 8;
constexpr unsigned kTV_taint = 12;
constexpr unsigned kTV_size  = 16;

// LocVar: a TString* and two ints.
constexpr unsigned kLV_startpc = 4;
constexpr unsigned kLV_endpc   = 8;
constexpr unsigned kLV_size    = 12;

// lua_State: top at 0x0C. LClosure: isC at +10, p at +24. The proto cache reads
// the same two objects at the same offsets.
constexpr unsigned kL_top       = 0x0C;
constexpr unsigned kC_isC       = 10;
constexpr unsigned kC_p         = 24;
constexpr uint32_t kTagFunction = 6;

constexpr uint8_t kTagNil     = 0;
constexpr uint8_t kTagBoolean = 1;
constexpr uint8_t kTagNumber  = 3;
constexpr uint8_t kTagString  = 4;

// --- Ceilings ---------------------------------------------------------------
//
// A length in the file is a length this code is about to allocate, so each one
// is checked against the bytes actually left and against a number no chunk this
// client compiles can reach. The largest thing WoW compiles is GlobalStrings.lua
// at just under a megabyte of source; these sit orders of magnitude above what
// that produces, and their job is to turn a corrupt file into a refusal instead
// of a multi-gigabyte allocation.
constexpr uint32_t kMaxCode      = 1u << 22;
constexpr uint32_t kMaxConstants = 1u << 20;
constexpr uint32_t kMaxNested    = 1u << 16;
constexpr uint32_t kMaxLineInfo  = 1u << 22;
constexpr uint32_t kMaxLocVars   = 1u << 20;
constexpr uint32_t kMaxUpvalues  = 255;
constexpr uint32_t kMaxStringLen = 1u << 24;
constexpr int      kMaxDepth     = 200;

// --- State ------------------------------------------------------------------

bool          g_dead     = false;
bool          g_probed   = false;
bool          g_usable   = false;
unsigned long g_loaded   = 0;
unsigned long g_refused  = 0;
unsigned long g_bytesIn  = 0;

inline uint32_t RD32(const void* p, unsigned off) { return *(const uint32_t*)((const char*)p + off); }
inline uint8_t  RD8 (const void* p, unsigned off) { return *(const uint8_t*) ((const char*)p + off); }
inline void*    RDP (const void* p, unsigned off) { return *(void* const*)   ((const char*)p + off); }
inline void WR32(void* p, unsigned off, uint32_t v) { *(uint32_t*)((char*)p + off) = v; }
inline void WR8 (void* p, unsigned off, uint8_t  v) { *(uint8_t*) ((char*)p + off) = v; }
inline void WRP (void* p, unsigned off, void*    v) { *(void**)   ((char*)p + off) = v; }

uint32_t CurrentTaint() {
    void* slot = *(void* const*)kTaintCurrent;
    if (!slot) return 0;
    return *(const uint32_t*)slot;
}

// --- Reading ----------------------------------------------------------------

struct Reader {
    const unsigned char* p;
    const unsigned char* end;
    bool                 bad;
};

inline bool Have(Reader& r, size_t n) {
    if (r.bad) return false;
    if ((size_t)(r.end - r.p) < n) { r.bad = true; return false; }
    return true;
}

inline uint32_t U32(Reader& r) {
    if (!Have(r, 4)) return 0;
    uint32_t v;
    memcpy(&v, r.p, 4);
    r.p += 4;
    return v;
}

inline uint8_t U8(Reader& r) {
    if (!Have(r, 1)) return 0;
    return *r.p++;
}

// The client writes size_t as four bytes; the header says so and is checked.
inline uint32_t Size(Reader& r) { return U32(r); }

// The header the client's DumpHeader writes: the signature, then nine bytes
// describing the machine it was written on. Anything else is a file from a
// different build and is refused rather than interpreted.
bool CheckHeader(Reader& r) {
    if (!Have(r, 12)) return false;
    static const unsigned char kExpect[12] = {
        0x1B, 'L', 'u', 'a',
        0x51,   // version 5.1
        0x00,   // format 0
        0x01,   // little endian
        0x04,   // sizeof(int)
        0x04,   // sizeof(size_t)
        0x04,   // sizeof(Instruction)
        0x08,   // sizeof(lua_Number)
        0x00    // lua_Number is not integral
    };
    if (memcmp(r.p, kExpect, 12) != 0) { r.bad = true; return false; }
    r.p += 12;
    return true;
}

// --- Pass one: walk it all, allocate nothing --------------------------------

bool ScanString(Reader& r) {
    uint32_t n = Size(r);
    if (r.bad) return false;
    if (n == 0) return true;
    if (n > kMaxStringLen) { r.bad = true; return false; }
    if (!Have(r, n)) return false;
    // The client always writes the terminator it counted.
    if (r.p[n - 1] != 0) { r.bad = true; return false; }
    r.p += n;
    return true;
}

bool ScanFunction(Reader& r, int depth) {
    if (depth > kMaxDepth) { r.bad = true; return false; }
    if (!ScanString(r)) return false;          // source
    U32(r);                                    // linedefined
    U32(r);                                    // lastlinedefined
    U8(r);                                     // nups
    U8(r);                                     // numparams
    U8(r);                                     // is_vararg
    U8(r);                                     // maxstacksize
    if (r.bad) return false;

    uint32_t sizecode = U32(r);
    if (r.bad || sizecode > kMaxCode) { r.bad = true; return false; }
    if (sizecode && !Have(r, (size_t)sizecode * 4)) return false;
    r.p += (size_t)sizecode * 4;

    uint32_t sizek = U32(r);
    if (r.bad || sizek > kMaxConstants) { r.bad = true; return false; }
    for (uint32_t i = 0; i < sizek; i++) {
        uint8_t tag = U8(r);
        if (r.bad) return false;
        switch (tag) {
            case kTagNil:     break;
            case kTagBoolean: U8(r); break;
            case kTagNumber:  if (!Have(r, 8)) return false; r.p += 8; break;
            case kTagString:  if (!ScanString(r)) return false; break;
            // A constant can only be one of those four. Anything else means
            // this is not a dump of a Lua 5.1 chunk.
            default: r.bad = true; return false;
        }
    }

    uint32_t sizep = U32(r);
    if (r.bad || sizep > kMaxNested) { r.bad = true; return false; }
    for (uint32_t i = 0; i < sizep; i++)
        if (!ScanFunction(r, depth + 1)) return false;

    uint32_t sizelineinfo = U32(r);
    if (r.bad || sizelineinfo > kMaxLineInfo) { r.bad = true; return false; }
    if (sizelineinfo && !Have(r, (size_t)sizelineinfo * 4)) return false;
    r.p += (size_t)sizelineinfo * 4;

    uint32_t sizelocvars = U32(r);
    if (r.bad || sizelocvars > kMaxLocVars) { r.bad = true; return false; }
    for (uint32_t i = 0; i < sizelocvars; i++) {
        if (!ScanString(r)) return false;
        U32(r);
        U32(r);
        if (r.bad) return false;
    }

    uint32_t sizeupvalues = U32(r);
    if (r.bad || sizeupvalues > kMaxUpvalues) { r.bad = true; return false; }
    for (uint32_t i = 0; i < sizeupvalues; i++)
        if (!ScanString(r)) return false;

    return !r.bad;
}

// --- Pass two: build --------------------------------------------------------

void* NewVector(void* L, size_t bytes) {
    if (bytes == 0) return nullptr;
    return p_luaM_realloc(L, nullptr, 0, bytes);
}

// Returns the interned TString, or null where the writer wrote a null.
void* BuildString(void* L, Reader& r) {
    uint32_t n = Size(r);
    if (n == 0) return nullptr;
    const char* s = (const char*)r.p;
    r.p += n;
    return p_luaS_newlstr(L, s, n - 1);   // the dump counts the terminator
}

void* BuildFunction(void* L, Reader& r, void* parentSource, int depth) {
    void* f = p_luaF_newproto(L);
    if (!f) return nullptr;

    void* source = BuildString(L, r);
    // DumpFunction writes a null source when it matches the parent's, so a
    // nested function inherits it. The top-level call passes null and the
    // top-level source is therefore always written out in full.
    WRP(f, kP_source, source ? source : parentSource);

    WR32(f, kP_linedefined,     U32(r));
    WR32(f, kP_lastlinedefined, U32(r));
    WR8 (f, kP_nups,            U8(r));
    WR8 (f, kP_numparams,       U8(r));
    WR8 (f, kP_isvararg,        U8(r));
    WR8 (f, kP_maxstacksize,    U8(r));

    // Code. Plain integers, not traversed by the collector, so the size can be
    // published as soon as the pointer is.
    uint32_t sizecode = U32(r);
    if (sizecode) {
        void* code = NewVector(L, (size_t)sizecode * 4);
        memcpy(code, r.p, (size_t)sizecode * 4);
        r.p += (size_t)sizecode * 4;
        WRP (f, kP_code, code);
        WR32(f, kP_sizecode, sizecode);
    }

    // Constants. Every one of these is marked unconditionally by the collector,
    // so the array is nil-filled before its size is published.
    uint32_t sizek = U32(r);
    if (sizek) {
        char* k = (char*)NewVector(L, (size_t)sizek * kTV_size);
        memset(k, 0, (size_t)sizek * kTV_size);
        WRP (f, kP_k, k);
        WR32(f, kP_sizek, sizek);

        for (uint32_t i = 0; i < sizek; i++) {
            char* tv = k + (size_t)i * kTV_size;
            uint8_t tag = U8(r);
            switch (tag) {
                case kTagNil:
                    break;
                case kTagBoolean:
                    *(uint32_t*)tv = U8(r) ? 1u : 0u;
                    break;
                case kTagNumber:
                    memcpy(tv, r.p, 8);
                    r.p += 8;
                    break;
                default:  // kTagString; pass one refused everything else
                    *(void**)tv = BuildString(L, r);
                    break;
            }
            WR32(tv, kTV_tt, tag);
            // Zero, and left zero. addk (0x00861F80) copies the token's whole
            // TValue into f->k[n] - taint included - rather than stamping the
            // current one, and then writes that taint back into the global, so
            // the global moves during a parse as constants are added. A
            // constant's taint belongs to the compile that made it, lua_dump
            // does not write it out, and nothing here can reconstruct it.
            //
            // So the store only keeps chunks whose constants were all zero and
            // only replays them while the current taint is zero. See the note
            // in the header.
            WR32(tv, kTV_taint, 0);
        }
    }

    // Nested functions. These are marked as pointers, so the size grows by one
    // only once the slot below it holds a real Proto.
    uint32_t sizep = U32(r);
    if (sizep) {
        void** ps = (void**)NewVector(L, (size_t)sizep * 4);
        memset(ps, 0, (size_t)sizep * 4);
        WRP(f, kP_p, ps);
        for (uint32_t i = 0; i < sizep; i++) {
            ps[i] = BuildFunction(L, r, RDP(f, kP_source), depth + 1);
            WR32(f, kP_sizep, i + 1);
        }
    }

    uint32_t sizelineinfo = U32(r);
    if (sizelineinfo) {
        void* li = NewVector(L, (size_t)sizelineinfo * 4);
        memcpy(li, r.p, (size_t)sizelineinfo * 4);
        r.p += (size_t)sizelineinfo * 4;
        WRP (f, kP_lineinfo, li);
        WR32(f, kP_sizelineinfo, sizelineinfo);
    }

    uint32_t sizelocvars = U32(r);
    if (sizelocvars) {
        char* lv = (char*)NewVector(L, (size_t)sizelocvars * kLV_size);
        memset(lv, 0, (size_t)sizelocvars * kLV_size);
        WRP (f, kP_locvars, lv);
        WR32(f, kP_sizelocvars, sizelocvars);
        for (uint32_t i = 0; i < sizelocvars; i++) {
            char* e = lv + (size_t)i * kLV_size;
            *(void**)e = BuildString(L, r);
            WR32(e, kLV_startpc, U32(r));
            WR32(e, kLV_endpc,   U32(r));
        }
    }

    uint32_t sizeupvalues = U32(r);
    if (sizeupvalues) {
        void** uv = (void**)NewVector(L, (size_t)sizeupvalues * 4);
        memset(uv, 0, (size_t)sizeupvalues * 4);
        WRP(f, kP_upvalues, uv);
        for (uint32_t i = 0; i < sizeupvalues; i++) {
            uv[i] = BuildString(L, r);
            WR32(f, kP_sizeupvalues, i + 1);
        }
    }

    return f;
}

// --- Comparison -------------------------------------------------------------

// Filled in when a comparison fails, so the log can carry the numbers instead
// of a category. A mismatch retires the feature permanently, so the one report
// it produces has to be enough to work from.
char g_detail[256];

bool SameConstants(void* a, void* b, const char** what) {
    uint32_t n = RD32(a, kP_sizek);
    const char* ka = (const char*)RDP(a, kP_k);
    const char* kb = (const char*)RDP(b, kP_k);
    if (n && (!ka || !kb)) { *what = "constants (null)"; return false; }
    for (uint32_t i = 0; i < n; i++) {
        const char* x = ka + (size_t)i * kTV_size;
        const char* y = kb + (size_t)i * kTV_size;
        uint32_t tx = RD32(x, kTV_tt);
        if (tx != RD32(y, kTV_tt)) {
            _snprintf(g_detail, sizeof(g_detail) - 1,
                      "constant %u of %u: rebuilt type %u, parsed type %u",
                      i, n, tx, RD32(y, kTV_tt));
            *what = "a constant's type";
            return false;
        }
        if (RD32(x, kTV_taint) != RD32(y, kTV_taint)) {
            _snprintf(g_detail, sizeof(g_detail) - 1,
                      "constant %u of %u, type %u: rebuilt taint 0x%08X, parsed "
                      "taint 0x%08X, the global reads 0x%08X now",
                      i, n, tx, RD32(x, kTV_taint), RD32(y, kTV_taint),
                      CurrentTaint());
            *what = "a constant's taint";
            return false;
        }
        // Only the bytes the tag gives meaning to. The client's constant array
        // comes from an uninitialised allocation, so the upper half of a
        // boolean or a string slot holds whatever was there before.
        if (tx == kTagNumber) {
            if (memcmp(x, y, 8) != 0)    { *what = "a number constant";  return false; }
        } else if (tx == kTagBoolean || tx == kTagString) {
            if (memcmp(x, y, 4) != 0)    { *what = "a constant's value"; return false; }
        }
    }
    return true;
}

bool EqualInner(void* a, void* b, const char** what, int depth) {
    if (!a || !b)          { *what = "one side is null";  return false; }
    if (depth > kMaxDepth) { *what = "nesting depth";     return false; }

    if (RD32(a, kP_sizecode)        != RD32(b, kP_sizecode))        { *what = "sizecode";        return false; }
    if (RD32(a, kP_sizek)           != RD32(b, kP_sizek))           { *what = "sizek";           return false; }
    if (RD32(a, kP_sizep)           != RD32(b, kP_sizep))           { *what = "sizep";           return false; }
    if (RD32(a, kP_sizelineinfo)    != RD32(b, kP_sizelineinfo))    { *what = "sizelineinfo";    return false; }
    if (RD32(a, kP_sizelocvars)     != RD32(b, kP_sizelocvars))     { *what = "sizelocvars";     return false; }
    if (RD32(a, kP_sizeupvalues)    != RD32(b, kP_sizeupvalues))    { *what = "sizeupvalues";    return false; }
    if (RD32(a, kP_linedefined)     != RD32(b, kP_linedefined))     { *what = "linedefined";     return false; }
    if (RD32(a, kP_lastlinedefined) != RD32(b, kP_lastlinedefined)) { *what = "lastlinedefined"; return false; }
    if (RD8 (a, kP_nups)            != RD8 (b, kP_nups))            { *what = "nups";            return false; }
    if (RD8 (a, kP_numparams)       != RD8 (b, kP_numparams))       { *what = "numparams";       return false; }
    if (RD8 (a, kP_isvararg)        != RD8 (b, kP_isvararg))        { *what = "is_vararg";       return false; }
    if (RD8 (a, kP_maxstacksize)    != RD8 (b, kP_maxstacksize))    { *what = "maxstacksize";    return false; }

    // Equal strings intern to one TString, so every string here is a pointer
    // comparison and any difference is a real difference.
    if (RDP(a, kP_source) != RDP(b, kP_source)) { *what = "source"; return false; }

    uint32_t n = RD32(a, kP_sizecode);
    if (n && memcmp(RDP(a, kP_code), RDP(b, kP_code), (size_t)n * 4) != 0) {
        *what = "code"; return false;
    }
    n = RD32(a, kP_sizelineinfo);
    if (n && memcmp(RDP(a, kP_lineinfo), RDP(b, kP_lineinfo), (size_t)n * 4) != 0) {
        *what = "lineinfo"; return false;
    }

    if (!SameConstants(a, b, what)) return false;

    n = RD32(a, kP_sizelocvars);
    if (n) {
        const char* la = (const char*)RDP(a, kP_locvars);
        const char* lb = (const char*)RDP(b, kP_locvars);
        for (uint32_t i = 0; i < n; i++) {
            const char* x = la + (size_t)i * kLV_size;
            const char* y = lb + (size_t)i * kLV_size;
            if (RDP (x, 0)           != RDP (y, 0))           { *what = "a local's name";    return false; }
            if (RD32(x, kLV_startpc) != RD32(y, kLV_startpc)) { *what = "a local's startpc"; return false; }
            if (RD32(x, kLV_endpc)   != RD32(y, kLV_endpc))   { *what = "a local's endpc";   return false; }
        }
    }

    n = RD32(a, kP_sizeupvalues);
    if (n) {
        void* const* ua = (void* const*)RDP(a, kP_upvalues);
        void* const* ub = (void* const*)RDP(b, kP_upvalues);
        for (uint32_t i = 0; i < n; i++)
            if (ua[i] != ub[i]) { *what = "an upvalue name"; return false; }
    }

    n = RD32(a, kP_sizep);
    if (n) {
        void* const* pa = (void* const*)RDP(a, kP_p);
        void* const* pb = (void* const*)RDP(b, kP_p);
        for (uint32_t i = 0; i < n; i++)
            if (!EqualInner(pa[i], pb[i], what, depth + 1)) return false;
    }
    return true;
}

}  // namespace

// --- Taint ------------------------------------------------------------------

unsigned long CurrentTaintValue() {
    __try {
        return (unsigned long)CurrentTaint();
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        // Unreadable means "not clean", which is the safe answer: every caller
        // treats a non-zero value as a reason to leave the chunk alone.
        return 0xFFFFFFFFul;
    }
}

void* ProtoOnStackTop(void* L) {
    if (!L) return nullptr;
    __try {
        uintptr_t top = *(const uintptr_t*)((const char*)L + kL_top);
        if (top < 0x10000) return nullptr;
        const char* tv = (const char*)(top - kTV_size);
        if (RD32(tv, kTV_tt) != kTagFunction) return nullptr;
        void* cl = RDP(tv, 0);
        if (!cl) return nullptr;
        if (RD8(cl, kC_isC) != 0) return nullptr;     // a C function has no Proto
        return RDP(cl, kC_p);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}

namespace {

bool UntaintedInner(void* f, int depth) {
    if (!f) return false;
    if (depth > kMaxDepth) return false;

    uint32_t n = RD32(f, kP_sizek);
    const char* k = (const char*)RDP(f, kP_k);
    if (n && !k) return false;
    for (uint32_t i = 0; i < n; i++) {
        if (RD32(k + (size_t)i * kTV_size, kTV_taint) != 0) return false;
    }

    n = RD32(f, kP_sizep);
    void* const* ps = (void* const*)RDP(f, kP_p);
    if (n && !ps) return false;
    for (uint32_t i = 0; i < n; i++) {
        if (!UntaintedInner(ps[i], depth + 1)) return false;
    }
    return true;
}

}  // namespace

bool ProtoIsUntainted(void* proto) {
    __try {
        return UntaintedInner(proto, 0);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool Retired() { return g_dead; }

void Retire(const char* why) {
    if (g_dead) return;
    g_dead = true;
    Log("[LuaUndump] Retired for the rest of this session: %s. Chunks will be "
        "compiled from source.", why ? why : "no reason given");
}

bool Available() {
    if (g_probed) return g_usable && !g_dead;
    g_probed = true;
    g_usable = !IsBadReadPtr((void*)kLuaFNewProto, 8)
            && !IsBadReadPtr((void*)kLuaMRealloc,  8)
            && !IsBadReadPtr((void*)kLuaSNewLStr,  8)
            && !IsBadReadPtr((void*)kTaintCurrent, 4);
    if (!g_usable) {
        Log("[LuaUndump] Not available: one of luaF_newproto (0x%08X), "
            "luaM_realloc_ (0x%08X), luaS_newlstr (0x%08X) or the taint slot "
            "(0x%08X) is unreadable.",
            (unsigned)kLuaFNewProto, (unsigned)kLuaMRealloc,
            (unsigned)kLuaSNewLStr, (unsigned)kTaintCurrent);
    }
    return g_usable && !g_dead;
}

void* Load(void* L, const void* data, size_t len) {
    if (!L || !data || len < 13) return nullptr;
    if (!Available()) return nullptr;

    Reader scan;
    scan.p   = (const unsigned char*)data;
    scan.end = scan.p + len;
    scan.bad = false;

    bool sound = false;
    __try {
        sound = CheckHeader(scan) && ScanFunction(scan, 0);
        // Trailing bytes mean this is not the file it claims to be.
        if (sound && scan.p != scan.end) sound = false;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        sound = false;
    }
    if (!sound) { g_refused++; return nullptr; }

    Reader build;
    build.p   = (const unsigned char*)data + 12;
    build.end = scan.end;
    build.bad = false;

    // A fault here is not a malformed file - pass one already ruled that out -
    // so it means an assumption about the client is wrong. Whatever was built
    // is GC-linked and consistent, so letting go of it is enough.
    void* f = nullptr;
    __try {
        f = BuildFunction(L, build, nullptr, 0);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        Retire("building a Proto from a buffer that had already been checked faulted");
        return nullptr;
    }
    if (!f) { g_refused++; return nullptr; }
    g_loaded++;
    g_bytesIn += (unsigned long)len;
    return f;
}

const char* LastDetail() { return g_detail; }

bool Equal(void* a, void* b, const char** what) {
    const char* ignored = "";
    if (!what) what = &ignored;
    *what = "";
    g_detail[0] = 0;
    __try {
        return EqualInner(a, b, what, 0);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        *what = "a field that could not be read";
        return false;
    }
}

// Printed from the periodic report. Shutdown does not run - the DLL exits
// through TerminateProcess - so anything reported only from there is never seen.
void LogStats() {
    if (!g_probed) {
        Log("[LuaUndump] Not measured: nothing has asked it to load a chunk, so "
            "it has not looked for its client entry points.");
        return;
    }
    if (!g_usable) {
        Log("[LuaUndump] Not available on this client; see the line above.");
        return;
    }
    Log("[LuaUndump] built=%lu (%lu KB of bytecode) refused=%lu%s",
        g_loaded, g_bytesIn / 1024, g_refused, g_dead ? " RETIRED" : "");
}

}  // namespace LuaUndump
