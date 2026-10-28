#include "mod/LinkBackend.h"

#include <ll/api/command/CommandHandle.h>
#include <ll/api/command/CommandRegistrar.h>
#include <ll/api/memory/Hook.h>
#include <ll/api/mod/RegisterHelper.h>
#include <ll/api/service/Bedrock.h>

#include <mc/certificates/identity/PlayerAuthenticationInfo.h>
#include <mc/common/Globals.h>
#include <mc/deps/ecs/gamerefs_entity/EntityContext.h>
#include <mc/deps/ecs/gamerefs_entity/GameRefsEntity.h>
#include <mc/certificates/identity/PlayerAuthenticationType.h>
#include <mc/network/ConnectionRequest.h>
#include <mc/network/ServerNetworkHandler.h>
#include <mc/network/packet/LoginPacket.h>
#include <mc/server/DedicatedServer.h>
#include <mc/server/PropertiesSettings.h>
#include <mc/world/actor/player/Player.h>

#include <filesystem>
#include <format>

namespace link_backend {
namespace {

/// The default `AgentConfig` writes this into config.json; it is compared against
/// so a fresh install gets a clear warning instead of a confusing auth failure.
constexpr std::string_view kPlaceholderToken = "replace-with-the-same-token-as-crosslink";

bool startsWith(std::string_view value, std::string_view prefix) {
    return !prefix.empty() && value.size() >= prefix.size() && value.compare(0, prefix.size(), prefix) == 0;
}

/// Recovers the XUID the gateway smuggled through a client property field.
std::string carrierXuid(std::string const& value, std::string_view xuidPrefix, std::string_view devicePrefix) {
    if (startsWith(value, xuidPrefix)) return value.substr(xuidPrefix.size());
    if (startsWith(value, devicePrefix)) return value.substr(devicePrefix.size());
    // Legacy prefixes from the predecessor project.
    if (startsWith(value, "MeshGate-XUID:")) return value.substr(14);
    if (startsWith(value, "meshgate:xuid:")) return value.substr(14);
    return {};
}

} // namespace

LinkBackendMod& LinkBackendMod::getInstance() {
    static LinkBackendMod instance;
    return instance;
}

std::string LinkBackendMod::carrierPrefix(std::string_view which) const {
    return which == "device" ? mConfig.deviceCarrierPrefix : mConfig.xuidCarrierPrefix;
}

void LinkBackendMod::rememberIdentity(std::string const& xuid, std::string const& name) {
    if (xuid.empty()) return;
    std::lock_guard lock{mIdentityMutex};
    mXuidToName[xuid] = name;
    if (!name.empty()) mNameToXuid[name] = xuid;
}

bool LinkBackendMod::connected() const noexcept { return mClient && mClient->connected(); }

std::string LinkBackendMod::serverId() const { return mClientConfig.serverId; }

Json LinkBackendMod::gatewayInfo() const { return mClient ? mClient->gatewayInfo() : Json::object(); }

bool LinkBackendMod::requestTransfer(
    std::string const& xuid,
    std::string const& playerName,
    std::string const& target,
    Json&              reply,
    std::string&       error
) {
    if (!mClient) {
        error = "the control client is not running";
        return false;
    }
    auto payload = Json::object();
    payload["xuid"] = xuid;
    payload["playerName"] = playerName;
    payload["identity"] = xuid.empty() ? playerName : xuid;
    payload["target"] = target;
    payload["source"] = mClientConfig.serverId;
    return mClient->request("crosslink.transfer.request", std::move(payload), reply, error);
}

void LinkBackendMod::requestTransferAsync(
    std::string const& xuid,
    std::string const& playerName,
    std::string const& target,
    ReplyHandler       handler
) {
    if (!mClient) {
        if (handler) handler(false, Json::object(), "the control client is not running");
        return;
    }
    auto payload = Json::object();
    payload["xuid"] = xuid;
    payload["playerName"] = playerName;
    payload["identity"] = xuid.empty() ? playerName : xuid;
    payload["target"] = target;
    payload["source"] = mClientConfig.serverId;
    mClient->requestAsync("crosslink.transfer.request", std::move(payload), std::move(handler));
}

std::uint64_t LinkBackendMod::onGatewayRequest(std::string const& kind, RequestHandler handler) {
    return mClient ? mClient->onGatewayRequest(kind, std::move(handler)) : 0;
}

bool LinkBackendMod::offGatewayRequest(std::uint64_t registration) {
    return mClient && mClient->offGatewayRequest(registration);
}

std::uint64_t LinkBackendMod::onConnectionChange(std::function<void(bool)> observer) {
    return mClient ? mClient->onConnectionChange(std::move(observer)) : 0;
}

bool LinkBackendMod::offConnectionChange(std::uint64_t registration) {
    return mClient && mClient->offConnectionChange(registration);
}

bool LinkBackendMod::xuidOf(std::string const& playerName, std::string& xuid) const {
    std::lock_guard lock{mIdentityMutex};
    const auto found = mNameToXuid.find(playerName);
    if (found == mNameToXuid.end()) return false;
    xuid = found->second;
    return true;
}

std::string LinkBackendMod::nameOfXuid(std::string const& xuid) const {
    std::lock_guard lock{mIdentityMutex};
    const auto found = mXuidToName.find(xuid);
    return found == mXuidToName.end() ? std::string{} : found->second;
}

bool LinkBackendMod::load() {
    auto& self = getSelf();
    const auto path = self.getConfigDir() / "config.json";

    // ll::config writes a fully defaulted file when it is missing, and merges an
    // older file into the current defaults otherwise, so adding a field never
    // invalidates an existing deployment. It returns false when it had to rewrite.
    if (!ll::config::loadConfig(mConfig, path)) {
        self.getLogger().info("updated {} to configuration version {}", path.string(), mConfig.version);
    }

    mClientConfig.host = mConfig.gatewayHost;
    mClientConfig.port = static_cast<std::uint16_t>(mConfig.gatewayPort);
    mClientConfig.token = mConfig.token;
    mClientConfig.serverId = mConfig.serverId;
    mClientConfig.heartbeatSeconds = mConfig.heartbeatSeconds;
    mClientConfig.requestTimeoutSeconds = mConfig.requestTimeoutSeconds;
    mClientConfig.reconnectSeconds = mConfig.reconnectSeconds;
    if (auto const& version = self.getManifest().version) {
        mClientConfig.agentVersion = std::format("{}.{}.{}", version->major, version->minor, version->patch);
    }

    if (mConfig.token == kPlaceholderToken) {
        self.getLogger().warn("config.token is still the placeholder; the gateway will refuse this agent");
    } else if (mConfig.token.size() < 32) {
        self.getLogger().error("config.token is shorter than 32 characters; the gateway will refuse this agent");
    }
    if (mConfig.serverId.empty()) {
        self.getLogger().error("config.serverId is empty; it must match a server id in the gateway configuration");
    }

    mClient = std::make_unique<ControlClient>(mClientConfig);
    self.getLogger().info(
        "LinkBackend loaded as server '{}' -> gateway {}:{}",
        mClientConfig.serverId,
        mClientConfig.host,
        mClientConfig.port
    );
    return true;
}

bool LinkBackendMod::enable() {
    auto& self = getSelf();
    mClient->onConnectionChange([this](bool connected) {
        auto& logger = getSelf().getLogger();
        if (connected) {
            const auto info = mClient->gatewayInfo();
            logger.info(
                "connected to CrossLink (protocol {}, game {}, transport {})",
                info.value("protocolVersion", 0),
                info.value("gameVersion", std::string{"?"}),
                info.value("transport", std::string{"?"})
            );
        } else {
            logger.warn("lost the CrossLink control connection, retrying every {}s", mClientConfig.reconnectSeconds);
        }
    });
    mClient->start();
    self.getLogger().info("enabled");
    return true;
}

bool LinkBackendMod::disable() {
    if (mClient) mClient->stop();
    getSelf().getLogger().info("disabled");
    return true;
}

bool LinkBackendMod::unload() {
    mClient.reset();
    return true;
}

} // namespace link_backend

