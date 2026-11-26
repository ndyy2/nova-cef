#include <malloc.h>

#include <algorithm>
#include <cctype>
#include <string>

#include "amx/amx.h"
#include "plugin.h"
#include <pawn-natives/NativeFunc.hpp>
#include <pawn-natives/NativesMain.hpp>
#include <common/plugin.hpp>
#include <common/logger.hpp>
#include <shared/version.hpp>
#include "samp_bridge.hpp"
#include "config_cfg.hpp"

#define SAMPGDK_STATIC
#include <sampgdk.h>

extern void* pAMXFunctions;
std::vector<AMX*> g_AmxList;
std::unique_ptr<CefPlugin> plugin_;

namespace
{
    std::string NormalizeConfigValue(std::string value)
    {
        const auto begin = std::find_if_not(value.begin(), value.end(), [](unsigned char c)
        {
            return std::isspace(c) != 0;
        });

        const auto end = std::find_if_not(value.rbegin(), value.rend(), [](unsigned char c)
        {
            return std::isspace(c) != 0;
        }).base();

        if (begin >= end)
            return {};

        value = std::string(begin, end);
        std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c)
        {
            return static_cast<char>(std::tolower(c));
        });

        return value;
    }

    ResourceLoaderUiMode ReadResourceLoaderMode(const ConfigCfg& config)
    {
        const bool hasMode = config.Has("cef_resources_loader_mode");
        const std::string rawValue = hasMode
            ? config.GetString("cef_resources_loader_mode", "cef")
            : config.GetString("cef_resources_loader_ui", "1");

        const std::string value = NormalizeConfigValue(rawValue);

        if (value == "cef" || value == "1")
            return ResourceLoaderUiMode::Cef;

        if (value == "samp_dialog")
            return ResourceLoaderUiMode::SampDialog;

        if (value == "none" || value == "0")
            return ResourceLoaderUiMode::None;

        sampgdk::logprintf(
            "[CEF] [WARN] Invalid resource loader mode '%s'. Expected 'cef', 'samp_dialog' or 'none'. Defaulting to 'cef'.",
            rawValue.c_str());

        return ResourceLoaderUiMode::Cef;
    }
}

PLUGIN_EXPORT unsigned int PLUGIN_CALL Supports()
{
    return sampgdk::Supports() | SUPPORTS_VERSION | SUPPORTS_AMX_NATIVES | SUPPORTS_PROCESS_TICK;
}

PLUGIN_EXPORT void PLUGIN_CALL ProcessTick()
{
    sampgdk::ProcessTick();

    if (plugin_)
        plugin_->ProcessMainThreadTasks();
}

