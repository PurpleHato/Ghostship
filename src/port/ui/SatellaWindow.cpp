#ifdef USE_NETWORKING
#include "port/ui/SatellaWindow.h"

#include <libultraship/libultraship.h> // CVarGet/Set* + full Ship::Config definition
#include <fast/Fast3dGui.h>
#include <ship/window/gui/resource/GuiTexture.h>
#include <stb_image.h>
#include <ixwebsocket/IXHttpClient.h>
#include "port/ui/UIWidgets.hpp"
#include "port/ShipUtils.h"

#include "port/ui/AchievementsWindow.h" // DrawAchievementsGrid
#include "port/ui/Notification.h"
#include "port/ui/cvar_prefixes.h"
#include "port/net/SatellaAuth.h"
#include "port/net/SatellaApi.h"           // GetJson (/v1/user), HttpHost/WsHost
#include "port/net/SatellaAchievementSync.h"
#include "port/net/SatellaClient.h"        // RequestJson, leaderboard cache
#include "ship/Context.h"
#include "spdlog/spdlog.h"

#include <imgui.h>
#include <nlohmann/json.hpp>

#include <cctype>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <iomanip>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#if defined(_WIN32)
#include <windows.h>
#include <shellapi.h> // ShellExecuteW
#endif

namespace {

// ── Login (device-code) ──────────────────────────────────────────────────────
float sPollAccumulator = 0.0f;
struct LoginView {
    bool active = false;
    std::string userCode;
    std::string verifyUri;
    std::string error;
};
LoginView sLogin;

// ── Cached self profile (username) ───────────────────────────────────────────
struct SelfProfile {
    bool fetched = false;
    std::string username;
    std::string alias;
    std::string ulid;
    std::string avatar;
    int accentColor = 0;
    std::string role;
    int serverPlaytime = -1; // -1 = not yet fetched
    std::chrono::steady_clock::time_point lastFetchTime;
};
SelfProfile sSelf;

// ── Friends cache ──
struct FriendsCache {
    bool fetched = false;
    nlohmann::json friends;
};
FriendsCache sFriends;
static char sSearchBuf[64] = "";
static nlohmann::json sSearchResults;
static bool sSearched = false;

// ── Chat (ephemeral, relay-based) ────────────────────────────────────────────
struct ChatMessage { std::string from; std::string text; int64_t ts = 0; bool mine = false;
    std::string alias; int accentColor = 0; std::string avatarUrl; };
static std::map<std::string, std::vector<ChatMessage>> sChatBuffers;
static std::set<std::string> sSubscribedChannels;
static std::string sChatTargetId;
static std::string sChatTargetName;
static char sChatInput[256] = "";

std::string ChatChannelId(const std::string& a, const std::string& b) {
    return "chat:" + (a < b ? a + ":" + b : b + ":" + a);
}

// ── Avatar loading (async HTTP download → stb decode → GPU upload on render thread) ─
struct PendingAvatar {
    std::string cacheKey;
    std::vector<uint8_t> pixels;
    int width = 0;
    int height = 0;
};
static std::mutex sAvatarMtx;
static std::vector<PendingAvatar> sPendingAvatars;
static std::set<std::string> sLoadedAvatars;
static std::set<std::string> sLoadingAvatars;

void RequestAvatar(const std::string& url, const std::string& cacheKey) {
    if (url.empty() || sLoadedAvatars.count(cacheKey) || sLoadingAvatars.count(cacheKey)) return;
    sLoadingAvatars.insert(cacheKey);
    std::thread([url, cacheKey]() {
        ix::HttpClient http;
        auto args = http.createRequest(url, ix::HttpClient::kGet);
        args->connectTimeout = 10;
        args->transferTimeout = 10;
        auto resp = http.get(url, args);
        if (!resp || resp->statusCode != 200) {
            std::lock_guard<std::mutex> lock(sAvatarMtx);
            sLoadingAvatars.erase(cacheKey);
            return;
        }
        int w, h;
        stbi_uc* pixels = stbi_load_from_memory(
            reinterpret_cast<const stbi_uc*>(resp->body.data()),
            static_cast<int>(resp->body.size()), &w, &h, nullptr, 4);
        if (!pixels) {
            std::lock_guard<std::mutex> lock(sAvatarMtx);
            sLoadingAvatars.erase(cacheKey);
            return;
        }
        PendingAvatar pa;
        pa.cacheKey = cacheKey;
        pa.pixels.assign(pixels, pixels + (size_t)w * h * 4);
        pa.width = w;
        pa.height = h;
        stbi_image_free(pixels);
        {
            std::lock_guard<std::mutex> lock(sAvatarMtx);
            sPendingAvatars.push_back(std::move(pa));
            sLoadingAvatars.erase(cacheKey);
        }
    }).detach();
}

void UploadPendingAvatars() {
    std::vector<PendingAvatar> toUpload;
    {
        std::lock_guard<std::mutex> lock(sAvatarMtx);
        toUpload.swap(sPendingAvatars);
    }
    if (toUpload.empty()) return;
    auto gui = std::static_pointer_cast<Fast::Fast3dGui>(
        Ship::Context::GetInstance()->GetWindow()->GetGui());
    for (auto& av : toUpload) {
        auto tex = std::make_shared<Ship::GuiTexture>();
        tex->Data = av.pixels.data();
        tex->DataSize = av.pixels.size();
        tex->Metadata.Width = av.width;
        tex->Metadata.Height = av.height;
        gui->LoadTextureFromResource(av.cacheKey, tex);
        tex->Data = nullptr; // Prevent ~GuiTexture from stbi_image_free-ing vector memory
        sLoadedAvatars.insert(av.cacheKey);
    }
}

// Render an avatar image by cache key, or a placeholder box of the given size.
void DrawAvatar(const std::string& cacheKey, float size) {
    auto gui = std::static_pointer_cast<Fast::Fast3dGui>(
        Ship::Context::GetInstance()->GetWindow()->GetGui());
    if (gui->HasTextureByName(cacheKey)) {
        ImGui::Image(gui->GetTextureByName(cacheKey), ImVec2(size, size));
    } else {
        ImGui::Dummy(ImVec2(size, size));
    }
}

bool HasAvatar(const std::string& cacheKey) {
    auto gui = std::static_pointer_cast<Fast::Fast3dGui>(
        Ship::Context::GetInstance()->GetWindow()->GetGui());
    return gui->HasTextureByName(cacheKey);
}

// ── Badges cache (WS badges/state) ───────────────────────────────────────────
struct BadgesCache {
    bool fetched = false;
    nlohmann::json trialDefs;   // array of {id,name,courseId,thresholds}
    nlohmann::json trialBadges; // array of {badgeId,rank,bestTime}
    nlohmann::json special;     // array of {badgeId, number?, badge:{name}}
};
BadgesCache sBadges;

// ── Leaderboard ──────────────────────────────────────────────────────────────
constexpr const char* kCourses[] = { "BOB", "THI", "CCM_PENGUIN", "PSS" };
int sLbCourse = 0;
std::string sLbFetchedCourse; // course we've already done the initial WS fetch for

// ── Playtime ─────────────────────────────────────────────────────────────────
const std::chrono::steady_clock::time_point sSessionStart = std::chrono::steady_clock::now();
float sPlaytimeSaveAccumulator = 0.0f;

// ── Helpers ──────────────────────────────────────────────────────────────────

void OpenInBrowser(const std::string& url) {
#if defined(_WIN32)
    ShellExecuteW(nullptr, L"open", std::wstring(url.begin(), url.end()).c_str(), nullptr, nullptr,
                  SW_SHOWNORMAL);
#elif defined(__APPLE__)
    system(("open '" + url + "'").c_str());
#else
    system(("xdg-open '" + url + "'").c_str());
#endif
}

std::string FormatDuration(float seconds) {
    const int total = static_cast<int>(seconds);
    const int h = total / 3600;
    const int m = (total % 3600) / 60;
    const int s = total % 60;
    char buf[32];
    if (h > 0) {
        snprintf(buf, sizeof(buf), "%dh %02dm", h, m);
    } else if (m > 0) {
        snprintf(buf, sizeof(buf), "%dm %02ds", m, s);
    } else {
        snprintf(buf, sizeof(buf), "%ds", s);
    }
    return buf;
}

std::string FormatRaceTime(int64_t ms) {
    if (ms <= 0) {
        return "--:--.-";
    }
    const int totalTenths = static_cast<int>(ms / 100);
    const int tenths = totalTenths % 10;
    const int totalSeconds = totalTenths / 10;
    const int seconds = totalSeconds % 60;
    const int minutes = totalSeconds / 60;
    char buf[32];
    snprintf(buf, sizeof(buf), "%d:%02d.%d", minutes, seconds, tenths);
    return buf;
}

// Blocking WS JSON request on the authed socket.
bool WsRequest(const std::string& route, const nlohmann::json& body, nlohmann::json& out) {
    int16_t status = 0;
    std::string resp;
    if (!Satella::Client::Instance().RequestJson(route, body.dump(), status, resp) || status != 200) {
        return false;
    }
    try {
        out = resp.empty() ? nlohmann::json::object() : nlohmann::json::parse(resp);
    } catch (...) {
        out = nlohmann::json::object();
    }
    return true;
}

void FetchSelfProfile() {
    int status = 0;
    nlohmann::json p;
    if (Satella::GetJson("/v1/user", status, p)) {
        sSelf.username = p.value("username", p.value("alias", ""));
        sSelf.alias = p.value("alias", "");
        sSelf.ulid = p.value("ulid", "");
        sSelf.avatar = p.value("avatar", "");
        sSelf.accentColor = p.value("accentColor", 0);
        sSelf.role = p.value("role", "user");
    }
    sSelf.fetched = true;
    sSelf.lastFetchTime = std::chrono::steady_clock::now();
}

void ResetCaches() {
    sSelf.fetched = false;
    sSelf.username.clear();
    sBadges.fetched = false;
    sFriends.fetched = false;
    sSearched = false;
    sLbFetchedCourse.clear();
    sSelf.serverPlaytime = -1;
}

// ── Tabs ─────────────────────────────────────────────────────────────────────

void DrawAccountTab() {
    // ── Authed: identity card ──
    if (SatellaAuth::IsAuthenticated()) {
        if (!sSelf.fetched) {
            FetchSelfProfile();
        }

        // Avatar: real image if loaded, else accent-color circle with initial.
        const std::string selfAvatarKey = "avatar_" + sSelf.ulid;
        if (HasAvatar(selfAvatarKey)) {
            DrawAvatar(selfAvatarKey, 64.0f);
        } else {
            const char initial = sSelf.username.empty() ? '?' :
                static_cast<char>(std::toupper(static_cast<unsigned char>(sSelf.username[0])));
            const ImVec4 accent(
                ((sSelf.accentColor >> 16) & 0xFF) / 255.0f,
                ((sSelf.accentColor >> 8) & 0xFF) / 255.0f,
                (sSelf.accentColor & 0xFF) / 255.0f,
                1.0f
            );
            ImGui::PushStyleColor(ImGuiCol_Button, accent);
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1, 1, 1, 1));
            ImGui::Button(std::string(1, initial).c_str(), ImVec2(64, 64));
            ImGui::PopStyleColor(2);
        }

