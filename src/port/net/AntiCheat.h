#pragma once

#ifdef USE_NETWORKING
/* Single source of truth for "is this a clean, leaderboard-eligible session?".
 *
 * A session is dirty if ANY gameplay-modifying CVar is currently on, or if it was
 * flagged dirty at runtime (a cheat toggled on mid-run). Cosmetic / QoL
 * enhancements are intentionally NOT checked. This is consulted before every
 * time-trial submission and achievement sync, and is written into
 * AchievementSaveData.cheated on save. */
namespace AntiCheat {
bool IsCleanSession();
void MarkDirty();
void ResetSession();
}
#endif
