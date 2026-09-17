// Description: C-level combat log event parser and aggregator

#include "combatlog_parser.h"
#include "version.h"
#include <windows.h>
#include <cstdint>
#include <cstring>
#include <unordered_map>
#include <string>
#include <vector>
#include <float.h>

extern "C" void Log(const char* fmt, ...);

#if !TEST_DISABLE_COMBATLOG_PARSER

// Basic Lua definitions
#define LUA_GLOBALSINDEX (-10002)
#define LUA_TNIL     0
#define LUA_TBOOLEAN 1
#define LUA_TNUMBER  3
#define LUA_TSTRING  4

union RawValue {
    void*     gc;
    uintptr_t ptr;
    double    n;
};

struct RawTValue {
    RawValue  value;
    int       tt;
    uint32_t  taint;
};

static inline bool IsValidPtr(uintptr_t ptr) {
    return (ptr >= 0x10000 && ptr < 0xFFFFF000);
}

static inline const char* ReadTStringDirect(RawTValue* tv, size_t* out_len) {
    if (tv->tt != LUA_TSTRING) return nullptr;

    void* ts_ptr = tv->value.gc;
    if (!IsValidPtr((uintptr_t)ts_ptr)) return nullptr;

    int len = *(int*)((char*)ts_ptr + 16);
    if (len < 0 || len > 1024) return nullptr;

    char* str = (char*)ts_ptr + 20;
    if (out_len) *out_len = (size_t)len;
    return str;
}

static inline double ReadTNumberDirect(RawTValue* tv) {
    if (tv->tt != LUA_TNUMBER) return 0.0;
    double d;
    memcpy(&d, &tv->value.n, sizeof(double));
    return d;
}

// Stats structures
struct SpellStats {
    uint32_t    spellId;
    std::string spellName;
    uint64_t    damage;
    uint64_t    healing;
    uint64_t    overheal;
    uint64_t    hits;
    uint64_t    crits;
};

struct PlayerStats {
    std::string guid;
    std::string name;
    uint64_t    totalDamage;
    uint64_t    totalHealing;
    uint64_t    totalOverheal;
    std::unordered_map<uint32_t, SpellStats> spells;
};

static std::unordered_map<std::string, PlayerStats> g_combatStats;
static SRWLOCK g_statsLock = SRWLOCK_INIT;
static bool g_combatStatsRequestedEver = false;

// Lua C API bindings for getting and resetting stats
typedef void (__cdecl *fn_lua_createtable)(void* L, int narr, int nrec);
static const fn_lua_createtable lua_createtable_ = (fn_lua_createtable)0x0084E850;

typedef void (__cdecl *fn_lua_settable)(void* L, int idx);
static const fn_lua_settable lua_settable_ = (fn_lua_settable)0x0084E8D0;

typedef void (__cdecl *fn_lua_pushnumber)(void* L, double n);
static const fn_lua_pushnumber lua_pushnumber_ = (fn_lua_pushnumber)0x0084E2A0;

typedef void (__cdecl *fn_lua_pushstring)(void* L, const char* s);
static const fn_lua_pushstring lua_pushstring_ = (fn_lua_pushstring)0x0084E350;

typedef void (__cdecl *fn_lua_pushboolean)(void* L, int b);
static const fn_lua_pushboolean lua_pushboolean_ = (fn_lua_pushboolean)0x0084E4D0;

typedef void (__cdecl *fn_lua_pushnil)(void* L);
static const fn_lua_pushnil lua_pushnil_ = (fn_lua_pushnil)0x0084E280;

typedef void (__cdecl *fn_lua_setfield)(void* L, int idx, const char* name);
static const fn_lua_setfield lua_setfield_ = (fn_lua_setfield)0x0084E900;

typedef int (__cdecl *fn_lua_getfield)(void* L, int idx, const char* name);
static const fn_lua_getfield lua_getfield_ = (fn_lua_getfield)0x0084E590;

typedef void (__cdecl *fn_lua_settop)(void* L, int idx);
static const fn_lua_settop lua_settop_ = (fn_lua_settop)0x0084DBF0;

typedef int (__cdecl *fn_lua_type)(void* L, int idx);
static const fn_lua_type lua_type_ = (fn_lua_type)0x0084DEB0;

typedef int (__cdecl *fn_lua_gettop)(void* L);
static const fn_lua_gettop lua_gettop_ = (fn_lua_gettop)0x0084DBD0;