        ImGui::SameLine();
        ImGui::BeginGroup();
        ImGui::PushFont(ImGui::GetIO().Fonts->Fonts.Size > 1 ? ImGui::GetIO().Fonts->Fonts[1] : nullptr);
        ImGui::Text("%s", (sSelf.alias.empty() ? sSelf.username : sSelf.alias).c_str());
        ImGui::PopFont();
        if (!sSelf.alias.empty() && sSelf.alias != sSelf.username) {
            ImGui::TextDisabled("@%s", sSelf.alias.c_str());
        }
        ImGui::EndGroup();

        ImGui::Spacing();
        if (!sSelf.ulid.empty()) {
            ImGui::TextDisabled("ULID: %s", sSelf.ulid.c_str());
        }
        if (!sSelf.role.empty() && sSelf.role != "user") {
            ImGui::Text("Role: %s", sSelf.role.c_str());
        }

        ImGui::Spacing();
        if (UIWidgets::Button("Open Web Profile", UIWidgets::ButtonOptions{}.Color(WIDGET_COLOR).Size(ImVec2(0, 0))) && !sSelf.username.empty()) {
            OpenInBrowser(Satella::WebsiteUrl() + "/satella/profile/" + sSelf.username);
        }
        ImGui::SameLine();
        if (UIWidgets::Button("Sign out", UIWidgets::ButtonOptions{}.Color(UIWidgets::Colors::Red))) {
            SatellaAuth::Logout();
            ResetCaches();
        }
    } else {
        // ── Not authed: What is Satella? + login ──
        if (!sLogin.active) {
            ImGui::TextWrapped("Satella connects Ghostship to the Harbour Masters community.");
            ImGui::Spacing();
            ImGui::TextWrapped("- Sync achievements, badges, and race times to your profile");
            ImGui::TextWrapped("- See live leaderboards and race against the clock");
            ImGui::TextWrapped("- Add friends and chat across games and the website");
            ImGui::Spacing();

            if (UIWidgets::Button("Sign in with Discord", UIWidgets::ButtonOptions{}.Color(WIDGET_COLOR))) {
                SatellaAuth::DeviceCode code;
                if (SatellaAuth::StartDeviceLogin(code)) {
                    sLogin.active = true;
                    sLogin.verifyUri = code.verificationUri;
                    sLogin.error.clear();
                } else {
                    sLogin.error = SatellaAuth::GetLastError();
                    if (sLogin.error.empty()) {
                        sLogin.error = "Could not start login. Is the server reachable?";
                    }
                }
            }
            if (!sLogin.error.empty()) {
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.4f, 0.4f, 1.0f));
                ImGui::TextWrapped("%s", sLogin.error.c_str());
                ImGui::PopStyleColor();
            }
        } else {
            ImGui::TextWrapped("Waiting for approval...");
            ImGui::TextDisabled("Your browser should have opened automatically.");
            ImGui::Spacing();
            if (UIWidgets::Button("Cancel", UIWidgets::ButtonOptions{}.Color(UIWidgets::Colors::Gray))) {
                SatellaAuth::Logout();
                sLogin.active = false;
            }
        }
    }

    // ── Developer settings (server + website URL overrides) ──
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();
    ImGui::TextDisabled("Developer Settings");

    bool useSatella = CVarGetInteger(CVAR_DEVELOPER_TOOLS("Satella"), 1) != 0;
    UIWidgets::CVarCheckbox("Use Satella", CVAR_DEVELOPER_TOOLS("Satella"));

    // Server (API) override.
    static char hostBuf[160] = "";
    static char webBuf[160] = "";
    static bool devLoaded = false;
    if (!devLoaded) {
        auto config = Ship::Context::GetInstance()->GetConfig();
        if (config) {
            snprintf(hostBuf, sizeof(hostBuf), "%s", config->GetString("Satella.Host").c_str());
            snprintf(webBuf, sizeof(webBuf), "%s", config->GetString("Satella.WebsiteUrl").c_str());
        }
        devLoaded = true;
    }
    UIWidgets::PushStyleInput(WIDGET_COLOR);
    ImGui::PushItemWidth(220);
    ImGui::InputTextWithHint("Server", "https://satella.net64.dev", hostBuf, sizeof(hostBuf));
    ImGui::PopItemWidth();
    UIWidgets::PopStyleInput();
    ImGui::SameLine();
    if (UIWidgets::Button("Save##host", UIWidgets::ButtonOptions{}.Color(WIDGET_COLOR).Size(UIWidgets::Sizes::Inline))) {
        auto config = Ship::Context::GetInstance()->GetConfig();
        if (config) { config->SetString("Satella.Host", hostBuf); config->Save(); }
    }
    UIWidgets::PushStyleInput(WIDGET_COLOR);
    ImGui::PushItemWidth(220);
    ImGui::InputTextWithHint("Website", "https://harbourmasters.org", webBuf, sizeof(webBuf));
    ImGui::PopItemWidth();
    UIWidgets::PopStyleInput();
    ImGui::SameLine();
    if (UIWidgets::Button("Save##web", UIWidgets::ButtonOptions{}.Color(WIDGET_COLOR).Size(UIWidgets::Sizes::Inline))) {
        auto config = Ship::Context::GetInstance()->GetConfig();
        if (config) { config->SetString("Satella.WebsiteUrl", webBuf); config->Save(); }
    }
    ImGui::TextDisabled("Restart to apply server/website changes.");
}

