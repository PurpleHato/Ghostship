#ifdef USE_NETWORKING
#include "port/net/SatellaAuth.h"

#include "port/net/SatellaApi.h"
#include "ship/Context.h"
#include "ship/config/Config.h"
#include "spdlog/spdlog.h"

#include <nlohmann/json.hpp>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <shellapi.h>
#endif

#include <cstdlib>
#include <mutex>
#include <string>

namespace SatellaAuth {

namespace {
struct PendingLogin {
    std::string deviceCode;
    int interval = 5;
};
std::mutex gMtx;
PendingLogin gPending;
std::string gUserCode;
std::string gVerifyUri;
std::string gLastError;

void OpenBrowser(const std::string& url) {
#if defined(_WIN32)
    ShellExecuteW(nullptr, L"open", std::wstring(url.begin(), url.end()).c_str(), nullptr, nullptr, SW_SHOWNORMAL);
#elif defined(__APPLE__)
    system(("open '" + url + "'").c_str());
#else
    system(("xdg-open '" + url + "'").c_str());
#endif
}

std::string LoadToken() {
    auto config = Ship::Context::GetInstance()->GetConfig();
    if (!config) return "";
    return config->GetString("Satella.Token");
}

void SaveToken(const std::string& token) {
    auto config = Ship::Context::GetInstance()->GetConfig();
    if (!config) return;
    config->SetString("Satella.Token", token);
    config->Save();
}
} // namespace

bool IsAuthenticated() {
    return !LoadToken().empty();
}

std::string GetToken() {
    return LoadToken();
}

void Logout() {
    SaveToken("");
    std::lock_guard<std::mutex> lock(gMtx);
    gPending = PendingLogin{};
    gUserCode.clear();
    gVerifyUri.clear();
}

bool StartDeviceLogin(DeviceCode& code) {
    nlohmann::json body;
    body["clientId"] = "ghostship";

    int status = 0;
    nlohmann::json resp;
    if (!Satella::PostJson("/v1/auth/device/code", body, status, resp)) {
        std::lock_guard<std::mutex> lock(gMtx);
        gLastError = "device/code request failed (status " + std::to_string(status) + ")";
        return false;
    }

    DeviceCode parsed;
    parsed.userCode = resp.value("userCode", "");
    parsed.verificationUri = resp.value("verificationUri", "https://satella.net64.dev/link");
    parsed.expiresIn = resp.value("expiresIn", 600);
    parsed.interval = resp.value("interval", 5);

    {
        std::lock_guard<std::mutex> lock(gMtx);
        gPending.deviceCode = resp.value("deviceCode", "");
        gPending.interval = parsed.interval;
        gUserCode = parsed.userCode;
        gVerifyUri = parsed.verificationUri;
        gLastError.clear();
    }

    OpenBrowser(parsed.verificationUri);
    code = parsed;
    return true;
}

PollResult PollDeviceLogin() {
    std::string deviceCode;
    {
        std::lock_guard<std::mutex> lock(gMtx);
        deviceCode = gPending.deviceCode;
    }
    if (deviceCode.empty()) {
        return PollResult::Error;
    }

    nlohmann::json body;
    body["deviceCode"] = deviceCode;

    int status = 0;
    nlohmann::json resp;
    // PostJson returns false for any non-200 AND does not parse the body, so
    // `resp` is null here — classify by status code only.
    if (!Satella::PostJson("/v1/auth/device/verify", body, status, resp)) {
        if (status == 410) {
            std::lock_guard<std::mutex> lock(gMtx);
            gPending.deviceCode.clear();
            return PollResult::Expired;
        }
        // 400 ("pending") / 429 (slow down) — keep polling.
        return PollResult::Pending;
    }

    if (resp.value("status", "approved") == "expired") {
        std::lock_guard<std::mutex> lock(gMtx);
        gPending.deviceCode.clear();
        return PollResult::Expired;
    }
    if (resp.contains("accessToken") && resp["accessToken"].is_string()) {
        SaveToken(resp["accessToken"].get<std::string>());
        std::lock_guard<std::mutex> lock(gMtx);
        gPending.deviceCode.clear();
        gUserCode.clear();
        gVerifyUri.clear();
        return PollResult::Success;
    }
    return PollResult::Pending;
}

std::string GetLastError() {
    std::lock_guard<std::mutex> lock(gMtx);
    return gLastError;
}

} // namespace SatellaAuth
#endif
