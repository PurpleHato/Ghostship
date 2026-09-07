#pragma once

#ifdef USE_NETWORKING
#include <ship/window/gui/GuiWindow.h>

/* Cohesive in-game Satella window — opened from the top-level "Satella" menu
 * header. Five tabs:
 *   - Connect     : "Use Satella" toggle + Discord device-code login (and, when
 *                   authed, the signed-in identity + sign out). Fires a login
 *                   toast on success.
 *   - Achievements: the shared achievement card grid (DrawAchievementsGrid).
 *   - Badges      : race medals (ranked) + prestige, fetched over WS.
 *   - Profile     : current-session + total (persisted, offline-accumulated)
 *                   play time, and an "open web profile" button.
 *   - Leaderboard : live race leaderboard (WS initial fetch + broadcast refresh). */
class SatellaWindow : public Ship::GuiWindow {
  public:
    using GuiWindow::GuiWindow;
    virtual ~SatellaWindow() = default;

  protected:
    void InitElement() override;
    void DrawElement() override;
    void UpdateElement() override;
};

// Inline panel draw functions — called from the menu sidebar PreFunc (draws the
// content directly in the sidebar, like Settings/Enhancements) or from the
// standalone SatellaWindow popup. UpdateElement (login poll + playtime) always
// runs because the GuiWindow stays registered even when hidden.
void DrawSatellaConnect(); // Account panel (identity, login, dev settings)
void DrawSatellaAchievements();
void DrawSatellaBadges();
void DrawSatellaProfile();
void DrawSatellaLeaderboard();
void DrawSatellaFriends();

// Shared avatar loader — callable from SatellaClient.cpp (chat notifications)
// and SatellaWindow.cpp (panels). Downloads the URL on a background thread,
// decodes with stb_image, uploads on the render thread. Idempotent.
void SatellaRequestAvatar(const std::string& url, const std::string& cacheKey);
bool SatellaHasAvatar(const std::string& cacheKey);

// Force-fetch the current user's profile (alias, accent, avatar, ulid) from
// /v1/user. Called at connect time so sSelf is populated before any chat
// messages are sent. Idempotent (sets a flag so periodic re-fetch works).
void SatellaFetchSelfProfile();
#endif