void DrawAchievementsTab() {
    DrawAchievementsGrid();
}

const char* RankName(int rank) {
    switch (rank) {
        case 1: return "Bronze";
        case 2: return "Silver";
        case 3: return "Gold";
        case 4: return "Platinum";
        case 5: return "Diamond";
        default: return "Locked";
    }
}

void DrawBadgesTab() {
    if (!SatellaAuth::IsAuthenticated()) {
        ImGui::TextWrapped("Connect to Satella to view your badges.");
        return;
    }

    if (!sBadges.fetched) {
        nlohmann::json body;
        body["gameId"] = "ghostship";
        nlohmann::json resp;
        if (WsRequest("/v1/satella/badges/state", body, resp)) {
            sBadges.trialDefs = resp.value("trialDefs", nlohmann::json::array());
            sBadges.trialBadges = resp.value("trialBadges", nlohmann::json::array());
            sBadges.special = resp.value("special", nlohmann::json::array());
            sBadges.fetched = true;
        } else {
            ImGui::TextWrapped("Could not load badges.");
            return;
        }
    }

    if (UIWidgets::Button("Refresh", UIWidgets::ButtonOptions{}.Color(WIDGET_COLOR))) {
        sBadges.fetched = false;
    }

    ImGui::Spacing();
    ImGui::TextDisabled("Race Medals");
    ImGui::Separator();
    if (sBadges.trialDefs.is_array()) {
        for (const auto& def : sBadges.trialDefs) {
            const std::string id = def.value("id", def.value("badgeId", "?"));
            const std::string name = def.value("name", id);
            int rank = 0;
            if (sBadges.trialBadges.is_array()) {
                for (const auto& tb : sBadges.trialBadges) {
                    if (tb.value("badgeId", "") == id) {
                        rank = tb.value("rank", 0);
                        break;
                    }
                }
            }
            ImGui::Text("%s — %s", name.c_str(), RankName(rank));
        }
    }

    if (sBadges.special.is_array() && !sBadges.special.empty()) {
        ImGui::Spacing();
        ImGui::TextDisabled("Prestige");
        ImGui::Separator();
        for (const auto& sp : sBadges.special) {
            const std::string name =
                sp.contains("badge") && sp["badge"].is_object()
                    ? sp["badge"].value("name", std::string("Special"))
                    : "Special";
            const int number = sp.value("number", -1);
            if (number >= 0) {
                ImGui::Text("#%d  %s", number, name.c_str());
            } else {
                ImGui::Text("%s", name.c_str());
            }
        }
    }
}

