#include "component.hpp"
#include "omp_bridge.hpp"
#include "cef_extension_api.hpp"

#include <Server/Components/Pawn/Impl/pawn_natives.hpp>
#include <Server/Components/Pawn/Impl/pawn_impl.hpp>

#include <shared/query_port.hpp>
#include <shared/version.hpp>

#include <string>

namespace
{
    std::string StringViewToString(StringView sv)
    {
        return sv.data() ? std::string(sv.data(), sv.length()) : std::string{};
    }

    ResourceLoaderUiMode ReadResourceLoaderMode(IConfig& config, ILogger& logger)
    {
        if (config.getType("cef.resources_loader_mode") == ConfigOptionType_String)
        {
            const StringView mode = config.getString("cef.resources_loader_mode");
            const std::string value = StringViewToString(mode);

            if (value == "cef")
                return ResourceLoaderUiMode::Cef;

            if (value == "samp_dialog")
                return ResourceLoaderUiMode::SampDialog;

            if (value == "none")
                return ResourceLoaderUiMode::None;

            logger.printLn(
                "[CEF] Invalid cef.resources_loader_mode '%.*s'. Expected 'cef', 'samp_dialog' or 'none'. Defaulting to 'cef'.",
                static_cast<int>(mode.length()),
                mode.data());

            return ResourceLoaderUiMode::Cef;
        }

        const bool legacy_ui_enabled = config.getBool("cef.resources_loader_ui")
            ? *config.getBool("cef.resources_loader_ui")
            : true;

        return legacy_ui_enabled ? ResourceLoaderUiMode::Cef : ResourceLoaderUiMode::None;
    }

    uint16_t ResolveCefNetworkPort(
        uint16_t server_port,
        uint16_t port_offset,
        uint16_t default_port,
        IConfig& config,
        ILogger& logger,
        bool log_messages)
    {
        const int derived_port = static_cast<int>(server_port) + static_cast<int>(port_offset);
        const bool derived_valid = (derived_port >= 1 && derived_port <= 65535);
        const uint16_t fallback_port = derived_valid
            ? static_cast<uint16_t>(derived_port)
            : default_port;

        if (config.getType("cef.port") == ConfigOptionType_None)
        {
            if (log_messages && !derived_valid)
            {
                logger.printLn(
                    "[CEF] Derived UDP port (%u + %u = %d) is out of range. Defaulting to %u.",
                    static_cast<unsigned int>(server_port),
                    static_cast<unsigned int>(port_offset),
                    derived_port,
                    static_cast<unsigned int>(fallback_port));
            }
            return fallback_port;
        }

        int* configured_port_ptr = config.getInt("cef.port");
        if (!configured_port_ptr)
        {
            if (log_messages)
            {
                logger.printLn(
                    "[CEF] Invalid cef.port value. Expected integer 0 (auto) or 1-65535. Defaulting to %u.",
                    static_cast<unsigned int>(fallback_port));
            }
            return fallback_port;
        }

        const int configured_port = *configured_port_ptr;
        if (configured_port == 0)
        {
            if (log_messages && !derived_valid)
            {
                logger.printLn(
                    "[CEF] Derived UDP port (%u + %u = %d) is out of range. Defaulting to %u.",
                    static_cast<unsigned int>(server_port),
                    static_cast<unsigned int>(port_offset),
                    derived_port,
                    static_cast<unsigned int>(fallback_port));
            }
            return fallback_port;
        }

        if (configured_port < 1 || configured_port > 65535)
        {
            if (log_messages)
            {
                logger.printLn(
                    "[CEF] Invalid cef.port %d. Expected 0 (auto) or 1-65535. Defaulting to %u.",
                    configured_port,
                    static_cast<unsigned int>(fallback_port));
            }
            return fallback_port;
        }

        if (log_messages)
        {
            logger.printLn("[CEF] Using configured UDP port %d (cef.port).", configured_port);
            if (derived_valid && configured_port != derived_port)
            {
                logger.printLn(
                    "[CEF] Note: cef.port (%d) differs from network.port + %u (%d). Official CEF clients connect to gamePort + %u unless forwarded via NAT/proxy.",
                    configured_port,
                    static_cast<unsigned int>(port_offset),
                    derived_port,
                    static_cast<unsigned int>(port_offset));
            }
        }
        return static_cast<uint16_t>(configured_port);
    }