static void UpdateStats(const char* guid, const char* name, uint32_t spellId, const char* spellName,
                         uint64_t damage, uint64_t healing, uint64_t overheal, bool critical, bool isDamage) {
    if (!guid || !name) return;

    AcquireSRWLockExclusive(&g_statsLock);

    auto& player = g_combatStats[guid];
    if (player.guid.empty()) {
        player.guid = guid;
        player.name = name;
        player.totalDamage = 0;
        player.totalHealing = 0;
        player.totalOverheal = 0;
    }

    if (isDamage) {
        player.totalDamage += damage;
    } else {
        player.totalHealing += healing;
        player.totalOverheal += overheal;
    }

    auto& spell = player.spells[spellId];
    if (spell.spellName.empty()) {
        spell.spellId = spellId;
        spell.spellName = spellName;
        spell.damage = 0;
        spell.healing = 0;
        spell.overheal = 0;
        spell.hits = 0;
        spell.crits = 0;
    }

    if (isDamage) {
        spell.damage += damage;
    } else {
        spell.healing += healing;
        spell.overheal += overheal;
    }

    spell.hits++;
    if (critical) {
        spell.crits++;
    }

    ReleaseSRWLockExclusive(&g_statsLock);
}

void CombatLogParser_ProcessEvent(void* L, int fieldCount) {
    if (!g_combatStatsRequestedEver) return;
    if (fieldCount < 11) return;

    __try {
        // L->top is at L + 0x0C
        uintptr_t* topPtr = *(uintptr_t**)((uintptr_t)L + 0x0C);
        if (!topPtr || !IsValidPtr((uintptr_t)topPtr)) return;

        RawTValue* base = (RawTValue*)topPtr - fieldCount;
        if (!IsValidPtr((uintptr_t)base) || !IsValidPtr((uintptr_t)(base + fieldCount))) {
            return;
        }

        // Read eventType
        size_t eventTypeLen = 0;
        const char* eventType = ReadTStringDirect(&base[1], &eventTypeLen);
        if (!eventType || !IsValidPtr((uintptr_t)eventType)) return;

        // Read sourceGUID and sourceName
        size_t sourceGUIDLen = 0;
        const char* sourceGUID = ReadTStringDirect(&base[3], &sourceGUIDLen);
        size_t sourceNameLen = 0;
        const char* sourceName = ReadTStringDirect(&base[4], &sourceNameLen);

        if (!sourceGUID || !sourceName) return;
        if (!IsValidPtr((uintptr_t)sourceGUID) || !IsValidPtr((uintptr_t)sourceName)) return;

        if (strcmp(eventType, "SWING_DAMAGE") == 0) {
            if (fieldCount < 12) return;
            double amount = ReadTNumberDirect(&base[11]);
            bool critical = false;
            if (fieldCount >= 18 && base[17].tt == LUA_TBOOLEAN) {
                critical = (base[17].value.gc != nullptr);
            }
            UpdateStats(sourceGUID, sourceName, 0, "Melee", (uint64_t)amount, 0, 0, critical, true);
        }
        else if (strcmp(eventType, "SPELL_DAMAGE") == 0 || 
                 strcmp(eventType, "SPELL_PERIODIC_DAMAGE") == 0 ||
                 strcmp(eventType, "RANGE_DAMAGE") == 0) {
            if (fieldCount < 15) return;
            double spellId = ReadTNumberDirect(&base[11]);
            size_t spellNameLen = 0;
            const char* spellName = ReadTStringDirect(&base[12], &spellNameLen);
            double amount = ReadTNumberDirect(&base[14]);
            bool critical = false;
            if (fieldCount >= 21 && base[20].tt == LUA_TBOOLEAN) {
                critical = (base[20].value.gc != nullptr);
            }
            UpdateStats(sourceGUID, sourceName, (uint32_t)spellId, spellName ? spellName : "Unknown", (uint64_t)amount, 0, 0, critical, true);
        }
        else if (strcmp(eventType, "SPELL_HEAL") == 0 || 
                 strcmp(eventType, "SPELL_PERIODIC_HEAL") == 0) {
            if (fieldCount < 15) return;
            double spellId = ReadTNumberDirect(&base[11]);
            size_t spellNameLen = 0;
            const char* spellName = ReadTStringDirect(&base[12], &spellNameLen);
            double amount = ReadTNumberDirect(&base[14]);
            double overhealing = 0;
            if (fieldCount >= 16) {
                overhealing = ReadTNumberDirect(&base[15]);
            }
            bool critical = false;
            if (fieldCount >= 17 && base[16].tt == LUA_TBOOLEAN) {
                critical = (base[16].value.gc != nullptr);
            }
            UpdateStats(sourceGUID, sourceName, (uint32_t)spellId, spellName ? spellName : "Unknown", 0, (uint64_t)amount, (uint64_t)overhealing, critical, false);
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}
extern "C" int LUABOOST_GetCombatStats(void* L) {
    g_combatStatsRequestedEver = true;
    int topBefore = 0;
    __try { topBefore = lua_gettop_(L); } __except(EXCEPTION_EXECUTE_HANDLER) {}

    __try {
        // Create the outer table: { ["playerGUID"] = playerTable, ... }
        lua_createtable_(L, 0, 0);
        
        AcquireSRWLockShared(&g_statsLock);
        
        for (const auto& pair : g_combatStats) {
            const auto& player = pair.second;
            
            // Push player table key (GUID string)
            lua_pushstring_(L, player.guid.c_str());
            
            // Create player table
            lua_createtable_(L, 0, 6);
            
            lua_pushstring_(L, "guid");
            lua_pushstring_(L, player.guid.c_str());
            lua_settable_(L, -3);
            
            // name
            lua_pushstring_(L, "name");
            lua_pushstring_(L, player.name.c_str());
            lua_settable_(L, -3);
            
            // damage
            lua_pushstring_(L, "damage");
            lua_pushnumber_(L, (double)player.totalDamage);
            lua_settable_(L, -3);
            
            // healing
            lua_pushstring_(L, "healing");
            lua_pushnumber_(L, (double)player.totalHealing);
            lua_settable_(L, -3);
            
            // overheal
            lua_pushstring_(L, "overheal");
            lua_pushnumber_(L, (double)player.totalOverheal);
            lua_settable_(L, -3);
            
            // Create spells table
            lua_pushstring_(L, "spells");
            lua_createtable_(L, 0, 0);
            
            for (const auto& spellPair : player.spells) {
                const auto& spell = spellPair.second;
                
                // Push spell table key (spellId number)
                lua_pushnumber_(L, (double)spell.spellId);
                
                // Create spell table
                lua_createtable_(L, 0, 6);
                
                lua_pushstring_(L, "name");
                lua_pushstring_(L, spell.spellName.c_str());
                lua_settable_(L, -3);
                
                lua_pushstring_(L, "damage");
                lua_pushnumber_(L, (double)spell.damage);
                lua_settable_(L, -3);
                
                lua_pushstring_(L, "healing");
                lua_pushnumber_(L, (double)spell.healing);
                lua_settable_(L, -3);
                
                lua_pushstring_(L, "overheal");
                lua_pushnumber_(L, (double)spell.overheal);
                lua_settable_(L, -3);
                
                lua_pushstring_(L, "hits");
                lua_pushnumber_(L, (double)spell.hits);
                lua_settable_(L, -3);
                
                lua_pushstring_(L, "crits");
                lua_pushnumber_(L, (double)spell.crits);
                lua_settable_(L, -3);
                
                // Set spellTable in spells table: spells[spellId] = spellTable
                lua_settable_(L, -3);
            }
            
            // Set spells table in player table: playerTable.spells = spellsTable
            lua_settable_(L, -3);
            
            // Set playerTable in outer table: outer[guid] = playerTable
            lua_settable_(L, -3);
        }
        
        ReleaseSRWLockShared(&g_statsLock);
        return 1;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        // Stack may be corrupted/partially populated: clean up
        __try {
            lua_settop_(L, topBefore);
            lua_pushnil_(L);
        } __except(EXCEPTION_EXECUTE_HANDLER) {}
        return 1;
    }
}

extern "C" int LUABOOST_ResetCombatStats(void* L) {
    g_combatStatsRequestedEver = true;
    AcquireSRWLockExclusive(&g_statsLock);
    g_combatStats.clear();
    ReleaseSRWLockExclusive(&g_statsLock);
    return 0;
}

bool InstallCombatLogParser() {
    AcquireSRWLockExclusive(&g_statsLock);
    g_combatStats.clear();
    ReleaseSRWLockExclusive(&g_statsLock);
    Log("[CombatLogParser] ACTIVE (C++ Aggregator & Lua Interop enabled)");
    return true;
}

void ShutdownCombatLogParser() {
    AcquireSRWLockExclusive(&g_statsLock);
    g_combatStats.clear();
    ReleaseSRWLockExclusive(&g_statsLock);
}

#else

void CombatLogParser_ProcessEvent(void*, int) {}
void CombatLogParser_Update(void*) {}
bool InstallCombatLogParser() { return false; }
void ShutdownCombatLogParser() {}

#endif