void DrawProfileTab() {
    if (!SatellaAuth::IsAuthenticated()) {
        ImGui::TextWrapped("Connect to Satella to view your profile.");
        return;
    }
    if (!sSelf.fetched) {
        FetchSelfProfile();
    }

    ImGui::Text("Signed in as: %s", sSelf.username.empty() ? "(unknown)" : sSelf.username.c_str());

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    // Current session (since the game launched).
    const float sessionSeconds =
        std::chrono::duration<float>(std::chrono::steady_clock::now() - sSessionStart).count();
    ImGui::Text("This session: %s", FormatDuration(sessionSeconds).c_str());

    // Total time played — fetch from server (authoritative, from PlaySession records).
    if (sSelf.serverPlaytime < 0 && !sSelf.ulid.empty()) {
        int st = 0;
        nlohmann::json statsResp;
        if (Satella::GetJson("/v1/stats/user/" + sSelf.ulid, st, statsResp)) {
            sSelf.serverPlaytime = statsResp.value("playtimeSeconds", 0);
        } else {
            sSelf.serverPlaytime = 0; // don't retry every frame
        }
    }
    ImGui::Text("Total played: %s", FormatDuration(static_cast<float>(sSelf.serverPlaytime)).c_str());

    ImGui::Spacing();
    if (UIWidgets::Button("Open Web Profile", UIWidgets::ButtonOptions{}.Color(WIDGET_COLOR)) && !sSelf.username.empty()) {
        OpenInBrowser(Satella::WebsiteUrl() + "/satella/profile/" + sSelf.username);
    }
}