    std::string ResolveBindAddress(IConfig& config)
    {
        if (config.getType("cef.bind") == ConfigOptionType_String)
        {
            const std::string cef_bind = StringViewToString(config.getString("cef.bind"));
            if (!cef_bind.empty())
                return cef_bind;
        }

        if (config.getType("network.bind") == ConfigOptionType_String)
        {
            const std::string net_bind = StringViewToString(config.getString("network.bind"));
            if (!net_bind.empty())
                return net_bind;
        }

        return {};
    }

    uint32_t ResolveMinClientVersion(IConfig& config, ILogger& logger, bool log_messages)
    {
        if (config.getType("cef.min_client_version") != ConfigOptionType_String)
            return MIN_CLIENT_VERSION_U32;

        const std::string raw = StringViewToString(config.getString("cef.min_client_version"));
        if (raw.empty())
            return MIN_CLIENT_VERSION_U32;

        uint32_t parsed = 0;
        if (!ParseVersionU32(raw, parsed) || parsed < MIN_CLIENT_VERSION_U32)
        {
            if (log_messages)
            {
                logger.printLn(
                    "[CEF] Invalid cef.min_client_version '%s' (must be >= %u.%u.%u). Defaulting to %u.%u.%u.",
                    raw.c_str(),
                    MIN_CLIENT_VERSION_MAJOR,
                    MIN_CLIENT_VERSION_MINOR,
                    MIN_CLIENT_VERSION_PATCH,
                    MIN_CLIENT_VERSION_MAJOR,
                    MIN_CLIENT_VERSION_MINOR,
                    MIN_CLIENT_VERSION_PATCH);
            }
            return MIN_CLIENT_VERSION_U32;
        }

        return parsed;
    }
}

StringView CefOmpComponent::componentName() const
{
    return "Cef Component";
}

SemanticVersion CefOmpComponent::componentVersion() const
{
	return { VERSION_MAJOR, VERSION_MINOR, VERSION_PATCH, VERSION_BUILD };
}

void CefOmpComponent::onLoad(ICore* core)
{
    core_ = core;
	core_->getEventDispatcher().addEventHandler(this);
    core_->getPlayers().getPlayerConnectDispatcher().addEventHandler(this);
    setAmxLookups(core_);
}

