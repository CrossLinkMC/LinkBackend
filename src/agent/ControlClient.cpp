#include "agent/ControlClient.h"

#include <chrono>
#include <format>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace link_backend {
namespace {

void closeLinkSocket(LinkSocket socket) {
    if (socket == invalidLinkSocket) return;
#ifdef _WIN32
    ::closesocket(socket);
#else
    ::close(socket);
#endif
}

bool writeAll(LinkSocket socket, std::string const& data) {
    std::size_t sent = 0;
    while (sent < data.size()) {
        const auto written = ::send(socket, data.data() + sent, static_cast<int>(data.size() - sent), 0);
        if (written <= 0) return false;
        sent += static_cast<std::size_t>(written);
    }
    return true;
}

} // namespace

ControlClient::ControlClient(ClientConfig config) : mConfig(std::move(config)) {}

ControlClient::~ControlClient() { stop(); }

void ControlClient::start() {
    mStopping.store(false);
    mWorker = std::thread([this] { run(); });
}

void ControlClient::stop() noexcept {
    mStopping.store(true);
    {
        std::lock_guard lock{mSendMutex};
        closeLinkSocket(mSocket);
        mSocket = invalidLinkSocket;
    }
    if (mWorker.joinable()) mWorker.join();
    failPending("the agent is shutting down");
}

Json ControlClient::gatewayInfo() const {
    std::lock_guard lock{mInfoMutex};
    return mGatewayInfo;
}

void ControlClient::setConnected(bool value) {
    if (mConnected.exchange(value) == value) return;
    std::vector<std::function<void(bool)>> observers;
    {
        std::lock_guard lock{mHandlersMutex};
        for (auto const& [id, observer] : mObservers) observers.push_back(observer);
    }
    for (auto const& observer : observers) observer(value);
}