void DrawLeaderboardTab() {
    if (!SatellaAuth::IsAuthenticated()) {
        ImGui::TextWrapped("Connect to Satella to view the leaderboard.");
        return;
    }

    ImGui::Combo("Course", &sLbCourse, kCourses, 4);
    const std::string courseId = kCourses[sLbCourse];

    // Initial fetch whenever the selected course changes (the live push keeps it
    // fresh afterwards via the notifications subscription → SetCachedLeaderboard).
    if (sLbFetchedCourse != courseId) {
        sLbFetchedCourse = courseId;
        nlohmann::json body;
        body["courseId"] = courseId;
        nlohmann::json resp;
        if (WsRequest("/v1/satella/leaderboard/get", body, resp) && resp.contains("entries") &&
            resp["entries"].is_array()) {
            std::vector<Satella::LeaderboardRow> rows;
            for (const auto& e : resp["entries"]) {
                Satella::LeaderboardRow row;
                row.username = e.value("username", e.value("alias", "?"));
                row.timeMs = e.value("time", static_cast<int64_t>(0));
                rows.push_back(std::move(row));
            }
            Satella::SetCachedLeaderboard(courseId, std::move(rows));
        }
    }

    const auto* rows = Satella::GetCachedLeaderboard(courseId);
    if (rows == nullptr || rows->empty()) {
        ImGui::TextWrapped("No times recorded yet. Be the first!");
        return;
    }
    int rank = 1;
    for (const auto& row : *rows) {
        ImGui::Text("%2d. %-16s  %s", rank++, row.username.c_str(),
                    FormatRaceTime(row.timeMs).c_str());
    }
}

// ── Friends ── (cache declarations are at the top of the anon namespace)

std::string FriendId(const nlohmann::json& f) {
    return f.value("userId", f.value("ulid", ""));
}

