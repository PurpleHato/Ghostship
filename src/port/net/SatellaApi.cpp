#ifdef USE_NETWORKING
#include "port/net/SatellaApi.h"

#include "port/net/AntiCheat.h"
#include "port/net/SatellaAchievementSync.h"
#include "port/net/SatellaAuth.h"
#include "port/net/SatellaClient.h"
#include "ship/Context.h"
#include "ship/config/Config.h"
#include "spdlog/spdlog.h"

#include <nlohmann/json.hpp>
#include <ixwebsocket/IXHttpClient.h>
#include <ixwebsocket/IXSocketTLSOptions.h>

#include <cstdint>
#include <mutex>
#include <string>
#include <thread>

#if !defined(_WIN32)
#include <unistd.h>
#endif

namespace {
// A single long-lived ix::HttpClient shared by every authenticated REST call.
// ix::HttpClient owns one socket guarded by an internal recursive_mutex; we add
// an external mutex so the device-code poll, presence thread and race-finish
// submit thread never interleave calls on the shared instance.
std::mutex gHttpMtx;

ix::HttpClient& Http() {
    static ix::HttpClient client;
    static bool tlsConfigured = false;
    if (!tlsConfigured) {
#if defined(__linux__)
        // HTTPS cert verification fails on Linux without a CA bundle. Mirror the
        // discovery used by the WS client (SatellaClient.cpp BuildTLSOptions).
        ix::SocketTLSOptions opts;
        static constexpr const char* kCAPaths[] = {
            "/etc/ssl/certs/ca-certificates.crt",
            "/etc/pki/tls/certs/ca-bundle.crt",
            "/etc/ssl/ca-bundle.pem",
            "/etc/pki/ca-trust/extracted/pem/tls-ca-bundle.pem",
        };
        for (const char* path : kCAPaths) {
            if (access(path, R_OK) == 0) {
                opts.caFile = path;
                break;
            }
        }
        client.setTLSOptions(opts);
#endif
        tlsConfigured = true;
    }
    return client;
}

std::string LoadToken() {
    auto config = Ship::Context::GetInstance()->GetConfig();
    if (!config) return "";
    return config->GetString("Satella.Token");
}

// Optional override of the Satella server (for local testing). Empty → prod.
std::string LoadHost() {
    auto config = Ship::Context::GetInstance()->GetConfig();
    if (!config) return "";
    return config->GetString("Satella.Host");
}

// Optional override of the Satella WEBSITE URL (where profiles live).
std::string LoadWebsiteUrl() {
    auto config = Ship::Context::GetInstance()->GetConfig();
    if (!config) return "";
    return config->GetString("Satella.WebsiteUrl");
}
} // namespace

namespace Satella {

bool PostJson(const std::string& path, const nlohmann::json& body, int& outStatus, nlohmann::json& outResp) {
    std::lock_guard<std::mutex> lock(gHttpMtx);
    auto& http = Http();
    const std::string url = HttpHost() + path;
    auto args = http.createRequest(url, ix::HttpClient::kPost);
    args->extraHeaders["Content-Type"] = "application/json";
    args->connectTimeout = 10;  // seconds
    args->transferTimeout = 10; // seconds (avoids the 1800s default freezing the caller)
    const std::string token = LoadToken();
    if (!token.empty()) {
        args->extraHeaders["Authorization"] = "Bearer " + token;
    }
    auto resp = http.post(url, body.dump(), args);
    if (!resp) {
        outStatus = 0;
        SPDLOG_DEBUG("Satella: POST {} produced no response", path);
        return false;
    }
    outStatus = resp->statusCode;
    if (resp->statusCode != 200) {
        SPDLOG_DEBUG("Satella: POST {} returned status {}", path, resp->statusCode);
        return false;
    }
    try {
        outResp = nlohmann::json::parse(resp->body);
    } catch (const std::exception& e) {
        SPDLOG_WARN("Satella: POST {} returned non-JSON body: {}", path, e.what());
        return false;
    }
    return true;
}

bool GetJson(const std::string& path, int& outStatus, nlohmann::json& outResp) {
    std::lock_guard<std::mutex> lock(gHttpMtx);
    auto& http = Http();
    const std::string url = HttpHost() + path;
    auto args = http.createRequest(url, ix::HttpClient::kGet);
    args->connectTimeout = 10;
    args->transferTimeout = 10;
    const std::string token = LoadToken();
    if (!token.empty()) {
        args->extraHeaders["Authorization"] = "Bearer " + token;
    }
    auto resp = http.get(url, args);
    if (!resp) {
        outStatus = 0;
        return false;
    }
    outStatus = resp->statusCode;
    if (resp->statusCode != 200) return false;
    try {
        outResp = nlohmann::json::parse(resp->body);
    } catch (...) {
        return false;
    }
    return true;
}

std::string HttpHost() {
    const std::string h = LoadHost();
    return h.empty() ? std::string(kHttpHost) : h;
}

std::string WsHost() {
    std::string h = LoadHost();
    if (h.empty()) {
        return std::string(kWsHost);
    }
    if (h.rfind("https://", 0) == 0) {
        h.replace(0, 8, "wss://");
        return h;
    }
    if (h.rfind("http://", 0) == 0) {
        h.replace(0, 7, "ws://");
        return h;
    }
    return "ws://" + h;
}

std::string WebsiteUrl() {
    const std::string h = LoadWebsiteUrl();
    return h.empty() ? std::string(kWebsiteUrl) : h;
}

} // namespace Satella

