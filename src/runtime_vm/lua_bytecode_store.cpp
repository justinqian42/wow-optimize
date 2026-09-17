// ============================================================================
// Description: Keeps compiled Lua chunks on disk so the next session skips the
//              parse as well as the repeat.
// ============================================================================
// The proto cache next door removes the second and later compiles of a chunk
// within one session. It measured what that leaves behind: a loading screen
// that spends 2128 ms in the Lua compiler, of which only 260 ms is source the
// session had already seen. The other 1868 ms is first sightings - work no
// cache living inside the process can remove, because the process has never
// seen the source before.
//
// It has been seen before. It was seen last time the game ran.
//
// So the chunk is dumped with the client's own lua_dump, kept on disk, and on a
// later run handed to LuaUndump, which builds the Proto the parse would have
// built. The proto cache drives both ends: it already knows the identity of a
// chunk, and it already owns the two hooks - luaY_parser, where a Proto is
// returned, and luaL_loadbuffer, where the finished closure is on the stack and
// can be dumped.
// ---------------------------------------------------------------------------
// Nothing is trusted until it has been proved, chunk by chunk
//
// A wrong Proto is worse than a slow one: it is arbitrary bytecode running with
// the caller's permissions. So a hit from disk does not go straight back.
//
// For the first kProveFirst hits the client parses the source as well, and the
// two Protos are compared field by field, into every nested function, including
// each constant's tag, value and taint word - LuaUndump::Equal. The caller is
// handed the client's Proto, never ours, so a session that is still proving
// behaves exactly like a session with this switched off. After that, one hit in
// kResampleMask+1 is proved the same way, so a store that goes wrong later is
// still caught.
//
// The first disagreement retires the store for the rest of the session and says
// which field differed. There is no second chance and no repair.
// ---------------------------------------------------------------------------
// Identity
//
// The proto cache resolves a hash collision by memcmp against the source it
// kept. This store cannot: keeping every chunk's source on disk to compare
// against would cost more than the bytecode does.
//
// What it keeps instead is two independent 64-bit hashes of the source, its
// exact length, and the chunk name in full. A hit needs all four to agree, and
// the name is compared byte for byte. Two different chunks colliding on both
// hashes and the length is not a thing that happens by accident; this is not a
// defence against someone constructing one on purpose, and the file lives in
// the user's own game folder, where anything that could write it could write
// the addons instead.
// ---------------------------------------------------------------------------
// Two files, because this client is killed rather than closed
//
//   bytecode.bin   a small header, then the bytecode blobs back to back, only
//                  ever appended to
//   bytecode.idx   the same header plus how far into the blob file it vouches
//                  for, then one record per chunk, rewritten whole
//
// Two files, not an index trailing the blob in one. This client exits through
// TerminateProcess, so a session always ends between a capture and a save, and a
// capture after a save would land on the index that save had just written. Split,
// the blob only grows, the index names the prefix vouched for, and a kill loses
// the captures made since the last save and nothing else.
//
// A pair whose header does not match this build of the DLL, or whose Wow.exe
// stamp has changed, is discarded and started again. Patching the client
// changes the compiler, so bytecode from before the patch has to go.
// ---------------------------------------------------------------------------
// The index is in memory, the bytecode is not
//
// A 32-bit client allocates from below 2GB, and three tester sessions have
// ended with the largest free run there at one or two megabytes. So this holds
// an index - a key, two lengths, an offset and a name, about forty bytes an
// entry - and reads a chunk's bytes off disk at the moment it is asked for,
// once per chunk per session, into one reused buffer.
// ============================================================================

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <cstdint>
#include <cstring>
#include <cstdio>
#include <cstddef>
#include <string>
#include <unordered_map>
#include <vector>

#include "lua_bytecode_store.h"
#include "lua_undump.h"
#include "config.h"
#include "version.h"

extern "C" void Log(const char* fmt, ...);

