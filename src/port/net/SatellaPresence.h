#pragma once

#ifdef USE_NETWORKING

/* Periodic game-presence reporter.
 *
 * Runs a background worker that snapshots the player's current course / activity
 * roughly every 30s and POSTs it to the Satella presence endpoint over the
 * parallel HTTP layer, so friends on the website can see "Playing Ghostship,
 * Bob-omb Battlefield". Only runs while authenticated. Start() is idempotent;
 * Stop() signals and joins the worker (called from GameEngine::Destroy). */
namespace SatellaPresence {
void Start();
void Stop();
}
#endif