PLUGIN_EXPORT bool PLUGIN_CALL Load(void** ppData)
{
    pAMXFunctions = ppData[PLUGIN_DATA_AMX_EXPORTS];
    
    bool load = sampgdk::Load(ppData);

    plugin_ = std::make_unique<CefPlugin>();

    ConfigCfg config;
    config.Load("server.cfg");

    const int server_port = config.GetPort(7777);
    const int cef_network_port = config.GetCefUdpPort(2);
    const std::string bind_address = config.Has("cef_bind")
        ? config.GetString("cef_bind", "")
        : config.GetString("bind", "");

    const bool strict_ip_check = config.GetInt("cef_strict_ip_check", 1) != 0;
    const int init_timeout_seconds = config.GetInt("cef_init_timeout", 15);

    uint32_t min_client_version = MIN_CLIENT_VERSION_U32;
    const std::string min_ver_str = config.GetString("cef_min_client_version", "1.3.0");
    if (!min_ver_str.empty())
    {
        uint32_t parsed = 0;
        if (ParseVersionU32(min_ver_str, parsed) && parsed >= MIN_CLIENT_VERSION_U32)
        {
            min_client_version = parsed;
        }
        else
        {
            sampgdk::logprintf(
                "[CEF] [WARN] Invalid cef_min_client_version '%s'. Defaulting to %u.%u.%u.",
                min_ver_str.c_str(),
                MIN_CLIENT_VERSION_MAJOR,
                MIN_CLIENT_VERSION_MINOR,
                MIN_CLIENT_VERSION_PATCH);
        }
    }

    const int derived_port = server_port + 2;
    if (cef_network_port != derived_port)
    {
        sampgdk::logprintf(
            "[CEF] Using configured UDP port %d (cef_port). Note: official CEF clients connect to gamePort + 2 (%d) unless forwarded via NAT/proxy.",
            cef_network_port,
            derived_port);
    }

    std::string debug_str = config.GetString("cef_debug", "0");
    bool debug_enabled_ = (std::stoi(debug_str) != 0);

    std::string master_key_str = config.GetString("cef_master_resource_key", "ThisIsA16ByteKey");

    std::vector<uint8_t> master_key(master_key_str.begin(), master_key_str.end());
    if (master_key.size() < 16) 
        master_key.resize(16, 0);

    if (master_key.size() > 16) 
        master_key.resize(16);

    const ResourceLoaderUiMode resource_loader_mode = ReadResourceLoaderMode(config);
    const int resource_download_dialog_id = config.GetInt("cef_resources_loader_dialog_id", ResourceDownloadDialogManager::DefaultDialogId);

    CefPluginOptions options;
    options.log_level = debug_enabled_ ? CefLogLevel::Debug : CefLogLevel::Info;
    options.master_resource_key = master_key;
    options.resources_loader_mode = resource_loader_mode;
    options.resources_loader_dialog_id = resource_download_dialog_id;
    options.bind_address = bind_address;
    options.min_client_version = min_client_version;
    options.strict_ip_check = strict_ip_check;
    options.init_timeout_seconds = init_timeout_seconds;

    auto bridge = CreateSampPlatformBridge();
    if (!plugin_->Initialize(std::move(bridge), cef_network_port, options))
    {
        sampgdk::logprintf("[CEF] [ERROR] Failed to initialize CEF network server on port %d.", cef_network_port);
        return false;
    }

    LOG_INFO("Plugin loaded (v%d.%d.%d)", VERSION_MAJOR, VERSION_MINOR, VERSION_PATCH);

    return true;
}

PLUGIN_EXPORT void PLUGIN_CALL Unload()
{
	plugin_->Shutdown();
    sampgdk::Unload();
}

PLUGIN_EXPORT int PLUGIN_CALL AmxLoad(AMX* amx)
{
    g_AmxList.push_back(amx);
    return pawn_natives::AmxLoad(amx);
}

PLUGIN_EXPORT int PLUGIN_CALL AmxUnload(AMX* amx)
{
    g_AmxList.erase(std::remove(g_AmxList.begin(), g_AmxList.end(), amx), g_AmxList.end());
    return AMX_ERR_NONE;
}


PLUGIN_EXPORT bool PLUGIN_CALL OnPlayerConnect(int playerid)
{
	plugin_->OnPlayerConnect(playerid);
    return true;
}

PLUGIN_EXPORT bool PLUGIN_CALL OnPlayerDisconnect(int playerid, int reason)
{
    (void)reason;
    if (plugin_)
        plugin_->OnPlayerDisconnect(playerid);
    return true;
}

// SA-MP doesn't have an "OnPlayerClientInit" callback like open.mp.
// We use early client lifecycle callbacks (class selection) to start
// the handshake timeout logic and detect players without the CEF client.
static inline void MaybeNotifyClientInit(int playerid)
{
    if (!plugin_)
        return;

    plugin_->OnPlayerClientInit(playerid);
}

PLUGIN_EXPORT bool PLUGIN_CALL OnPlayerRequestClass(int playerid, int classid)
{
    (void)classid;
    MaybeNotifyClientInit(playerid);
    return true;
}