// ---------------------------------------------------------------------------
// C bridge consumed by the SM64 C game code (race behaviors, achievements).
// ---------------------------------------------------------------------------

extern "C" int Satella_IsCleanSession(void) {
    return AntiCheat::IsCleanSession() ? 1 : 0;
}

extern "C" int Satella_IsAuthenticated(void) {
    return SatellaAuth::IsAuthenticated() ? 1 : 0;
}

extern "C" void Satella_QueueAchievement(const char* achievementId) {
    if (achievementId == nullptr) return;
    if (!AntiCheat::IsCleanSession()) return;
    if (!SatellaAuth::IsAuthenticated()) return;
    SatellaAchievementSync::Queue(achievementId);
}

namespace {
void SubmitRaceTimeAsync(SatellaCourseId course, unsigned int timeFrames) {
    static constexpr const char* kCourseIds[] = { "BOB", "THI", "CCM_PENGUIN", "PSS" };
    if (course > SATELLA_COURSE_PSS) return;

    // frames @ 30 FPS -> milliseconds
    const uint64_t timeMs = static_cast<uint64_t>(timeFrames) * 1000ULL / 30ULL;

    nlohmann::json body;
    body["courseId"] = kCourseIds[course];
    // Backend reads `time` (ms). The old REST call sent `timeMs`, which the
    // server ignored — submissions silently failed. Also send the anti-cheat
    // flag the server rejects (matches Satella_IsCleanSession gating upstream).
    body["time"] = timeMs;
    body["cheated"] = false;

    int16_t status = 0;
    std::string respBody;
    if (Satella::Client::Instance().RequestJson("/v1/satella/leaderboard/submit", body.dump(), status, respBody) &&
        status == 200) {
        SPDLOG_INFO("Satella: submitted {} time ({} ms)", kCourseIds[course], timeMs);
    } else {
        SPDLOG_WARN("Satella: leaderboard submit for {} failed (status {})", kCourseIds[course], status);
    }
}
} // namespace

extern "C" int Satella_SubmitRaceTime(SatellaCourseId course, unsigned int timeFrames) {
    if (!AntiCheat::IsCleanSession()) return 0;
    if (!SatellaAuth::IsAuthenticated()) return 0;
    // POST off the game thread so the race-finish frame never stalls.
    std::thread(SubmitRaceTimeAsync, course, timeFrames).detach();
    return 1;
}

#else
// USE_NETWORKING off: empty stubs so the C game code compiles and links unmodified.
#include "port/net/SatellaApi.h"

extern "C" int Satella_SubmitRaceTime(SatellaCourseId, unsigned int) { return 0; }
extern "C" void Satella_QueueAchievement(const char*) {}
extern "C" int Satella_IsCleanSession(void) { return 1; }
extern "C" int Satella_IsAuthenticated(void) { return 0; }
#endif
