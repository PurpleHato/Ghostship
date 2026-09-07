#pragma once

#ifdef USE_NETWORKING
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>
#include <atomic>

#include <ixwebsocket/IXWebSocketMessage.h>

#define SATELLA_HOST "wss://satella.net64.dev"

namespace Satella {

enum class Phase { Idle, Connecting, FetchingKeys, Done };

class IPacket {
public:
    virtual ~IPacket() = default;
    virtual const char* GetRoute() const = 0;
    virtual void OnResponse(int16_t status, const std::string& body) = 0;
    virtual bool IsBlocking() const { return false; }
    virtual bool IsSubscription() const { return false; }
    virtual void OnPush(int16_t status, uint8_t packetType, const std::string& body) {}
};

class Client {
public:
    static Client& Instance();

    void Register(std::unique_ptr<IPacket> packet);
    void Execute(const std::string& url = SATELLA_HOST);
    Phase GetPhase() const { return mPhase.load(std::memory_order_relaxed); }

    // Register a subscription packet onto an already-running connection.
    // If not yet connected, the packet is queued for the next Execute() call.
    void RegisterLive(std::unique_ptr<IPacket> packet);

    // Send a raw binary frame: HM64 header + route + 4-byte body length + body.
    // No-op if the socket is not open.
    void SendRaw(const std::string& route, const void* data, size_t size);

    // Blocking JSON request/response over the (authed) WS socket: frames
    // HM64 + JSON type + route + body, awaits the server reply. Serialized by
    // an internal request mutex so only one request is in flight at a time
    // (prevents stray replies from being misrouted). Returns false if the
    // socket isn't open or no reply arrived within the timeout.
    bool RequestJson(const std::string& route, const std::string& jsonBody,
                     int16_t& outStatus, std::string& outBody);

private:
    Client() = default;
    ~Client();

    void Connect(const std::string& url);
    void SendAndReceive(IPacket& packet);
    void OnMessage(const ix::WebSocketMessagePtr& msg);

    std::vector<std::unique_ptr<IPacket>> mPackets;
    std::vector<IPacket*>                 mSubscriptions;

    std::string   mUrl;

    std::mutex              mMtx;
    std::condition_variable mCv;
    // Serializes RequestJson callers so replies always match the in-flight req.
    std::mutex              mReqMtx;

    std::atomic<Phase> mPhase{ Phase::Idle };

    bool    mConnected          = false;
    bool    mWaitingForResponse = false;
    bool    mResponseReady      = false;
    bool    mResponseValid      = false;
    int16_t mResponseStatus     = 0;
    std::string mResponseBody;
};

// Minimal leaderboard row cached from the live WS push; SatellaWindow reads it.
struct LeaderboardRow {
    std::string username;
    int64_t timeMs = 0;
};
// Per-course live cache, updated by the leaderboard broadcast-on-submit push.
const std::vector<LeaderboardRow>* GetCachedLeaderboard(const std::string& courseId);
void SetCachedLeaderboard(const std::string& courseId, std::vector<LeaderboardRow> rows);

// ── Chat (ephemeral, relay-based) ────────────────────────────────────────────
struct IncomingChatMessage {
    std::string channelId;
    std::string from;
    std::string text;
    int64_t ts = 0;
    std::string alias;
    int accentColor = 0;
    std::string avatarUrl;
};
// Drain the incoming chat queue (called by SatellaWindow each frame).
std::vector<IncomingChatMessage> DrainIncomingChat();

template<typename T>
struct PacketAutoReg {
    PacketAutoReg() { Client::Instance().Register(std::make_unique<T>()); }
};

#define SATELLA_REGISTER_PACKET(T) \
    static ::Satella::PacketAutoReg<T> _satella_pkt_##T

} // namespace Satella
#endif