void DrawFriendsTab() {
    if (!SatellaAuth::IsAuthenticated()) {
        ImGui::TextWrapped("Sign in to manage friends.");
        return;
    }

    // ── Conversation mode ── (when a chat target is selected)
    if (!sChatTargetId.empty()) {
        if (UIWidgets::Button("\xef\x81\xa0 Back", UIWidgets::ButtonOptions{}.Color(WIDGET_COLOR).Size(UIWidgets::Sizes::Inline))) {
            sChatTargetId.clear();
            sChatTargetName.clear();
            return;
        }
        ImGui::Spacing();
        // Conversation header with bigger avatar.
        DrawAvatar("avatar_" + sChatTargetId, 32.0f);
        ImGui::SameLine();
        ImGui::BeginGroup();
        ImGui::PushFont(ImGui::GetIO().Fonts->Fonts.Size > 1 ? ImGui::GetIO().Fonts->Fonts[1] : nullptr);
        ImGui::Text("%s", sChatTargetName.c_str());
        ImGui::PopFont();
        ImGui::EndGroup();
        ImGui::Separator();

        const std::string chId = ChatChannelId(sSelf.ulid, sChatTargetId);

        // Get or create the message buffer for this conversation.
        auto& buf = sChatBuffers[chId];
        const std::string myAvatarKey = "avatar_" + sSelf.ulid;

        // Message scroll area — each message has an avatar next to it.
        const float footer = ImGui::GetFrameHeightWithSpacing() + ImGui::GetStyle().ItemSpacing.y * 2;
        ImGui::BeginChild("ChatScroll", ImVec2(0, -footer), false);
        for (const auto& msg : buf) {
            if (msg.mine) {
                // Right-aligned: text then avatar.
                const float avatarSz = 20.0f;
                const float availW = ImGui::GetContentRegionAvail().x;
                const float textW = availW - avatarSz - ImGui::GetStyle().ItemSpacing.x;
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.4f, 0.8f, 0.9f, 1));
                ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + textW);
                ImGui::TextWrapped("%s", msg.text.c_str());
                ImGui::PopTextWrapPos();
                ImGui::PopStyleColor();
                ImGui::SameLine();
                DrawAvatar(myAvatarKey, avatarSz);
            } else {
                // Left-aligned: avatar then sender alias + text.
                const float avatarSz = 20.0f;
                DrawAvatar("avatar_" + sChatTargetId, avatarSz);
                ImGui::SameLine();
                const float availW = ImGui::GetContentRegionAvail().x;
                // Show the sender's alias above the message in their accent color.
                if (!msg.alias.empty()) {
                    const ImVec4 accent = (msg.accentColor != 0)
                        ? ImVec4(((msg.accentColor >> 16) & 0xFF) / 255.0f,
                                 ((msg.accentColor >> 8) & 0xFF) / 255.0f,
                                 (msg.accentColor & 0xFF) / 255.0f, 1.0f)
                        : ImVec4(1.0f, 0.85f, 0.0f, 1.0f); // gold fallback
                    ImGui::PushStyleColor(ImGuiCol_Text, accent);
                    ImGui::Text("%s", msg.alias.c_str());
                    ImGui::PopStyleColor();
                }
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.85f, 0.85f, 0.85f, 1));
                ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + availW);
                ImGui::TextWrapped("%s", msg.text.c_str());
                ImGui::PopTextWrapPos();
                ImGui::PopStyleColor();
            }
            ImGui::Spacing();
        }
        if (!buf.empty()) {
            ImGui::SetScrollHereY(1.0f);
        }
        ImGui::EndChild();

        ImGui::Spacing();
        // Input bar — themed.
        UIWidgets::PushStyleInput(WIDGET_COLOR);
        const bool enterPressed = ImGui::InputTextWithHint("##chatinput", "Type a message...",
                                                           sChatInput, sizeof(sChatInput),
                                                           ImGuiInputTextFlags_EnterReturnsTrue);
        UIWidgets::PopStyleInput();
        ImGui::SameLine();
        if ((UIWidgets::Button("Send", UIWidgets::ButtonOptions{}.Color(WIDGET_COLOR).Size(UIWidgets::Sizes::Inline)) || enterPressed) && sChatInput[0] != '\0') {
            nlohmann::json msg;
            msg["from"] = sSelf.ulid;
            msg["text"] = std::string(sChatInput);
            msg["channelId"] = chId;
            msg["ts"] = static_cast<int64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                          std::chrono::system_clock::now().time_since_epoch()).count());
            // Enrich with sender identity so the receiver can render the
            // notification with alias + accent color + avatar without a lookup.
            msg["alias"] = sSelf.alias.empty() ? sSelf.username : sSelf.alias;
            msg["accentColor"] = sSelf.accentColor;
            msg["avatarUrl"] = sSelf.avatar;
            const std::string payload = msg.dump();
            Satella::Client::Instance().SendRaw(
                "/v1/relay/" + chId + "/send",
                reinterpret_cast<const uint8_t*>(payload.data()), payload.size());
            // Optimistically append locally.
            sChatBuffers[chId].push_back({
                sSelf.ulid, std::string(sChatInput), msg["ts"].get<int64_t>(), true,
                sSelf.alias.empty() ? sSelf.username : sSelf.alias,
                sSelf.accentColor, sSelf.avatar
            });
            sChatInput[0] = '\0';
        }
        return;
    }

    // ── Friend list mode ──

    // Auto-refresh every 15s so presence/online status stays fresh without
    // the user manually pressing Refresh.
    static float sFriendsRefreshAcc = 0.0f;
    sFriendsRefreshAcc += ImGui::GetIO().DeltaTime;
    if (sFriendsRefreshAcc > 15.0f) {
        sFriendsRefreshAcc = 0.0f;
        sFriends.fetched = false;
    }

    if (!sFriends.fetched) {
        nlohmann::json resp;
        if (WsRequest("/v1/satella/friends/list", nlohmann::json::object(), resp)) {
            sFriends.friends = resp.value("friends", nlohmann::json::array());
        }
        sFriends.fetched = true;
    }

    if (UIWidgets::Button("Refresh", UIWidgets::ButtonOptions{}.Color(WIDGET_COLOR))) {
        sFriends.fetched = false;
    }
    ImGui::Spacing();

    if (!sFriends.friends.is_array()) return;

    // Incoming requests
    bool hasHeader = false;
    for (const auto& f : sFriends.friends) {
        if (!f.value("isPending", false) || f.value("isRequester", false)) continue;
        if (!hasHeader) { ImGui::TextDisabled("Incoming Requests"); ImGui::Separator(); hasHeader = true; }
        const std::string name = f.value("displayName", f.value("alias", f.value("username", "?")));
        const std::string id = FriendId(f);
        ImGui::Text("%s", name.c_str());
        ImGui::SameLine(ImGui::GetWindowWidth() - 130);
        if (ImGui::Button(("Accept##" + id).c_str())) {
            nlohmann::json b; b["friendId"] = id; b["accept"] = true;
            nlohmann::json r; WsRequest("/v1/satella/friends/modify", b, r);
            sFriends.fetched = false;
        }
        ImGui::SameLine();
        if (ImGui::Button(("Decline##" + id).c_str())) {
            nlohmann::json b; b["friendId"] = id; b["accept"] = false;
            nlohmann::json r; WsRequest("/v1/satella/friends/modify", b, r);
            sFriends.fetched = false;
        }
    }

    // Online friends
    hasHeader = false;
    for (const auto& f : sFriends.friends) {
        if (!f.value("isAccepted", false)) continue;
        if (f.value("onlineStatus", "offline") == "offline") continue;
        if (!hasHeader) { ImGui::Spacing(); ImGui::TextDisabled("Online"); ImGui::Separator(); hasHeader = true; }

        const std::string name = f.value("displayName", f.value("alias", f.value("username", "?")));
        const std::string status = f.value("onlineStatus", "offline");
        const std::string fid = FriendId(f);
        // Avatar
        RequestAvatar(f.value("avatar", ""), "avatar_" + fid);
        DrawAvatar("avatar_" + fid, 32.0f);
        ImGui::SameLine();
        const ImVec4 dot = (status == "ingame") ? ImVec4(0.35f, 0.4f, 0.95f, 1.0f)
                                                 : ImVec4(0.2f, 0.8f, 0.35f, 1.0f);
        ImGui::PushStyleColor(ImGuiCol_Text, dot);
        ImGui::TextUnformatted("\xe2\x97\x8f"); // ●
        ImGui::PopStyleColor();
        ImGui::SameLine();
        ImGui::TextUnformatted(name.c_str());
        // Chat button.
        {
            ImGui::SameLine(ImGui::GetWindowWidth() - 55);
            if (ImGui::Button(("Chat##on" + fid).c_str())) {
                sChatTargetId = fid;
                sChatTargetName = name;
                const std::string chId = ChatChannelId(sSelf.ulid, fid);
                if (sSubscribedChannels.find(chId) == sSubscribedChannels.end() && !sSelf.ulid.empty()) {
                    int16_t st = 0; std::string r;
                    Satella::Client::Instance().RequestJson("/v1/relay/" + chId + "/subscribe", "{}", st, r);
                    sSubscribedChannels.insert(chId);
                }
            }
        }

        if (status == "ingame" && f.contains("presence") && f["presence"].is_object()) {
            std::string game = f["presence"].value("game", "a game");
            if (!game.empty()) game[0] = std::toupper(static_cast<unsigned char>(game[0]));
            // Compute session duration from sessionStartedAt.
            std::string sessionStr = f["presence"].value("sessionStartedAt", "");
            if (!sessionStr.empty()) {
                // Parse ISO 8601 → elapsed seconds (rough — uses system_clock).
                std::tm tm = {};
                std::istringstream ss(sessionStr);
                ss >> std::get_time(&tm, "%Y-%m-%dT%H:%M:%S");
                if (ss.good() || ss.eof()) {
                    const auto start = std::chrono::system_clock::from_time_t(std::mktime(&tm));
                    const auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
                        std::chrono::system_clock::now() - start).count();
                    const int secs = static_cast<int>(elapsed);
                    const int h = secs / 3600;
                    const int m = (secs % 3600) / 60;
                    const int s = secs % 60;
                    char timeBuf[16];
                    if (h > 0) snprintf(timeBuf, sizeof(timeBuf), "%d:%02d:%02d", h, m, s);
                    else snprintf(timeBuf, sizeof(timeBuf), "%02d:%02d", m, s);
                    ImGui::TextDisabled("  Playing: %s for %s", game.c_str(), timeBuf);
                } else {
                    ImGui::TextDisabled("  Playing: %s", game.c_str());
                }
            } else {
                ImGui::TextDisabled("  Playing: %s", game.c_str());
            }
        } else {
            ImGui::TextDisabled("  Online");
        }
    }

    // Offline friends
    hasHeader = false;
    for (const auto& f : sFriends.friends) {
        if (!f.value("isAccepted", false)) continue;
        if (f.value("onlineStatus", "offline") != "offline") continue;
        if (!hasHeader) { ImGui::Spacing(); ImGui::TextDisabled("Offline"); ImGui::Separator(); hasHeader = true; }

        const std::string name = f.value("displayName", f.value("alias", f.value("username", "?")));
        const std::string fid = FriendId(f);
        // Avatar
        RequestAvatar(f.value("avatar", ""), "avatar_" + fid);
        DrawAvatar("avatar_" + fid, 32.0f);
        ImGui::SameLine();
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.4f, 0.4f, 0.4f, 1.0f));
        ImGui::TextUnformatted("\xe2\x97\x8f");
        ImGui::PopStyleColor();
        ImGui::SameLine();
        ImGui::TextDisabled("%s", name.c_str());
        // Chat button.
        {
            ImGui::SameLine(ImGui::GetWindowWidth() - 55);
            if (ImGui::Button(("Chat##off" + fid).c_str())) {
                sChatTargetId = fid;
                sChatTargetName = name;
                const std::string chId = ChatChannelId(sSelf.ulid, fid);
                if (sSubscribedChannels.find(chId) == sSubscribedChannels.end() && !sSelf.ulid.empty()) {
                    int16_t st = 0; std::string r;
                    Satella::Client::Instance().RequestJson("/v1/relay/" + chId + "/subscribe", "{}", st, r);
                    sSubscribedChannels.insert(chId);
                }
            }
        }
    }

    // Outgoing requests
    hasHeader = false;
    for (const auto& f : sFriends.friends) {
        if (!f.value("isPending", false) || !f.value("isRequester", false)) continue;
        if (!hasHeader) { ImGui::Spacing(); ImGui::TextDisabled("Sent Requests"); ImGui::Separator(); hasHeader = true; }

        const std::string name = f.value("displayName", f.value("alias", f.value("username", "?")));
        const std::string id = FriendId(f);
        ImGui::TextDisabled("%s (pending)", name.c_str());
        ImGui::SameLine(ImGui::GetWindowWidth() - 60);
        if (ImGui::Button(("Cancel##" + id).c_str())) {
            nlohmann::json b; b["friendId"] = id;
            nlohmann::json r; WsRequest("/v1/satella/friends/remove", b, r);
            sFriends.fetched = false;
        }
    }

    // Add friend
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();
    ImGui::TextDisabled("Add Friend");
    UIWidgets::PushStyleInput(WIDGET_COLOR);
    const bool enterPressed = ImGui::InputTextWithHint("##fs", "Search username...",
                                                       sSearchBuf, sizeof(sSearchBuf),
                                                       ImGuiInputTextFlags_EnterReturnsTrue);
    UIWidgets::PopStyleInput();
    ImGui::SameLine();
    if (UIWidgets::Button("Go", UIWidgets::ButtonOptions{}.Color(WIDGET_COLOR).Size(UIWidgets::Sizes::Inline)) || enterPressed) {
        if (sSearchBuf[0] != '\0') {
            nlohmann::json b; b["q"] = std::string(sSearchBuf);
            nlohmann::json resp;
            if (WsRequest("/v1/satella/friends/search", b, resp)) {
                sSearchResults = resp.value("results", nlohmann::json::array());
            }
            sSearched = true;
        }
    }

    if (sSearched) {
        ImGui::Spacing();
        if (!sSearchResults.is_array() || sSearchResults.empty()) {
            ImGui::TextDisabled("No users found.");
        }
        for (const auto& u : sSearchResults) {
            const std::string name = u.value("username", u.value("alias", "?"));
            const std::string id = FriendId(u);
            ImGui::BulletText("%s", name.c_str());
            ImGui::SameLine(ImGui::GetWindowWidth() - 60);
            if (ImGui::Button(("Add##" + id).c_str())) {
                nlohmann::json b; b["friendId"] = id;
                nlohmann::json r; WsRequest("/v1/satella/friends/add", b, r);
                sFriends.fetched = false;
                sSearched = false;
                sSearchBuf[0] = '\0';
            }
        }
    }
}

} // namespace

