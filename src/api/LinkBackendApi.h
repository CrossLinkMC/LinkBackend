#pragma once

// LinkBackend's exported surface for other LeviLamina mods.
//
// Everything crosses the DLL boundary through the two extern "C" functions at the
// bottom, so the ABI stays narrow. A dependent mod declares
// {"name": "LinkBackend"} in its manifest dependencies and links this library.

#include <nlohmann/json.hpp>

#include <cstdint>
#include <functional>
#include <string>

#ifdef _WIN32
#define LINK_BACKEND_EXPORT extern "C" __declspec(dllexport)
#else
#define LINK_BACKEND_EXPORT extern "C" __attribute__((visibility("default")))
#endif

#define LINK_BACKEND_API_VERSION 1u

namespace link_backend {

using Json = nlohmann::ordered_json;

/// Called when an agent-to-gateway request finishes. `ok` is false on transport
/// failure or timeout, and also when the gateway answered with an error kind.
using ReplyHandler = std::function<void(bool ok, Json const& reply, std::string const& error)>;

/// Handles a request the gateway sent to this server. The returned json becomes
/// the reply payload; return an object holding {"accepted": false, "reason": ...}
/// to refuse. Runs on the control-channel reader thread, so it must not block and
/// must not touch BDS objects directly.
using RequestHandler = std::function<Json(Json const& payload)>;

class IAgentApi {
public:
    virtual ~IAgentApi() = default;

    [[nodiscard]] virtual std::uint32_t apiVersion() const noexcept = 0;

    // ---- connection state -------------------------------------------
    [[nodiscard]] virtual bool        connected() const noexcept = 0;
    [[nodiscard]] virtual std::string serverId() const           = 0;
    /// What the gateway reported in its hello.ack.
    [[nodiscard]] virtual Json        gatewayInfo() const        = 0;

    // ---- agent -> gateway --------------------------------------------
    /// Blocking transfer request. Returns false and fills `error` when the gateway
    /// refused or could not be reached; `reply` carries operationId/phase/reason.
    virtual bool requestTransfer(
        std::string const& xuid,
        std::string const& playerName,
        std::string const& target,
        Json&              reply,
        std::string&       error
    ) = 0;
    virtual void requestTransferAsync(
        std::string const& xuid,
        std::string const& playerName,
        std::string const& target,
        ReplyHandler       handler
    ) = 0;

    /// Registered handler for a gateway kind, e.g. "crosslink.handoff.prepare".
    /// Several mods may register the same kind; they are consulted in registration
    /// order until one returns a payload containing "accepted".
    virtual std::uint64_t onGatewayRequest(std::string const& kind, RequestHandler handler) = 0;
    virtual bool          offGatewayRequest(std::uint64_t registration)                     = 0;

    /// Notified when the control channel comes up or goes down.
    virtual std::uint64_t onConnectionChange(std::function<void(bool connected)> observer) = 0;
    virtual bool          offConnectionChange(std::uint64_t registration)                  = 0;

    // ---- identity ------------------------------------------------------
    /// The XUID the gateway attached to this player's login, if any.
    [[nodiscard]] virtual bool        xuidOf(std::string const& playerName, std::string& xuid) const = 0;
    [[nodiscard]] virtual std::string nameOfXuid(std::string const& xuid) const                      = 0;
};

} // namespace link_backend

LINK_BACKEND_EXPORT link_backend::IAgentApi* linkBackendApi();
LINK_BACKEND_EXPORT std::uint32_t            linkBackendApiVersion();
