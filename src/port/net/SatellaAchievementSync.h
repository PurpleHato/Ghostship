#pragma once

#ifdef USE_NETWORKING
#include <string>
#include <vector>

/* Queue-based achievement sync to the Satella website.
 *
 * Achievement unlocks (from BOTH Achievements.cpp unlock paths) are pushed onto a
 * thread-safe queue and flushed to /v1/achievements/sync over the parallel HTTP
 * layer. On Satella connect the queue is re-seeded from every achieved entry in
 * gAchievementList so progress is reconciled even if the player earned things
 * while offline. Dirty sessions are never synced. */
namespace SatellaAchievementSync {
void Queue(const std::string& id);
std::vector<std::string> Drain();
void Flush();
void FlushAll();

/* Sync-down: pull the user's server-side achievements for ghostship and apply
 * them to the local save (silent restore). Returns the set of server-known
 * achievement ids so the caller can push only what's new. Over WebSocket. */
std::vector<std::string> PullAndRestore();

/* Send the full achievement catalog to /v1/satella/achievements/register once
 * per process (after auth) over WebSocket. */
void RegisterCatalog();

/* Subscribe to all accepted-friend relay chat channels so incoming messages
 * are received even before the user opens a conversation. Call after FlushAll. */
void SubscribeFriendChannels();
}
#endif
