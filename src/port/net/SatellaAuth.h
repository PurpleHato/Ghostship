#pragma once

#ifdef USE_NETWORKING
#include <string>

/* Device-code (RFC 8628) Discord login for the Satella website.
 *
 * Flow: StartDeviceLogin() POSTs /v1/auth/device/code, opens the verification URI
 * in the user's browser, and returns the userCode to display. The ImGui window
 * then polls PollDeviceLogin() (POST /v1/auth/device/verify) until the user
 * approves; on success the access token is persisted to config key "Satella.Token"
 * (mirroring the existing "Satella.PlayerSeed" pattern) and reused as a bearer
 * token by every authenticated REST call. */
namespace SatellaAuth {
struct DeviceCode {
    std::string userCode;
    std::string verificationUri;
    int expiresIn = 0;
    int interval = 5;
};

bool IsAuthenticated();
std::string GetToken();
void Logout();

bool StartDeviceLogin(DeviceCode& code);

enum class PollResult { Pending, Success, Expired, Error };
PollResult PollDeviceLogin();

std::string GetLastError();
}
#endif
