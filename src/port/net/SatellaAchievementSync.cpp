#ifdef USE_NETWORKING
#include "port/net/SatellaAchievementSync.h"

#include "port/mods/achievements/Achievements.h"
#include "port/net/SatellaApi.h"
#include "port/net/SatellaAuth.h"
#include "port/net/SatellaClient.h"
#include "port/ui/SatellaWindow.h"
#include "spdlog/spdlog.h"

#include <nlohmann/json.hpp>
#include <algorithm>
#include <mutex>
#include <string>
#include <unordered_set>
#include <vector>

namespace SatellaAchievementSync {

static std::mutex sMtx;
static std::vector<std::string> sPending;
static bool sCatalogRegistered = false;

// Blocking JSON request over the authed WS socket. Returns true on HTTP-ny 200
// (and fills outResp with the parsed JSON body). All Satella data calls are
// serialized through Client::RequestJson's internal mutex.
static bool WsRequest(const std::string& route, const nlohmann::json& body, nlohmann::json& outResp) {
    if (!SatellaAuth::IsAuthenticated()) {
        return false;
    }
    int16_t status = 0;
    std::string respBody;
    if (!Satella::Client::Instance().RequestJson(route, body.dump(), status, respBody)) {
        return false;
    }
    if (status != 200) {
        SPDLOG_WARN("Satella: WS {} returned status {}", route, status);
        return false;
    }
    try {
        outResp = respBody.empty() ? nlohmann::json::object() : nlohmann::json::parse(respBody);
    } catch (const std::exception& e) {
        SPDLOG_WARN("Satella: WS {} returned non-JSON body: {}", route, e.what());
        outResp = nlohmann::json::object();
    }
    return true;
}

void Queue(const std::string& id) {
    // Queue achieved achievements (for unlock sync).
    const AchievementProgress* p = Achievement_GetProgress(id);
    if (p == nullptr) return;
    if (p->achieved || p->progress > 0) {
        std::lock_guard<std::mutex> lock(sMtx);
        for (const auto& existing : sPending) {
            if (existing == id) return; // already queued
        }
        sPending.push_back(id);
    }
}

std::vector<std::string> Drain() {
    std::lock_guard<std::mutex> lock(sMtx);
    std::vector<std::string> out;
    out.swap(sPending);
    return out;
}

void Flush() {
    if (!SatellaAuth::IsAuthenticated()) {
        return;
    }
    const auto ids = Drain();
    if (ids.empty()) return;

    nlohmann::json arr = nlohmann::json::array();
    for (const auto& id : ids) {
        const AchievementProgress* p = Achievement_GetProgress(id);
        arr.push_back({
            { "id", id },
            { "progress", p != nullptr ? p->progress : 0 },
            { "achieved", p != nullptr ? p->achieved : true },
        });
    }

    nlohmann::json body;
    body["gameId"] = "ghostship";
    body["achievements"] = arr;

    nlohmann::json resp;
    if (!WsRequest("/v1/satella/achievements/sync", body, resp)) {
        SPDLOG_WARN("Satella: achievement sync failed");
    }
}

void RegisterCatalog() {
    if (sCatalogRegistered || !SatellaAuth::IsAuthenticated()) {
        return;
    }

    nlohmann::json arr = nlohmann::json::array();
    for (const auto& [id, achievement] : gAchievementList) {
        const int points = achievement.maxProgress > 1
            ? std::min(achievement.maxProgress, 50) * 2
            : 10;
        arr.push_back({
            { "id", id },
            { "name", achievement.name },
            { "description", achievement.description },
            { "category", static_cast<int>(achievement.category) },
            { "maxProgress", achievement.maxProgress },
            { "points", points },
            { "icon", achievement.icon },
            { "order", achievement.order },
        });
    }

    nlohmann::json body;
    body["gameId"] = "ghostship";
    body["achievements"] = arr;

    nlohmann::json resp;
    if (WsRequest("/v1/satella/achievements/register", body, resp)) {
        sCatalogRegistered = true;
        SPDLOG_INFO("Satella: registered {} achievements into the catalog", arr.size());
    }
}

std::vector<std::string> PullAndRestore() {
    std::vector<std::string> serverIds;
    if (!SatellaAuth::IsAuthenticated()) {
        SPDLOG_WARN("Satella: PullAndRestore skipped — not authenticated");
        return serverIds;
    }

    nlohmann::json body;
    body["gameId"] = "ghostship";

    nlohmann::json resp;
    if (!WsRequest("/v1/satella/achievements/state", body, resp)) {
        SPDLOG_WARN("Satella: PullAndRestore — WS request to achievements/state failed");
        return serverIds;
    }

    if (!resp.contains("achievements") || !resp["achievements"].is_array()) {
        SPDLOG_WARN("Satella: PullAndRestore — response missing 'achievements' array");
        return serverIds;
    }

    for (const auto& entry : resp["achievements"]) {
        if (entry.contains("achievementId") && entry["achievementId"].is_string()) {
            const std::string id = entry["achievementId"].get<std::string>();
            serverIds.push_back(id);
            // Check if this entry has a progress value and whether it's achieved.
            const int serverProgress = entry.value("progress", 0);
            const bool hasAchievedAt = entry.contains("achievedAt") && !entry["achievedAt"].is_null();

            if (hasAchievedAt) {
                // Fully achieved — restore as complete.
                Achievement_SetAchievedSilent(id);
            } else if (serverProgress > 0) {
                // Partial progress (e.g. 50/100 jumps) — restore the count
                // WITHOUT marking achieved, so the player continues from where
                // they left off after a reinstall.
                Achievement_SetProgressSilent(id, serverProgress);
            }
        }
    }

    SPDLOG_INFO("Satella: restored {} server achievement(s) locally", serverIds.size());
    return serverIds;
}

void FlushAll() {
    if (!SatellaAuth::IsAuthenticated()) {
        return;
    }

    // SYNC-FIRST: pull the server's known unlocks and apply them locally before
    // pushing anything, so a fresh install / deleted save never regresses the
    // server and recovers its own progress. (The server is add-only regardless,
    // so this is belt-and-suspenders.)
    const auto serverIds = PullAndRestore();
    const std::unordered_set<std::string> known(serverIds.begin(), serverIds.end());

    RegisterCatalog();

    // Push local achievements the server doesn't already know about. Queue()
    // accepts both achieved (for unlock sync) and partial-progress entries
    // (so count-based progress like 50/100 jumps is stored server-side).
    for (const auto& [id, achievement] : gAchievementList) {
        (void)achievement;
        if (known.find(id) != known.end()) continue;
        Queue(id);
    }
    Flush();
}

void SubscribeFriendChannels() {
    if (!SatellaAuth::IsAuthenticated()) return;

    // Fetch the friends list.
    nlohmann::json resp;
    if (!WsRequest("/v1/satella/friends/list", nlohmann::json::object(), resp)) {
        return;
    }

    // Read my ULID from the /v1/user endpoint (needed for the channel ID).
    int status = 0;
    nlohmann::json profile;
    if (!Satella::GetJson("/v1/user", status, profile)) return;
    const std::string myUlid = profile.value("ulid", "");
    if (myUlid.empty()) return;

    // Subscribe to each accepted friend's chat channel + pre-cache their avatars.
    if (!resp.contains("friends") || !resp["friends"].is_array()) return;
    int subscribed = 0;
    for (const auto& f : resp["friends"]) {
        if (!f.value("isAccepted", false)) continue;
        const std::string friendUlid = f.value("userId", f.value("ulid", ""));
        if (friendUlid.empty()) continue;

        // Pre-cache the friend's avatar so chat notifications render with it.
        const std::string avatarUrl = f.value("avatar", "");
        if (!avatarUrl.empty()) {
            SatellaRequestAvatar(avatarUrl, "avatar_" + friendUlid);
        }

        const std::string chId = "chat:" + (myUlid < friendUlid
            ? myUlid + ":" + friendUlid
            : friendUlid + ":" + myUlid);

        int16_t st = 0;
        std::string r;
        Satella::Client::Instance().RequestJson("/v1/relay/" + chId + "/subscribe", "{}", st, r);
        ++subscribed;
    }
    if (subscribed > 0) {
        SPDLOG_INFO("Satella: subscribed to {} friend chat channel(s)", subscribed);
    }
}

} // namespace SatellaAchievementSync
#endif