namespace {

using link_backend::LinkBackendMod;
using link_backend::carrierXuid;

/// Reads the gateway's identity carriers back out of the login and hands BDS a
/// fully authenticated player, so a transfer keeps the same XUID on every server.
bool injectIdentity(ConnectionRequest const& request, PlayerAuthenticationInfo& info) {
    auto& mod = LinkBackendMod::getInstance();
    const auto xuid = carrierXuid(request.getDeviceModel(), mod.carrierPrefix("xuid"), mod.carrierPrefix("device"));
    const auto deviceXuid = xuid.empty() ? carrierXuid(request.getDeviceId(), mod.carrierPrefix("xuid"), mod.carrierPrefix("device")) : xuid;
    if (deviceXuid.empty()) return false;

    info.Xuid = deviceXuid;
    info.XboxLiveName = request.getThirdPartyName();
    info.PlayFabId = request.getPlatformOnlineId();
    info.PublicKey = {};
    info.AuthenticatedUuid = makePlayerUUIDForXUID(deviceXuid);
    mod.rememberIdentity(deviceXuid, request.getThirdPartyName());
    return true;
}

} // namespace

LL_AUTO_TYPE_INSTANCE_HOOK(
    LinkBackendValidateLoginHook,
    HookPriority::Normal,
    ServerNetworkHandler,
    &ServerNetworkHandler::$_validateLoginPacket,
    std::optional<PlayerAuthenticationInfo>,
    NetworkIdentifier const& source,
    LoginPacket const& packet
) {
    auto validated = origin(source, packet);
    auto const& request = packet.mConnectionRequest.get();
    if (!validated || !request) return validated;
    if (injectIdentity(*request, *validated)) {
        const_cast<ConnectionRequest&>(*request).mAuthenticationType = PlayerAuthenticationType::Full;
    }
    return validated;
}

