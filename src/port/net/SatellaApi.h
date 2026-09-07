#pragma once

/* C-safe bridge between SM64 C game code and the Satella website REST API.
 *
 * This header is safe to #include from .c / .inc.c game files: every C++-only
 * declaration lives under #ifdef __cplusplus and is skipped by the C compiler.
 *
 * Phase 3 adds a PARALLEL HTTP layer (ix::HttpClient, already linked) that talks
 * to the Satella website Express API over HTTPS. It is completely separate from
 * the existing HM64 binary WebSocket client (Satella::Client) and the multiplayer
 * Relay layer — those are left untouched. */

#ifdef __cplusplus
extern "C" {
#endif

/* Course IDs for time-trial submission. Mirrors the Satella API TrialCourseId. */
typedef enum SatellaCourseId {
    SATELLA_COURSE_BOB = 0,
    SATELLA_COURSE_THI = 1,
    SATELLA_COURSE_CCM_PENGUIN = 2,
    SATELLA_COURSE_PSS = 3,
} SatellaCourseId;

/* Submit a finished race time. `timeFrames` is elapsed frames at 30 FPS; the
 * bridge converts to milliseconds. The network POST is dispatched off the game
 * thread so the race-finish frame never stalls. Returns 1 if the submission was
 * dispatched, 0 if skipped (dirty session / not authenticated / networking off). */
int Satella_SubmitRaceTime(SatellaCourseId course, unsigned int timeFrames);

/* Queue an achievement id for sync to the website. No-op if the session is dirty
 * or the user is not authenticated. */
void Satella_QueueAchievement(const char* achievementId);

/* Returns 1 if no gameplay-modifying cheat is active this session, 0 otherwise. */
int Satella_IsCleanSession(void);

/* Returns 1 if a Satella website bearer token is stored, 0 otherwise. */
int Satella_IsAuthenticated(void);

#ifdef __cplusplus
} /* extern "C" */
#endif

#ifdef __cplusplus
#include <string>
#include <nlohmann/json.hpp>

/* C++-only REST helpers shared by the Satella modules (auth / sync / presence /
 * window). NOT part of the C ABI. Lives in namespace Satella alongside Client. */
namespace Satella {
/* Authenticated JSON POST to SATELLA_HTTP_HOST + path. Adds an
 * "Authorization: Bearer <token>" header when a token is stored. Returns true on
 * HTTP 200 with a parseable JSON body (written to outResp). */
bool PostJson(const std::string& path, const nlohmann::json& body, int& outStatus, nlohmann::json& outResp);

/* Authenticated JSON GET to SATELLA_HTTP_HOST + path. */
bool GetJson(const std::string& path, int& outStatus, nlohmann::json& outResp);

/* The HTTPS host for the Satella website API (parallel to the wss:// SATELLA_HOST
 * used by the WebSocket client). */
constexpr const char* kHttpHost = "https://satella.net64.dev";
constexpr const char* kWsHost   = "wss://satella.net64.dev";

/* Resolved Satella hosts. Overridable for LOCAL TESTING via the "Satella.Host"
 * config key (e.g. "http://localhost:8080"); empty → production. WsHost derives
 * the ws/wss scheme from the configured http(s) host. */
std::string HttpHost();
std::string WsHost();

/* The base URL of the Satella WEBSITE (where profiles live). Overridable for
 * local testing via "Satella.WebsiteUrl" config; empty → production. */
constexpr const char* kWebsiteUrl = "https://harbourmasters.org";
std::string WebsiteUrl();
}
#endif
