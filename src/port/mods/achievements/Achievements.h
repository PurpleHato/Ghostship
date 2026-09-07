#pragma once

#include "types.h"

typedef struct AchievementSaveEntry {
    const char* id;
    int32_t progress;
} AchievementSaveEntry;

struct AchievementSaveData {
    bool cheated;
    int32_t capStars;
    int32_t coins;
    AchievementSaveEntry entries[100];
};

typedef struct AchievementProgress {
    int32_t progress;
    bool achieved;
} AchievementProgress;

#define HAS_ACHIEVEMENTS(fileNum) (gSaveBuffer.files[fileNum]->shipSaveData.features.achievements && gCurrDemoInput == NULL)

#ifdef __cplusplus
#include <map>

enum class AchievementCategory {
    None      = 0,
    Stars   = 1 << 0,
    Caps    = 1 << 1,
    Levels  = 1 << 2,
    Bosses  = 1 << 3,
    Deaths  = 1 << 4,
    Extras  = 1 << 5,
};

struct Achievement {
    AchievementCategory category;
    std::string name;
    const char* icon;
    std::string description;
    size_t order;
    std::vector<std::string> dependencies;
    int32_t maxProgress = 1;
};

extern std::unordered_map<std::string, Achievement>         gAchievementList;
extern std::unordered_map<std::string, AchievementProgress> gAchievementProgress;

extern AchievementProgress* Achievement_GetProgress(const std::string& id);
extern void                 Achievement_Progress(const std::string& id, int32_t amount = 1);

/* Mark an achievement achieved + progress=max WITHOUT emitting a notification
 * or queueing a Satella sync — used by the sync-first restore (apply the
 * server's known unlocks to a fresh / deleted local save). No-op if the id is
 * unknown or already achieved. */
extern void                 Achievement_SetAchievedSilent(const std::string& id);

/* Set partial progress WITHOUT marking achieved or queueing a sync — used by
 * the sync-first restore to recover count-based progress (e.g. 50/100 jumps)
 * from the server after a fresh install. */
extern void                 Achievement_SetProgressSilent(const std::string& id, int32_t progress);
#endif