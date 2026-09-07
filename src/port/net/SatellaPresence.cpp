#ifdef USE_NETWORKING
#include "port/net/SatellaPresence.h"

#include "port/net/SatellaApi.h"
#include "port/net/SatellaAuth.h"
#include "spdlog/spdlog.h"

#include "game/area.h"      // gCurrCourseNum
#include "course_table.h"   // COURSE_* enum

#include <nlohmann/json.hpp>
#include <atomic>
#include <chrono>
#include <thread>

namespace SatellaPresence {

namespace {
// course id -> display name. All COURSE_* values come from levels/course_defines.h
// (included via course_table.h), so this table is compile-safe.
struct CourseName {
    int id;
    const char* name;
};
constexpr CourseName kCourseNames[] = {
    { COURSE_BOB, "Bob-omb Battlefield" },
    { COURSE_WF, "Whomp's Fortress" },
    { COURSE_JRB, "Jolly Roger Bay" },
    { COURSE_CCM, "Cool, Cool Mountain" },
    { COURSE_BBH, "Big Boo's Haunt" },
    { COURSE_HMC, "Hazy Maze Cave" },
    { COURSE_LLL, "Lethal Lava Land" },
    { COURSE_SSL, "Shifting Sand Land" },
    { COURSE_DDD, "Dire, Dire Docks" },
    { COURSE_SL, "Snowman's Land" },
    { COURSE_WDW, "Wet-Dry World" },
    { COURSE_TTM, "Tall, Tall Mountain" },
    { COURSE_THI, "Tiny-Huge Island" },
    { COURSE_TTC, "Tick Tock Clock" },
    { COURSE_RR, "Rainbow Ride" },
    { COURSE_BITDW, "Bowser in the Dark World" },
    { COURSE_BITFS, "Bowser in the Fire Sea" },
    { COURSE_BITS, "Bowser in the Sky" },
    { COURSE_PSS, "Princess's Secret Slide" },
    { COURSE_COTMC, "Cavern of the Metal Cap" },
    { COURSE_TOTWC, "Tower of the Wing Cap" },
    { COURSE_VCUTM, "Vanish Cap Under the Moat" },
    { COURSE_WMOTR, "Winged Mario Over the Rainbow" },
    { COURSE_SA, "Secret Aquarium" },
    { COURSE_CAKE_END, "The End" },
};

const char* CourseIdToName(int courseId) {
    for (const auto& c : kCourseNames) {
        if (c.id == courseId) return c.name;
    }
    return "Castle";
}
} // namespace

static std::thread sThread;
static std::atomic<bool> sRun{ false };

void Tick() {
    if (!SatellaAuth::IsAuthenticated()) return;

    // gCurrCourseNum is written by the single game thread; a torn read of a small
    // int here only ever produces a one-cycle-stale course for presence, which is
    // acceptable for a best-effort "now playing" signal.
    const int courseId = gCurrCourseNum;

    nlohmann::json payload;
    payload["game"] = "Ghostship";
    payload["status"] = (courseId <= COURSE_NONE) ? "online" : "ingame";
    payload["courseId"] = courseId;
    payload["course"] = CourseIdToName(courseId);

    int status = 0;
    nlohmann::json resp;
    if (!Satella::PostJson("/v1/presence/update", payload, status, resp)) {
        SPDLOG_DEBUG("Satella: presence update failed (status {})", status);
    }
}

void Start() {
    if (sRun.exchange(true)) {
        return; // already running
    }
    sThread = std::thread([]() {
        // Send the first heartbeat immediately so presence is registered
        // before the user opens the Friends tab (no 30s wait).
        Tick();
        while (sRun.load(std::memory_order_acquire)) {
            // Sleep in 1s increments so Stop() is responsive (max 1s to join).
            for (int i = 0; i < 30 && sRun.load(std::memory_order_acquire); ++i) {
                std::this_thread::sleep_for(std::chrono::seconds(1));
            }
            if (!sRun.load(std::memory_order_acquire)) break;
            Tick();
        }
    });
}

void Stop() {
    sRun.store(false, std::memory_order_release);
    if (sThread.joinable()) {
        sThread.join();
    }
}

} // namespace SatellaPresence
#endif