// ── Extern panel wrappers (declared in SatellaWindow.h) ──────────────────────
// Called from the menu sidebar PreFunc to draw content inline, or from the
// standalone SatellaWindow popup.
void DrawSatellaConnect() { DrawAccountTab(); }
void DrawSatellaAchievements() { DrawAchievementsTab(); }
void DrawSatellaBadges() { DrawBadgesTab(); }
void DrawSatellaProfile() { DrawProfileTab(); }
void DrawSatellaLeaderboard() { DrawLeaderboardTab(); }
void DrawSatellaFriends() { DrawFriendsTab(); }

// Public wrappers for the avatar system (used by SatellaClient.cpp).
void SatellaRequestAvatar(const std::string& url, const std::string& cacheKey) {
    RequestAvatar(url, cacheKey);
}
bool SatellaHasAvatar(const std::string& cacheKey) {
    return HasAvatar(cacheKey);
}

void SatellaFetchSelfProfile() {
    FetchSelfProfile();
}

void SatellaWindow::InitElement() {
}

void SatellaWindow::UpdateElement() {
    // ── Drain incoming chat messages into the per-conversation buffers ──
    const auto incoming = Satella::DrainIncomingChat();
    for (const auto& msg : incoming) {
        const bool mine = msg.from == sSelf.ulid;
        sChatBuffers[msg.channelId].push_back({
            msg.from, msg.text, msg.ts, mine,
            mine ? (sSelf.alias.empty() ? sSelf.username : sSelf.alias) : msg.alias,
            mine ? sSelf.accentColor : msg.accentColor,
            mine ? sSelf.avatar : msg.avatarUrl,
        });
        // Pre-cache the sender's avatar for future notifications.
        if (!mine && !msg.avatarUrl.empty()) {
            RequestAvatar(msg.avatarUrl, "avatar_" + msg.from);
        }
    }

    // ── Periodically allow profile re-fetch (alias/avatar changes propagate) ──
    if (sSelf.fetched &&
        std::chrono::steady_clock::now() - sSelf.lastFetchTime > std::chrono::seconds(60)) {
        sSelf.fetched = false; // next Account/Profile tab render will re-fetch
    }

    // ── Upload any avatars downloaded on background threads ──
    UploadPendingAvatars();

    // ── Request self avatar if not yet loading/loaded ──
    if (!sSelf.avatar.empty() && !sSelf.ulid.empty()) {
        RequestAvatar(sSelf.avatar, "avatar_" + sSelf.ulid);
    }

    // Playtime is now tracked server-side via PlaySession records (authoritative).
    // The local CVAR accumulator was removed — the Profile tab fetches the
    // server's total via GET /v1/stats/user/:ulid.
    const float dt = ImGui::GetIO().DeltaTime;

    // ── Device-code login polling ──
    if (!sLogin.active) {
        return;
    }
    sPollAccumulator += dt;
    if (sPollAccumulator < 5.0f) {
        return;
    }
    sPollAccumulator = 0.0f;

    const auto result = SatellaAuth::PollDeviceLogin();
    if (result == SatellaAuth::PollResult::Success) {
        sLogin.active = false;
        sLogin.userCode.clear();
        sLogin.verifyUri.clear();
        SPDLOG_INFO("Satella: authenticated successfully");
        // Fetch the username, then fire the login toast.
        FetchSelfProfile();
        Notification::Options opts;
        opts.prefix = "Satella";
        opts.prefixColor = ImVec4(1.0f, 0.85f, 0.0f, 1.0f); // gold
        opts.message = sSelf.username.empty() ? "Logged in" : ("Logged in as " + sSelf.username);
        opts.messageColor = ImVec4(1.0f, 1.0f, 1.0f, 1.0f); // white
        opts.isAchievement = true; // gold-border enhanced card
        Notification::Emit(opts);
    } else if (result == SatellaAuth::PollResult::Expired || result == SatellaAuth::PollResult::Error) {
        sLogin.active = false;
        sLogin.error = "Login expired or failed. Please try again.";
    }
}