void CefOmpComponent::onInit(IComponentList* components)
{
    plugin_ = std::make_unique<CefPlugin>();
    pawn_ = components->queryComponent<IPawnComponent>();

    if (pawn_)
    {
        setAmxFunctions(pawn_->getAmxFunctions());
		setAmxLookups(components);
        pawn_->getEventDispatcher().addEventHandler(this);
    }

    // Re-read final configuration from core after all components (e.g. LegacyConfig
    // or CLI arguments) have finished updating network.port / network.bind / cef.*.
    if (core_)
    {
        IConfig& config = core_->getConfig();
        int* port_ptr = config.getInt("network.port");
        const int port_value = (port_ptr && *port_ptr > 0 && *port_ptr <= 65535) ? *port_ptr : 7777;
        server_port_ = static_cast<uint16_t>(port_value);

        cef_network_port_ = ResolveCefNetworkPort(
            server_port_,
            cef_network_port_offset,
            default_cef_network_port,
            config,
            *core_,
            true);

        bind_address_ = ResolveBindAddress(config);
        min_client_version_ = ResolveMinClientVersion(config, *core_, true);
        strict_ip_check_ = config.getBool("cef.strict_ip_check") ? *config.getBool("cef.strict_ip_check") : true;
        debug_enabled_ = config.getBool("cef.debug") ? *config.getBool("cef.debug") : false;

        StringView key_sv = config.getString("cef.master_resource_key");
        size_t key_len = key_sv.length();
        if (key_len == 16 || key_len == 24 || key_len == 32)
        {
            master_resource_key_.assign(key_sv.data(), key_sv.data() + key_len);
        }

        resource_loader_ui_mode_ = ReadResourceLoaderMode(config, *core_);
        int* dialog_id = config.getInt("cef.resources_loader_dialog_id");
        resource_download_dialog_id_ = dialog_id ? *dialog_id : ResourceDownloadDialogManager::DefaultDialogId;
    }

    CefPluginOptions options;
    options.log_level = debug_enabled_ ? CefLogLevel::Debug : CefLogLevel::Info;
    options.master_resource_key = master_resource_key_;
    options.resources_loader_mode = resource_loader_ui_mode_;
    options.resources_loader_dialog_id = resource_download_dialog_id_;
    options.bind_address = bind_address_;
    options.min_client_version = min_client_version_;
    options.strict_ip_check = strict_ip_check_;

    auto bridge = CreateOmpPlatformBridge(core_, pawn_);
    if (!plugin_->Initialize(std::move(bridge), cef_network_port_, options))
    {
        if (core_)
        {
            core_->printLn("[CEF] [ERROR] Component failed to initialize CEF UDP server on port %u.",
                static_cast<unsigned int>(cef_network_port_));
        }
        return;
    }

    LOG_INFO("Component initialized (v%d.%d.%d, min client v%s).",
        VERSION_MAJOR, VERSION_MINOR, VERSION_PATCH, VersionToString(min_client_version_).c_str());
}

void CefOmpComponent::PublishQueryRules()
{
    if (!core_ || !plugin_ || !plugin_->IsRunning())
        return;

    const std::string port_str = std::to_string(cef_network_port_);
    bool published = false;

    const FlatPtrHashSet<INetwork>& networks = core_->getNetworks();
    for (INetwork* network : networks)
    {
        if (!network)
            continue;

        if (auto* query = queryExtension<INetworkQueryExtension>(network))
        {
            if (query->addRule(cef_query::kCefPortRuleName, port_str))
            {
                published = true;
            }
        }
    }

    if (published)
    {
        LOG_INFO("Published server query rule %s=%s for automatic client port detection.",
            cef_query::kCefPortRuleName, port_str.c_str());
    }
}

void CefOmpComponent::onReady()
{
    PublishQueryRules();
}

void CefOmpComponent::onFree(IComponent* component)
{
    if (component == pawn_)
    {
		if (plugin_)
		{
			plugin_->InvalidatePawnBridge();
		}
        pawn_ = nullptr;
    }
}