LL_AUTO_TYPE_INSTANCE_HOOK(
    LinkBackendCreatePlayerHook,
    HookPriority::Normal,
    ServerNetworkHandler,
    &ServerNetworkHandler::createNewPlayer,
    OwnerPtr<EntityContext>,
    NetworkIdentifier const& source,
    ConnectionRequest const& connectionRequest,
    PlayerAuthenticationInfo const& playerInfo
) {
    auto enriched = playerInfo;
    if (!injectIdentity(connectionRequest, enriched)) return origin(source, connectionRequest, playerInfo);
    auto normalized = connectionRequest;
    normalized.mAuthenticationType = PlayerAuthenticationType::Full;
    return origin(source, normalized, enriched);
}

/// A gateway-fronted server must not let clients generate chunks: the client would
/// build terrain for a world it is about to leave.
LL_AUTO_TYPE_INSTANCE_HOOK(
    LinkBackendClientSideGenHook,
    HookPriority::Normal,
    DedicatedServer,
    &DedicatedServer::initializeHttp,
    void,
    PropertiesSettings const& properties
) {
    auto& settings = const_cast<PropertiesSettings&>(properties);
    settings.mClientSideGenerationEnabled = false;
    return origin(settings);
}

LL_REGISTER_MOD(link_backend::LinkBackendMod, link_backend::LinkBackendMod::getInstance())

// The single narrow ABI surface other LeviLamina mods link against.
LINK_BACKEND_EXPORT link_backend::IAgentApi* linkBackendApi() { return &link_backend::LinkBackendMod::getInstance(); }

LINK_BACKEND_EXPORT std::uint32_t linkBackendApiVersion() { return LINK_BACKEND_API_VERSION; }