void SatellaWindow::DrawElement() {
    // Read a tab-selection request from the sidebar navigation buttons.
    const int requestedTab = CVarGetInteger("gSettings.SatellaInitialTab", -1);
    if (requestedTab >= 0) {
        CVarSetInteger("gSettings.SatellaInitialTab", -1); // consume
    }

    if (ImGui::BeginTabBar("SatellaTabs")) {
        if (ImGui::BeginTabItem("Account", nullptr,
                                requestedTab == 0 ? ImGuiTabItemFlags_SetSelected : 0)) {
            DrawAccountTab();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Achievements", nullptr,
                                requestedTab == 1 ? ImGuiTabItemFlags_SetSelected : 0)) {
            DrawAchievementsTab();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Badges", nullptr,
                                requestedTab == 2 ? ImGuiTabItemFlags_SetSelected : 0)) {
            DrawBadgesTab();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Profile", nullptr,
                                requestedTab == 3 ? ImGuiTabItemFlags_SetSelected : 0)) {
            DrawProfileTab();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Leaderboard", nullptr,
                                requestedTab == 4 ? ImGuiTabItemFlags_SetSelected : 0)) {
            DrawLeaderboardTab();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Friends", nullptr,
                                requestedTab == 5 ? ImGuiTabItemFlags_SetSelected : 0)) {
            DrawFriendsTab();
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
}
#endif