void CefOmpComponent::provideConfiguration(ILogger& logger, IEarlyConfig& config, bool defaults) 
{
    int* port_ptr = config.getInt("network.port");
    const int port_value = (port_ptr && *port_ptr > 0 && *port_ptr <= 65535) ? *port_ptr : 7777;
    
    server_port_ = static_cast<uint16_t>(port_value);

	if (defaults) {
		config.setBool("cef.debug", false);
		config.setString("cef.master_resource_key", "ThisIsA16ByteKey");
		config.setInt("cef.port", 0);
		config.setString("cef.bind", "");
		config.setString("cef.min_client_version", "1.3.0");
		config.setBool("cef.strict_ip_check", true);
        config.setString("cef.resources_loader_mode", "cef");
        config.setInt("cef.resources_loader_dialog_id", ResourceDownloadDialogManager::DefaultDialogId);
	}
	else {
		if (config.getType("cef.debug") == ConfigOptionType_None) {
			config.setBool("cef.debug", false);
		}

		if (config.getType("cef.master_resource_key") == ConfigOptionType_None) {
			config.setString("cef.master_resource_key", "ThisIsA16ByteKey");
		}

		if (config.getType("cef.port") == ConfigOptionType_None) {
			config.setInt("cef.port", 0);
		}

		if (config.getType("cef.bind") == ConfigOptionType_None) {
			config.setString("cef.bind", "");
		}

		if (config.getType("cef.min_client_version") == ConfigOptionType_None) {
			config.setString("cef.min_client_version", "1.3.0");
		}

		if (config.getType("cef.strict_ip_check") == ConfigOptionType_None) {
			config.setBool("cef.strict_ip_check", true);
		}

        if (config.getType("cef.resources_loader_mode") == ConfigOptionType_None &&
            config.getType("cef.resources_loader_ui") == ConfigOptionType_None) {
            config.setString("cef.resources_loader_mode", "cef");
        }

        if (config.getType("cef.resources_loader_dialog_id") == ConfigOptionType_None) {
            config.setInt("cef.resources_loader_dialog_id", ResourceDownloadDialogManager::DefaultDialogId);
        }
	}

    cef_network_port_ = ResolveCefNetworkPort(
        server_port_,
        cef_network_port_offset,
        default_cef_network_port,
        config,
        logger,
        false);

    bind_address_ = ResolveBindAddress(config);
    min_client_version_ = ResolveMinClientVersion(config, logger, false);
    strict_ip_check_ = config.getBool("cef.strict_ip_check") ? *config.getBool("cef.strict_ip_check") : true;
	debug_enabled_ = config.getBool("cef.debug") ? *config.getBool("cef.debug") : false;

	StringView key_sv = config.getString("cef.master_resource_key");
	size_t key_len = key_sv.length();

	if (key_len == 16 || key_len == 24 || key_len == 32) {
		master_resource_key_.assign(key_sv.data(), key_sv.data() + key_len);
		logger.printLn("[CEF Security] Master resource key loaded successfully (%zu bytes, for AES-%zu).", key_len, key_len * 8);
	}
	else {
		logger.printLn("===================================================================");
		logger.printLn("[CEF SECURITY ERROR] The 'cef.master_resource_key' is INVALID!");
		logger.printLn("[CEF SECURITY ERROR] Its length is %zu bytes, but it MUST be 16, 24, or 32 bytes.", key_len);
		logger.printLn("[CEF SECURITY ERROR] Resource encryption will fail. Please fix your configuration.");
		logger.printLn("===================================================================");
	}

    resource_loader_ui_mode_ = ReadResourceLoaderMode(config, logger);

    int* dialog_id = config.getInt("cef.resources_loader_dialog_id");
    resource_download_dialog_id_ = dialog_id ? *dialog_id : ResourceDownloadDialogManager::DefaultDialogId;
}

void CefOmpComponent::free()
{
    delete this;
}

void CefOmpComponent::reset()
{
    PublishQueryRules();
}

IExtension* CefOmpComponent::getExtension(UID id)
{
    if (id == ICefComponent::ExtensionIID)
    {
        return GetCefExtension();
    }
    return nullptr;
}

void CefOmpComponent::onAmxLoad(IPawnScript& script) 
{
    pawn_natives::AmxLoad(script.GetAMX());
}

void CefOmpComponent::onAmxUnload(IPawnScript& script) 
{

}

void CefOmpComponent::onTick(Microseconds elapsed, TimePoint now)
{
    (void)elapsed;
    (void)now;

    if (plugin_)
    {
        plugin_->ProcessMainThreadTasks();
    }
}

void CefOmpComponent::onPlayerConnect(IPlayer& player)
{
    if (plugin_)
        plugin_->OnPlayerConnect(player.getID());
}

void CefOmpComponent::onPlayerClientInit(IPlayer& player)
{
    if (plugin_)
        plugin_->OnPlayerClientInit(player.getID());
}

void CefOmpComponent::onPlayerDisconnect(IPlayer& player, PeerDisconnectReason reason) 
{
    (void)reason;
    if (plugin_)
	    plugin_->OnPlayerDisconnect(player.getID());
}

CefOmpComponent::~CefOmpComponent()
{
    if (pawn_)
    {
        pawn_->getEventDispatcher().removeEventHandler(this);
    }

    if (core_)
    {
		core_->getEventDispatcher().removeEventHandler(this);
        core_->getPlayers().getPlayerConnectDispatcher().removeEventHandler(this);
    }
}