namespace LuaBytecodeStore {

namespace {

constexpr uintptr_t kLuaDump = 0x0084ED00;   // int (L, writer, data)

typedef int (__cdecl* lua_Writer)(void* L, const void* p, size_t sz, void* ud);
typedef int (__cdecl* lua_dump_fn)(void* L, lua_Writer w, void* data);

lua_dump_fn p_lua_dump = (lua_dump_fn)kLuaDump;

// How much proving happens before a hit is served without one, and how often a
// proof is repeated afterwards. Two thousand is what the proto cache uses for
// the same job and it costs one extra parse each, on chunks that were going to
// be parsed anyway before this existed.
constexpr unsigned long kProveFirst   = 2000;
constexpr unsigned long kResampleMask = 255;

// The budget. A measured session compiled 40 MB of source; the bytecode for
// that is well inside this. If it ever binds, capture stops and the log says
// how much was turned away, which is the number that would justify building
// compaction - there is none, and a full store keeps what it has.
constexpr uint32_t kMaxEntries    = 16384;
constexpr uint32_t kMaxFileBytes  = 32u * 1024 * 1024;
constexpr uint32_t kMaxChunkBytes = 4u * 1024 * 1024;

constexpr char     kMagic[8]      = { 'W','O','W','O','P','T','B','C' };
constexpr uint32_t kFormatVersion = 1;

#pragma pack(push, 1)
struct FileHeader {
    char     magic[8];
    uint32_t formatVersion;
    uint32_t dllStamp;        // changes whenever this DLL's version string does
    uint8_t  luaHeader[12];   // the client's own dump header, verbatim
    uint32_t exeSize;
    uint32_t exeTimeLo;
    uint32_t exeTimeHi;
    uint32_t entryCount;      // index file only
    uint32_t blobValidLen;    // index file only: how much of the blob file counts
};
struct IndexRecord {
    uint64_t h1;
    uint64_t h2;
    uint32_t srcLen;
    uint32_t blobOffset;
    uint32_t blobLen;
    uint32_t nameLen;
};
#pragma pack(pop)

struct Entry {
    uint64_t h2;
    uint32_t srcLen;
    uint32_t blobOffset;
    uint32_t blobLen;
    uint32_t nameOffset;      // into g_names
    uint32_t nameLen;
};

// --- State ------------------------------------------------------------------

bool     g_dead     = false;
bool     g_ready    = false;
bool     g_dirty    = false;
HANDLE   g_bin      = INVALID_HANDLE_VALUE;
HANDLE   g_idx      = INVALID_HANDLE_VALUE;
uint32_t g_appendAt = 0;

std::unordered_map<uint64_t, Entry> g_index;
std::vector<char>                   g_names;
std::vector<unsigned char>          g_scratch;   // one reused read/dump buffer
uint8_t  g_luaHeader[12];
bool     g_luaHeaderKnown = false;

unsigned long g_hits        = 0;   // served from disk without a parse
unsigned long g_proved      = 0;
unsigned long g_misses      = 0;
unsigned long g_captured    = 0;
unsigned long g_captureFail = 0;
unsigned long g_full        = 0;
unsigned long g_fullBytes   = 0;
unsigned long g_undumpFail  = 0;
unsigned long g_loadedIndex = 0;
unsigned long g_saves       = 0;
// Chunks left alone because a taint was involved. Not failures: they are the
// part of the problem this store is not able to solve, and the log has to be
// able to say how big that part is.
unsigned long g_taintedChunk = 0;   // a constant in it carried a taint
unsigned long g_taintedNow   = 0;   // the context was tainted at the time
double        g_msPerParse  = 0.0;   // measured by the proto cache, see below
unsigned long g_parseSeq    = 0;

uint64_t Fnv1a(const void* d, size_t n, uint64_t h) {
    const unsigned char* p = (const unsigned char*)d;
    for (size_t i = 0; i < n; i++) { h ^= p[i]; h *= 0x100000001b3ULL; }
    return h;
}

// The same shape as the proto cache's key, so the two agree on identity.
uint64_t Key1(const char* src, size_t srcLen, const char* name, size_t nameLen) {
    uint64_t h = Fnv1a(src, srcLen, 0xcbf29ce484222325ULL);
    h = Fnv1a(name, nameLen, h);
    h ^= (uint64_t)srcLen * 0x9E3779B97F4A7C15ULL;
    return h;
}

// A second hash over the same bytes, from a different basis and in the other
// order, so a pair of chunks would have to collide on both.
uint64_t Key2(const char* src, size_t srcLen, const char* name, size_t nameLen) {
    uint64_t h = Fnv1a(name, nameLen, 0x84222325cbf29ce4ULL);
    h = Fnv1a(src, srcLen, h);
    h ^= (uint64_t)nameLen * 0xC2B2AE3D27D4EB4FULL;
    return h;
}

uint32_t DllStamp() {
    return (uint32_t)Fnv1a(WOW_OPTIMIZE_VERSION_STR,
                           strlen(WOW_OPTIMIZE_VERSION_STR),
                           0xcbf29ce484222325ULL);
}

std::string StorePath(const char* suffix) {
    char path[MAX_PATH];
    DWORD n = GetModuleFileNameA(NULL, path, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return std::string();
    std::string s(path);
    size_t slash = s.find_last_of("\\/");
    if (slash == std::string::npos) return std::string();
    std::string dir = s.substr(0, slash + 1) + "Cache";
    CreateDirectoryA(dir.c_str(), NULL);
    return dir + "\\wow_optimize_bytecode" + suffix;
}

bool ExeStamp(uint32_t* size, uint32_t* lo, uint32_t* hi) {
    char path[MAX_PATH];
    DWORD n = GetModuleFileNameA(NULL, path, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return false;
    WIN32_FILE_ATTRIBUTE_DATA fad;
    if (!GetFileAttributesExA(path, GetFileExInfoStandard, &fad)) return false;
    *size = fad.nFileSizeLow;
    *lo   = fad.ftLastWriteTime.dwLowDateTime;
    *hi   = fad.ftLastWriteTime.dwHighDateTime;
    return true;
}

bool ReadAt(HANDLE h, uint32_t off, void* dst, uint32_t len) {
    if (len == 0) return true;
    LARGE_INTEGER li;
    li.QuadPart = off;
    if (!SetFilePointerEx(h, li, NULL, FILE_BEGIN)) return false;
    DWORD got = 0;
    if (!ReadFile(h, dst, len, &got, NULL)) return false;
    return got == len;
}

bool WriteAt(HANDLE h, uint32_t off, const void* src, uint32_t len) {
    if (len == 0) return true;
    LARGE_INTEGER li;
    li.QuadPart = off;
    if (!SetFilePointerEx(h, li, NULL, FILE_BEGIN)) return false;
    DWORD put = 0;
    if (!WriteFile(h, src, len, &put, NULL)) return false;
    return put == len;
}

void FillHeader(FileHeader* h) {
    memset(h, 0, sizeof(*h));
    memcpy(h->magic, kMagic, 8);
    h->formatVersion = kFormatVersion;
    h->dllStamp      = DllStamp();
    ExeStamp(&h->exeSize, &h->exeTimeLo, &h->exeTimeHi);
}

void Truncate(HANDLE h) {
    LARGE_INTEGER zero;
    zero.QuadPart = 0;
    SetFilePointerEx(h, zero, NULL, FILE_BEGIN);
    SetEndOfFile(h);
}

void StartFresh() {
    g_index.clear();
    g_names.clear();
    g_appendAt = sizeof(FileHeader);
    g_luaHeaderKnown = false;
    memset(g_luaHeader, 0, sizeof(g_luaHeader));

    FileHeader h;
    FillHeader(&h);
    Truncate(g_bin);
    WriteAt(g_bin, 0, &h, sizeof(h));
    // An index with no entries and no bytes vouched for. Written now rather
    // than at the first save, so a session that captures nothing still leaves a
    // pair the next one can read instead of a stale one it has to discard.
    h.entryCount   = 0;
    h.blobValidLen = sizeof(FileHeader);
    Truncate(g_idx);
    WriteAt(g_idx, 0, &h, sizeof(h));
    g_dirty = false;
}

// Reads the index trailer. Anything that does not add up starts the file again
// rather than being interpreted, because a store is worth having only if every
// entry in it can be trusted.
bool SameBuild(const FileHeader& h) {
    if (memcmp(h.magic, kMagic, 8) != 0)   return false;
    if (h.formatVersion != kFormatVersion) return false;
    if (h.dllStamp != DllStamp())          return false;
    uint32_t es = 0, lo = 0, hi = 0;
    if (!ExeStamp(&es, &lo, &hi))          return false;
    return h.exeSize == es && h.exeTimeLo == lo && h.exeTimeHi == hi;
}

bool LoadIndex() {
    FileHeader ih, bh;
    if (!ReadAt(g_idx, 0, &ih, sizeof(ih))) return false;
    if (!ReadAt(g_bin, 0, &bh, sizeof(bh))) return false;
    if (!SameBuild(ih) || !SameBuild(bh))   return false;
    // The two files have to be describing the same client and the same dump
    // format, or they are a pair only by their names.
    if (memcmp(ih.luaHeader, bh.luaHeader, 12) != 0) return false;
    if (ih.entryCount > kMaxEntries) return false;

    LARGE_INTEGER binSize;
    if (!GetFileSizeEx(g_bin, &binSize)) return false;
    if (ih.blobValidLen < sizeof(FileHeader)) return false;
    if (ih.blobValidLen > kMaxFileBytes) return false;
    // Captures made after the last save leave the blob file longer than the
    // index vouches for. Shorter means the blob file was replaced or truncated
    // under a matching index, which is not a pair worth reading.
    if ((LONGLONG)ih.blobValidLen > binSize.QuadPart) return false;

    memcpy(g_luaHeader, ih.luaHeader, 12);
    g_luaHeaderKnown = (g_luaHeader[0] == 0x1B);

    uint32_t at = sizeof(FileHeader);
    for (uint32_t i = 0; i < ih.entryCount; i++) {
        IndexRecord r;
        if (!ReadAt(g_idx, at, &r, sizeof(r))) return false;
        at += sizeof(r);
        if (r.nameLen > 4096 || r.blobLen == 0 || r.blobLen > kMaxChunkBytes) return false;
        if (r.blobOffset < sizeof(FileHeader)) return false;
        if ((uint64_t)r.blobOffset + r.blobLen > ih.blobValidLen) return false;

        Entry e;
        e.h2         = r.h2;
        e.srcLen     = r.srcLen;
        e.blobOffset = r.blobOffset;
        e.blobLen    = r.blobLen;
        e.nameLen    = r.nameLen;
        e.nameOffset = (uint32_t)g_names.size();
        g_names.resize(g_names.size() + r.nameLen);
        if (r.nameLen && !ReadAt(g_idx, at, &g_names[e.nameOffset], r.nameLen)) return false;
        at += r.nameLen;
        g_index[r.h1] = e;
    }
    // Anything the blob file holds past this point was captured after the last
    // save and has no index entry, so it is written over rather than kept.
    g_appendAt = ih.blobValidLen;
    g_loadedIndex = (unsigned long)g_index.size();
    return true;
}

// --- Capture ----------------------------------------------------------------

int __cdecl DumpWriter(void*, const void* p, size_t sz, void* ud) {
    std::vector<unsigned char>* out = (std::vector<unsigned char>*)ud;
    if (out->size() + sz > kMaxChunkBytes) return 1;   // stop the dump
    const unsigned char* b = (const unsigned char*)p;
    out->insert(out->end(), b, b + sz);
    return 0;
}

// The __try lives here rather than in Capture, which owns the vector: a frame
// that has to unwind a C++ object cannot also carry a structured handler.
int SafeDump(void* L, std::vector<unsigned char>* out) {
    __try {
        return p_lua_dump(L, DumpWriter, out);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 1;
    }
}

}  // namespace

// ---------------------------------------------------------------------------

bool Init() {
    if (!Config::g_settings.OptLuaBytecodeStore) return true;
    if (!LuaUndump::Available()) {
        Log("[BytecodeStore] NOT active: the undumper cannot run on this client, "
            "so nothing kept on disk could be read back.");
        return false;
    }
    if (IsBadReadPtr((void*)kLuaDump, 8)) {
        Log("[BytecodeStore] NOT active: lua_dump (0x%08X) is unreadable.",
            (unsigned)kLuaDump);
        return false;
    }

    // No write sharing: two processes appending to one blob file would
    // interleave. A second client started against the same game folder used to
    // fail here and run the whole session without a store, which a tester's
    // two-client log shows on every report. It takes the next numbered pair
    // instead, the way the log files do, and keeps it for its next session.
    std::string binPath, idxPath;
    DWORD err = 0;
    for (int slot = 1; slot <= 8; ++slot) {
        const std::string tag = slot == 1 ? std::string() : "." + std::to_string(slot);
        binPath = StorePath((tag + ".bin").c_str());
        idxPath = StorePath((tag + ".idx").c_str());
        if (binPath.empty()) {
            Log("[BytecodeStore] NOT active: could not work out where Wow.exe lives.");
            return false;
        }
        g_bin = CreateFileA(binPath.c_str(), GENERIC_READ | GENERIC_WRITE,
                            FILE_SHARE_READ, NULL, OPEN_ALWAYS,
                            FILE_ATTRIBUTE_NORMAL, NULL);
        const DWORD binErr = GetLastError();
        g_idx = CreateFileA(idxPath.c_str(), GENERIC_READ | GENERIC_WRITE,
                            FILE_SHARE_READ, NULL, OPEN_ALWAYS,
                            FILE_ATTRIBUTE_NORMAL, NULL);
        const DWORD idxErr = GetLastError();
        if (g_bin != INVALID_HANDLE_VALUE && g_idx != INVALID_HANDLE_VALUE) break;
        err = g_bin == INVALID_HANDLE_VALUE ? binErr : idxErr;
        if (g_bin != INVALID_HANDLE_VALUE) { CloseHandle(g_bin); g_bin = INVALID_HANDLE_VALUE; }
        if (g_idx != INVALID_HANDLE_VALUE) { CloseHandle(g_idx); g_idx = INVALID_HANDLE_VALUE; }
    }
    if (g_bin == INVALID_HANDLE_VALUE || g_idx == INVALID_HANDLE_VALUE) {
        Log("[BytecodeStore] NOT active: every numbered store up to %s is held by "
            "another client or cannot be opened (last error %lu).",
            binPath.c_str(), err);
        return false;
    }

    if (!LoadIndex()) {
        StartFresh();
        Log("[BytecodeStore] Active: starting a new store at %s. Chunks compiled "
            "this session are kept for the next one.", binPath.c_str());
    } else {
        Log("[BytecodeStore] Active: %lu chunks from an earlier session, %lu KB "
            "on disk. The first %lu of them are parsed as well and compared "
            "field by field before any is trusted, and one in %u after that.",
            g_loadedIndex, g_appendAt / 1024, kProveFirst,
            (unsigned)(kResampleMask + 1));
    }
    g_ready = true;
    return true;
}

void* Lookup(void* L, const char* src, size_t srcLen, const char* name,
             size_t nameLen, bool* needsCheck) {
    *needsCheck = false;
    if (!g_ready || g_dead || LuaUndump::Retired()) return nullptr;
    if (!L || !src || !name) return nullptr;

    // Only into a clean context. Everything in the store was captured with
    // every constant's taint at zero, and that is what it will be rebuilt with;
    // handing it to a tainted caller would produce a Proto less tainted than a
    // parse would have made, which is the wrong direction to be wrong in.
    if (LuaUndump::CurrentTaintValue() != 0) { g_taintedNow++; return nullptr; }

    uint64_t k1 = Key1(src, srcLen, name, nameLen);
    std::unordered_map<uint64_t, Entry>::iterator it = g_index.find(k1);
    if (it == g_index.end()) { g_misses++; return nullptr; }

    const Entry& e = it->second;
    if (e.srcLen != (uint32_t)srcLen || e.nameLen != (uint32_t)nameLen) { g_misses++; return nullptr; }
    if (e.h2 != Key2(src, srcLen, name, nameLen))                       { g_misses++; return nullptr; }
    if (e.nameLen && memcmp(&g_names[e.nameOffset], name, e.nameLen) != 0) { g_misses++; return nullptr; }

    // Blobs are written the moment they are captured, so an entry from this
    // session is as readable as one from last week. This is a bounds check on
    // an index that has already been trusted once, at load.
    if (e.blobLen == 0 || e.blobOffset + e.blobLen > g_appendAt) { g_misses++; return nullptr; }

    if (g_scratch.size() < e.blobLen) g_scratch.resize(e.blobLen);
    if (!ReadAt(g_bin, e.blobOffset, &g_scratch[0], e.blobLen)) {
        g_dead = true;
        Log("[BytecodeStore] Retired: could not read %u bytes at %u from the "
            "store. Nothing more will be served from it this session.",
            e.blobLen, e.blobOffset);
        return nullptr;
    }

    void* proto = LuaUndump::Load(L, &g_scratch[0], e.blobLen);
    if (!proto) {
        g_undumpFail++;
        return nullptr;
    }

    *needsCheck = (g_hits + g_proved) < kProveFirst
               || ((g_parseSeq++ & kResampleMask) == 0);
    if (!*needsCheck) g_hits++;
    return proto;
}

bool Confirm(void* mine, void* fresh, const char* name) {
    const char* what = "";
    if (LuaUndump::Equal(mine, fresh, &what)) {
        g_proved++;
        return true;
    }
    g_dead = true;
    LuaUndump::Retire("a Proto rebuilt from the store did not match a fresh parse");
    const char* detail = LuaUndump::LastDetail();
    Log("[BytecodeStore] Retired: the Proto rebuilt for \"%s\" differs from the "
        "one the client just parsed, in %s%s%s. It had agreed on %lu before this "
        "one. The store is not used again this session and the files are "
        "discarded on the next start.",
        name ? name : "?", what,
        (detail && detail[0]) ? " - " : "",
        (detail && detail[0]) ? detail : "",
        g_proved);
    // Clear the index so the next session starts from nothing rather than
    // reading a store this one has decided it cannot trust. The blob file is
    // left alone; without an index it is unreachable and will be truncated the
    // next time a fresh store is started.
    if (g_idx != INVALID_HANDLE_VALUE) {
        uint32_t zero = 0;
        WriteAt(g_idx, (uint32_t)offsetof(FileHeader, formatVersion), &zero, 4);
        FlushFileBuffers(g_idx);
    }
    return false;
}

void Capture(void* L, const char* src, size_t srcLen, const char* name,
             size_t nameLen) {
    if (!g_ready || g_dead || !L || !src || !name) return;
    if (srcLen == 0 || nameLen == 0 || nameLen > 4096) return;

    // A constant's taint is copied from the token that made it and lua_dump
    // does not write it out, so a chunk with one cannot be rebuilt faithfully.
    // Keep the clean ones - the game's own interface code, which is where the
    // seconds are - and leave the rest to the compiler.
    if (LuaUndump::CurrentTaintValue() != 0) { g_taintedNow++; return; }
    void* proto = LuaUndump::ProtoOnStackTop(L);
    if (!proto) { g_captureFail++; return; }
    if (!LuaUndump::ProtoIsUntainted(proto)) { g_taintedChunk++; return; }

    uint64_t k1 = Key1(src, srcLen, name, nameLen);
    if (g_index.find(k1) != g_index.end()) return;

    if (g_index.size() >= kMaxEntries || g_appendAt >= kMaxFileBytes) {
        g_full++;
        g_fullBytes += (unsigned long)srcLen;
        return;
    }

    std::vector<unsigned char> blob;
    int rc = SafeDump(L, &blob);
    if (rc != 0 || blob.size() < 13 || blob.size() > kMaxChunkBytes) {
        g_captureFail++;
        return;
    }
    // The first dump of the session fixes the header every later one has to
    // match. A client that started writing a different header mid-session is
    // not a thing that happens, and if it did the file would be unreadable
    // next time rather than wrong.
    if (!g_luaHeaderKnown) {
        memcpy(g_luaHeader, &blob[0], 12);
        g_luaHeaderKnown = true;
        WriteAt(g_bin, (uint32_t)offsetof(FileHeader, luaHeader), g_luaHeader, 12);
    } else if (memcmp(&blob[0], g_luaHeader, 12) != 0) {
        g_captureFail++;
        return;
    }

    if (!WriteAt(g_bin, g_appendAt, &blob[0], (uint32_t)blob.size())) {
        g_captureFail++;
        return;
    }

    Entry e;
    e.h2         = Key2(src, srcLen, name, nameLen);
    e.srcLen     = (uint32_t)srcLen;
    e.blobOffset = g_appendAt;
    e.blobLen    = (uint32_t)blob.size();
    e.nameLen    = (uint32_t)nameLen;
    e.nameOffset = (uint32_t)g_names.size();
    g_names.resize(g_names.size() + nameLen);
    memcpy(&g_names[e.nameOffset], name, nameLen);
    g_index[k1] = e;

    g_appendAt += (uint32_t)blob.size();
    g_captured++;
    g_dirty = true;
}

void SaveIfDirty() {
    if (!g_ready || g_dead || !g_dirty) return;

    // Captures cluster into loading screens and then stop, so this writes a few
    // times during a load and nothing at all while the player is moving around.
    static DWORD lastSave = 0;
    DWORD now = GetTickCount();
    if (lastSave && (now - lastSave) < 30000) return;
    lastSave = now;

    // The blob file is flushed first. The index is what makes a blob readable,
    // so it must never name bytes that are still only in a write buffer.
    FlushFileBuffers(g_bin);

    std::vector<unsigned char> buf;
    buf.reserve(g_index.size() * (sizeof(IndexRecord) + 32));
    for (std::unordered_map<uint64_t, Entry>::const_iterator it = g_index.begin();
         it != g_index.end(); ++it) {
        IndexRecord r;
        r.h1         = it->first;
        r.h2         = it->second.h2;
        r.srcLen     = it->second.srcLen;
        r.blobOffset = it->second.blobOffset;
        r.blobLen    = it->second.blobLen;
        r.nameLen    = it->second.nameLen;
        const unsigned char* rp = (const unsigned char*)&r;
        buf.insert(buf.end(), rp, rp + sizeof(r));
        if (r.nameLen) {
            const unsigned char* np = (const unsigned char*)&g_names[it->second.nameOffset];
            buf.insert(buf.end(), np, np + r.nameLen);
        }
    }

    FileHeader h;
    FillHeader(&h);
    memcpy(h.luaHeader, g_luaHeader, 12);
    h.entryCount   = (uint32_t)g_index.size();
    h.blobValidLen = g_appendAt;

    // Written whole, header last, so a file caught halfway through this is one
    // whose header still describes the previous save and whose records past
    // that point are simply not read.
    Truncate(g_idx);
    if (!WriteAt(g_idx, sizeof(FileHeader), buf.empty() ? NULL : &buf[0],
                 (uint32_t)buf.size()) ||
        !WriteAt(g_idx, 0, &h, sizeof(h))) {
        g_dead = true;
        Log("[BytecodeStore] Retired: writing the index failed (error %lu). "
            "Nothing captured this session will be there next time.",
            GetLastError());
        return;
    }
    FlushFileBuffers(g_idx);
    g_dirty = false;
    g_saves++;
}

void OnLuaStateSwap() {
    // Nothing to do. The store holds no pointers into the Lua state - a Proto
    // it built belongs to whoever asked for it, exactly like a parsed one.
}

void NoteParseCost(double msPerParse) { g_msPerParse = msPerParse; }

void LogStats() {
    if (!Config::g_settings.OptLuaBytecodeStore) {
        Log("[BytecodeStore] Off. Lua compiled in one session is compiled again "
            "from source in the next one.");
        return;
    }
    if (!g_ready) {
        Log("[BytecodeStore] Not measured: it did not start; see the reason above.");
        return;
    }
    double saved = (g_msPerParse > 0.0) ? (g_msPerParse * (double)g_hits) : 0.0;
    Log("[BytecodeStore] %lu from an earlier session, served=%lu proved=%lu "
        "miss=%lu captured=%lu saves=%lu%s",
        g_loadedIndex, g_hits, g_proved, g_misses, g_captured, g_saves,
        g_dead ? " RETIRED" : "");
    if (g_msPerParse > 0.0) {
        Log("[BytecodeStore]   about %.0f ms of parsing not done, at the %.3f ms "
            "the proto cache measured for an average parse. That figure is the "
            "average over every chunk, so it is an estimate, not a measurement "
            "of these chunks.", saved, g_msPerParse);
    } else {
        Log("[BytecodeStore]   time saved not measured: the proto cache has no "
            "average parse cost yet, so there is nothing to multiply by.");
    }
    if (g_taintedChunk || g_taintedNow) {
        Log("[BytecodeStore]   left alone because of addon ownership: %lu chunk(s) "
            "had a constant carrying it, and %lu request(s) came from a context "
            "that had it. Neither is a failure - a constant's ownership is fixed "
            "when it is first compiled and the game's own dump format does not "
            "record it, so only untouched code can be rebuilt exactly.",
            g_taintedChunk, g_taintedNow);
    }
    if (g_undumpFail || g_captureFail)
        Log("[BytecodeStore]   %lu stored chunks would not rebuild, %lu would not "
            "dump.", g_undumpFail, g_captureFail);
    if (g_full)
        Log("[BytecodeStore]   full: %lu chunks (%lu KB of source) were not kept. "
            "The store holds what it already had.", g_full, g_fullBytes / 1024);
}

void Shutdown() {
    if (g_bin != INVALID_HANDLE_VALUE) {
        SaveIfDirty();
        CloseHandle(g_bin);
        g_bin = INVALID_HANDLE_VALUE;
    }
    if (g_idx != INVALID_HANDLE_VALUE) {
        CloseHandle(g_idx);
        g_idx = INVALID_HANDLE_VALUE;
    }
    g_ready = false;
}

}  // namespace LuaBytecodeStore
