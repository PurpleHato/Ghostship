#ifdef USE_NETWORKING
#include "port/net/AntiCheat.h"

#include "sm64.h"
#include "port/ui/cvar_prefixes.h"

#include <atomic>

namespace AntiCheat {

static std::atomic<bool> sDirty{ false };

void MarkDirty() {
    sDirty.store(true, std::memory_order_release);
}

void ResetSession() {
    sDirty.store(false, std::memory_order_release);
}

// Returns false if any gameplay-modifying CVar is on. Each entry is verified in
// GhostshipMenuEnhancements.cpp (and DebugMode is the developer level-select).
// Cosmetic / QoL enhancements are intentionally NOT checked here.
bool IsCleanSession() {
    if (sDirty.load(std::memory_order_acquire)) {
        return false;
    }
    if (CVarGetInteger(CVAR_CHEAT("InfiniteHealth"), 0) != 0) return false;
    if (CVarGetInteger(CVAR_CHEAT("InfiniteLives"), 0) != 0) return false;
    if (CVarGetInteger(CVAR_CHEAT("AlwaysFlyTripleJump"), 0) != 0) return false;
    if (CVarGetInteger(CVAR_CHEAT("FlyingTripleJumpHighLaunch"), 0) != 0) return false;
    if (CVarGetInteger(CVAR_CHEAT("PauseExitWhenever"), 0) != 0) return false;
    if (CVarGetInteger(CVAR_CHEAT("PlayInDemo"), 0) != 0) return false;
    if (CVarGetInteger(CVAR_DEVELOPER_TOOLS("DebugMode"), 0) != 0) return false;
    return true;
}

} // namespace AntiCheat
#endif
