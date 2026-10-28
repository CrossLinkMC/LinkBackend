#pragma once

#include "api/LinkBackendApi.h"

#include <atomic>
#include <condition_variable>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <winsock2.h>
using LinkSocket = SOCKET;
inline constexpr LinkSocket invalidLinkSocket = INVALID_SOCKET;
#else
using LinkSocket = int;
inline constexpr LinkSocket invalidLinkSocket = -1;
#endif

namespace link_backend {

struct ClientConfig {
    std::string host{"127.0.0.1"};
    std::uint16_t port{31920};
    std::string token;
    std::string serverId;
    std::string agentVersion{"0.1.0"};
    int heartbeatSeconds{15};
    int requestTimeoutSeconds{10};
    int reconnectSeconds{5};
    std::size_t maxLineBytes{1u * 1024u * 1024u};
};

/// Persistent JSON Lines client for the CrossLink control plane, with reconnect,
/// heartbeat, request/reply correlation in both directions, and a registry of
/// handlers other mods install through IAgentApi.
class ControlClient {
public:
    explicit ControlClient(ClientConfig config);
    ~ControlClient();

    ControlClient(ControlClient const&)            = delete;
    ControlClient& operator=(ControlClient const&) = delete;

    void start();
    void stop() noexcept;

    [[nodiscard]] bool        connected() const noexcept { return mConnected.load(); }
    [[nodiscard]] Json        gatewayInfo() const;
    [[nodiscard]] ClientConfig const& config() const noexcept { return mConfig; }

    bool request(std::string const& kind, Json payload, Json& reply, std::string& error);
    void requestAsync(std::string const& kind, Json payload, ReplyHandler handler);

    std::uint64_t onGatewayRequest(std::string const& kind, RequestHandler handler);
    bool          offGatewayRequest(std::uint64_t registration);
    std::uint64_t onConnectionChange(std::function<void(bool)> observer);
    bool          offConnectionChange(std::uint64_t registration);

private:
    struct Pending {
        std::mutex              mutex;
        std::condition_variable condition;
        bool                    done{};
        bool                    ok{};
        Json                    reply;
        std::string             error;
        ReplyHandler            handler;
    };

    void run();
    bool handshake(LinkSocket socket, std::string& error);
    void readLoop(LinkSocket socket);
    void handleLine(std::string const& line);
    bool sendLine(Json const& json);
    void setConnected(bool value);
    void failPending(std::string const& error);

    ClientConfig                             mConfig;
    std::atomic_bool                         mStopping{false};
    std::atomic_bool                         mConnected{false};
    std::thread                              mWorker;
    mutable std::recursive_mutex             mSendMutex;
    LinkSocket                               mSocket{invalidLinkSocket};
    std::atomic<std::uint64_t>               mNextId{1};
    std::mutex                               mPendingMutex;
    std::map<std::uint64_t, std::shared_ptr<Pending>> mPending;
    mutable std::mutex                       mInfoMutex;
    Json                                     mGatewayInfo = Json::object();
    struct KindHandler {
        std::uint64_t   id{};
        std::string     kind;
        RequestHandler  handler;
    };
    mutable std::recursive_mutex             mHandlersMutex;
    std::vector<KindHandler>                 mHandlers;
    std::vector<std::pair<std::uint64_t, std::function<void(bool)>>> mObservers;
    std::uint64_t                            mNextRegistration{1};
};

} // namespace link_backend