void ControlClient::run() {
#ifdef _WIN32
    WSADATA data{};
    if (::WSAStartup(MAKEWORD(2, 2), &data) != 0) return;
#endif
    while (!mStopping.load()) {
        LinkSocket socket = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (socket == invalidLinkSocket) {
            std::this_thread::sleep_for(std::chrono::seconds(mConfig.reconnectSeconds));
            continue;
        }

        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_port = htons(mConfig.port);
        const auto host = mConfig.host == "localhost" ? std::string{"127.0.0.1"} : mConfig.host;
        std::string error;
        if (::InetPtonA(AF_INET, host.c_str(), &address.sin_addr) != 1) {
            error = "gateway host must be an IPv4 literal or localhost";
        } else if (::connect(socket, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
            error = std::format("cannot reach the gateway at {}:{}", host, mConfig.port);
        }

        if (!error.empty()) {
            closeLinkSocket(socket);
            setConnected(false);
            if (!mStopping.load()) std::this_thread::sleep_for(std::chrono::seconds(mConfig.reconnectSeconds));
            continue;
        }

        {
            std::lock_guard lock{mSendMutex};
            mSocket = socket;
        }
        if (!handshake(socket, error)) {
            closeLinkSocket(socket);
            {
                std::lock_guard lock{mSendMutex};
                mSocket = invalidLinkSocket;
            }
            setConnected(false);
            if (!mStopping.load()) std::this_thread::sleep_for(std::chrono::seconds(mConfig.reconnectSeconds));
            continue;
        }

        setConnected(true);
        readLoop(socket);
        setConnected(false);
        failPending("the control connection was lost");
        closeLinkSocket(socket);
        {
            std::lock_guard lock{mSendMutex};
            mSocket = invalidLinkSocket;
        }
        if (!mStopping.load()) std::this_thread::sleep_for(std::chrono::seconds(mConfig.reconnectSeconds));
    }
#ifdef _WIN32
    ::WSACleanup();
#endif
}

bool ControlClient::handshake(LinkSocket socket, std::string& error) {
    auto hello = Json::object();
    hello["v"] = 1;
    hello["id"] = mNextId.fetch_add(1);
    hello["kind"] = "hello";
    auto payload = Json::object();
    payload["token"] = mConfig.token;
    payload["serverId"] = mConfig.serverId;
    payload["agentVersion"] = mConfig.agentVersion;
    auto capabilities = Json::object();
    capabilities["phases"] = Json::array({"prepare", "detach", "bind", "commit", "abort"});
    payload["capabilities"] = std::move(capabilities);
    hello["payload"] = std::move(payload);

    const auto requestId = hello["id"].get<std::uint64_t>();
    auto pending = std::make_shared<Pending>();
    {
        std::lock_guard lock{mPendingMutex};
        mPending.emplace(requestId, pending);
    }
    if (!writeAll(socket, hello.dump() + "\n")) {
        error = "cannot send the hello";
        return false;
    }

    // Read until the ack arrives; the socket is still exclusively ours here.
    std::string buffer;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(mConfig.requestTimeoutSeconds);
    char chunk[8192];
    while (std::chrono::steady_clock::now() < deadline) {
        const auto received = ::recv(socket, chunk, sizeof(chunk), 0);
        if (received <= 0) {
            error = "the gateway closed the connection during the handshake";
            return false;
        }
        buffer.append(chunk, static_cast<std::size_t>(received));
        const auto newline = buffer.find('\n');
        if (newline == std::string::npos) continue;

        const auto line = buffer.substr(0, newline);
        buffer.erase(0, newline + 1);
        const auto json = Json::parse(line, nullptr, false, true);
        if (json.is_discarded()) continue;
        if (json.value("kind", std::string{}) == "hello.ack") {
            std::lock_guard lock{mInfoMutex};
            mGatewayInfo = json.contains("payload") ? json["payload"] : Json::object();
            {
                std::lock_guard pendingLock{mPendingMutex};
                mPending.erase(requestId);
            }
            return true;
        }
        if (json.value("kind", std::string{}) == "error") {
            error = json.contains("payload") ? json["payload"].value("message", std::string{"the gateway refused the hello"})
                                             : std::string{"the gateway refused the hello"};
            return false;
        }
    }
    error = "the gateway did not answer the hello in time";
    return false;
}

void ControlClient::readLoop(LinkSocket socket) {
    std::string buffer;
    char chunk[8192];
    auto lastPing = std::chrono::steady_clock::now();

    while (!mStopping.load()) {
        const auto now = std::chrono::steady_clock::now();
        if (now - lastPing >= std::chrono::seconds(mConfig.heartbeatSeconds)) {
            lastPing = now;
            auto ping = Json::object();
            ping["v"] = 1;
            ping["id"] = mNextId.fetch_add(1);
            ping["kind"] = "ping";
            ping["payload"] = Json::object();
            if (!sendLine(ping)) return;
        }

        const auto received = ::recv(socket, chunk, sizeof(chunk), 0);
        if (received <= 0) return;
        buffer.append(chunk, static_cast<std::size_t>(received));

        while (true) {
            const auto newline = buffer.find('\n');
            if (newline == std::string::npos) break;
            auto line = buffer.substr(0, newline);
            buffer.erase(0, newline + 1);
            if (line.size() > mConfig.maxLineBytes) return;
            while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
            if (!line.empty()) handleLine(line);
        }
    }
}

void ControlClient::handleLine(std::string const& line) {
    const auto json = Json::parse(line, nullptr, false, true);
    if (json.is_discarded() || !json.is_object()) return;

    const auto kind = json.value("kind", std::string{});
    const auto id = json.contains("id") && json["id"].is_number_unsigned() ? json["id"].get<std::uint64_t>() : 0u;
    const auto replyTo = json.contains("replyTo") && json["replyTo"].is_number_unsigned() ? json["replyTo"].get<std::uint64_t>() : 0u;
    const Json payload = json.contains("payload") ? json["payload"] : Json::object();

    if (replyTo != 0) {
        std::shared_ptr<Pending> pending;
        {
            std::lock_guard lock{mPendingMutex};
            const auto found = mPending.find(replyTo);
            if (found == mPending.end()) return;
            pending = found->second;
            mPending.erase(found);
        }
        {
            std::lock_guard lock{pending->mutex};
            pending->ok = kind != "error";
            pending->reply = payload;
            if (!pending->ok) pending->error = payload.value("message", std::string{"the gateway reported an error"});
            pending->done = true;
        }
        pending->condition.notify_all();
        if (pending->handler) pending->handler(pending->ok, pending->reply, pending->error);
        return;
    }

    // A request from the gateway: consult every registered handler for the kind
    // until one answers with a decision.
    std::vector<RequestHandler> handlers;
    {
        std::lock_guard lock{mHandlersMutex};
        for (auto const& entry : mHandlers) {
            if (entry.kind == kind) handlers.push_back(entry.handler);
        }
    }

    Json replyPayload = Json::object();
    bool decided = false;
    for (auto const& handler : handlers) {
        try {
            auto result = handler(payload);
            if (result.is_object() && result.contains("accepted")) {
                replyPayload = std::move(result);
                decided = true;
                break;
            }
            if (result.is_object() && !result.empty() && !decided) replyPayload = std::move(result);
        } catch (...) {
            // A misbehaving dependent mod must not take the channel down.
        }
    }

    if (id == 0) return;

    auto reply = Json::object();
    reply["v"] = 1;
    reply["id"] = 0;
    reply["replyTo"] = id;
    if (decided || !handlers.empty()) {
        reply["kind"] = kind;
        reply["payload"] = std::move(replyPayload);
    } else {
        reply["kind"] = "error";
        auto error = Json::object();
        error["message"] = std::format("no handler for '{}'", kind);
        reply["payload"] = std::move(error);
    }
    (void)sendLine(reply);
}

bool ControlClient::sendLine(Json const& json) {
    std::lock_guard lock{mSendMutex};
    if (mSocket == invalidLinkSocket) return false;
    return writeAll(mSocket, json.dump() + "\n");
}

bool ControlClient::request(std::string const& kind, Json payload, Json& reply, std::string& error) {
    if (!connected()) {
        error = "the gateway is not connected";
        return false;
    }
    auto pending = std::make_shared<Pending>();
    const auto id = mNextId.fetch_add(1);
    {
        std::lock_guard lock{mPendingMutex};
        mPending.emplace(id, pending);
    }

    auto message = Json::object();
    message["v"] = 1;
    message["id"] = id;
    message["kind"] = kind;
    message["payload"] = std::move(payload);
    if (!sendLine(message)) {
        std::lock_guard lock{mPendingMutex};
        mPending.erase(id);
        error = "cannot reach the gateway";
        return false;
    }

    std::unique_lock lock{pending->mutex};
    const auto timeout = std::chrono::seconds(mConfig.requestTimeoutSeconds);
    if (!pending->condition.wait_for(lock, timeout, [&pending] { return pending->done; })) {
        std::lock_guard pendingLock{mPendingMutex};
        mPending.erase(id);
        error = std::format("the gateway did not answer '{}' in time", kind);
        return false;
    }
    reply = pending->reply;
    error = pending->error;
    return pending->ok;
}

void ControlClient::requestAsync(std::string const& kind, Json payload, ReplyHandler handler) {
    auto pending = std::make_shared<Pending>();
    pending->handler = std::move(handler);
    const auto id = mNextId.fetch_add(1);
    {
        std::lock_guard lock{mPendingMutex};
        mPending.emplace(id, pending);
    }
    auto message = Json::object();
    message["v"] = 1;
    message["id"] = id;
    message["kind"] = kind;
    message["payload"] = std::move(payload);
    if (!sendLine(message)) {
        std::lock_guard lock{mPendingMutex};
        mPending.erase(id);
        if (pending->handler) pending->handler(false, Json::object(), "cannot reach the gateway");
    }
}

std::uint64_t ControlClient::onGatewayRequest(std::string const& kind, RequestHandler handler) {
    if (!handler) return 0;
    std::lock_guard lock{mHandlersMutex};
    const auto id = mNextRegistration++;
    mHandlers.push_back({id, kind, std::move(handler)});
    return id;
}

bool ControlClient::offGatewayRequest(std::uint64_t registration) {
    std::lock_guard lock{mHandlersMutex};
    const auto before = mHandlers.size();
    std::erase_if(mHandlers, [registration](auto const& entry) { return entry.id == registration; });
    return mHandlers.size() != before;
}

std::uint64_t ControlClient::onConnectionChange(std::function<void(bool)> observer) {
    if (!observer) return 0;
    std::lock_guard lock{mHandlersMutex};
    const auto id = mNextRegistration++;
    mObservers.emplace_back(id, std::move(observer));
    return id;
}

bool ControlClient::offConnectionChange(std::uint64_t registration) {
    std::lock_guard lock{mHandlersMutex};
    const auto before = mObservers.size();
    std::erase_if(mObservers, [registration](auto const& entry) { return entry.first == registration; });
    return mObservers.size() != before;
}

void ControlClient::failPending(std::string const& error) {
    std::vector<std::shared_ptr<Pending>> pending;
    {
        std::lock_guard lock{mPendingMutex};
        for (auto& [id, entry] : mPending) pending.push_back(entry);
        mPending.clear();
    }
    for (auto& entry : pending) {
        {
            std::lock_guard lock{entry->mutex};
            entry->ok = false;
            entry->error = error;
            entry->done = true;
        }
        entry->condition.notify_all();
        if (entry->handler) entry->handler(false, Json::object(), error);
    }
}

} // namespace link_backend
