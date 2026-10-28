#pragma once

#include "agent/ControlClient.h"
#include "api/LinkBackendApi.h"

#include <ll/api/Config.h>
#include <ll/api/mod/NativeMod.h>

#include <memory>
#include <mutex>
#include <unordered_map>

namespace link_backend {

/// Reflectable configuration, loaded and written back through `ll::config`.
/// The integral `version` member is what makes it satisfy `ll::config::IsConfig`
/// and lets LeviLamina migrate an older file instead of failing on it.
struct AgentConfig {
    int         version{1};
    std::string gatewayHost{"127.0.0.1"};
    int         gatewayPort{31920};
    std::string token{"replace-with-the-same-token-as-crosslink"};
    std::string serverId{"lobby"};
    int         heartbeatSeconds{15};
    int         requestTimeoutSeconds{10};
    int         reconnectSeconds{5};
    std::string xuidCarrierPrefix{"CrossLink-XUID:"};
    std::string deviceCarrierPrefix{"crosslink:xuid:"};
    std::string logLevel{"info"};
};

class LinkBackendMod final : public IAgentApi {
public:
    static LinkBackendMod& getInstance();

    LinkBackendMod() : mSelf(*ll::mod::NativeMod::current()) {}

    [[nodiscard]] ll::mod::NativeMod& getSelf() const { return mSelf; }

    bool load();
    bool enable();
    bool disable();
    bool unload();

    // IAgentApi
    [[nodiscard]] std::uint32_t apiVersion() const noexcept override { return LINK_BACKEND_API_VERSION; }
    [[nodiscard]] bool          connected() const noexcept override;
    [[nodiscard]] std::string   serverId() const override;
    [[nodiscard]] Json          gatewayInfo() const override;
    bool requestTransfer(std::string const& xuid, std::string const& playerName, std::string const& target, Json& reply,
                         std::string& error) override;
    void requestTransferAsync(std::string const& xuid, std::string const& playerName, std::string const& target,
                              ReplyHandler handler) override;
    std::uint64_t onGatewayRequest(std::string const& kind, RequestHandler handler) override;
    bool          offGatewayRequest(std::uint64_t registration) override;
    std::uint64_t onConnectionChange(std::function<void(bool)> observer) override;
    bool          offConnectionChange(std::uint64_t registration) override;
    [[nodiscard]] bool        xuidOf(std::string const& playerName, std::string& xuid) const override;
    [[nodiscard]] std::string nameOfXuid(std::string const& xuid) const override;

    // Identity carriers read back from the login the gateway re-signed.
    void rememberIdentity(std::string const& xuid, std::string const& name);
    [[nodiscard]] std::string carrierPrefix(std::string_view which) const;

    ll::mod::NativeMod&           mSelf;
    ClientConfig                  mClientConfig;
    AgentConfig                   mConfig;
    std::unique_ptr<ControlClient> mClient;
    mutable std::recursive_mutex  mIdentityMutex;
    std::unordered_map<std::string, std::string> mNameToXuid;
    std::unordered_map<std::string, std::string> mXuidToName;
};

} // namespace link_backend
