// ============================================================================
// Description: Keeps hold of the particleDensity CVar object for other code. It
//              no longer scales anything - particle density is the quality
//              governor's now.
//
// What used to be here had the same three defects as the shadow and draw-distance
// scalers it shared a design with. It started from a hardcoded 1.0 and restored to
// 1.0, so a player who chose 0.5 was pushed up to full density rather than
// protected. It switched on 35 / 50 / 58 fps thresholds read off an EMA of
// 1000/elapsed, which a single frame can cross. And it never put anything back.
// ============================================================================

#include "particle_density_scaler.h"
#include "version.h"

extern "C" void Log(const char* fmt, ...);

namespace ParticleDensityScaler {

static void* g_particleDensityCVar = nullptr;

// Called from the CVar::Register detour in adaptive_farclip.cpp. Kept because
// other code may want the object; the governor learns its own by watching writes.
void RegisterCVar(const char* name, void* cvar) {
    if (name && strcmp(name, "particleDensity") == 0) {
        g_particleDensityCVar = cvar;
    }
}

} // namespace ParticleDensityScaler

extern "C" void RegisterParticleCVar(const char* name, void* cvar) {
    ParticleDensityScaler::RegisterCVar(name, cvar);
}
