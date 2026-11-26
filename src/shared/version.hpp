#pragma once

#include <cstdint>
#include <cstdio>
#include <string>

#define VERSION_MAJOR 1
#define VERSION_MINOR 4
#define VERSION_PATCH 3
#define VERSION_BUILD 0

#define MIN_CLIENT_VERSION_MAJOR 1
#define MIN_CLIENT_VERSION_MINOR 3
#define MIN_CLIENT_VERSION_PATCH 0
#define MIN_CLIENT_VERSION_BUILD 0

static constexpr uint32_t MakeVersionU32(uint8_t maj, uint8_t min, uint8_t patch, uint8_t build)
{
    return (uint32_t(maj) << 24) | (uint32_t(min) << 16) | (uint32_t(patch) << 8) | uint32_t(build);
}

static constexpr uint32_t PLUGIN_VERSION_U32 =
    MakeVersionU32(VERSION_MAJOR, VERSION_MINOR, VERSION_PATCH, VERSION_BUILD);

static constexpr uint32_t MIN_CLIENT_VERSION_U32 =
    MakeVersionU32(MIN_CLIENT_VERSION_MAJOR, MIN_CLIENT_VERSION_MINOR, MIN_CLIENT_VERSION_PATCH, MIN_CLIENT_VERSION_BUILD);

static constexpr uint32_t BROWSER_LAYER_MIN_VERSION_U32 =
    MakeVersionU32(1, 4, 0, 0);

static constexpr uint32_t KCP_MANIFEST_MIN_VERSION_U32 =
    MakeVersionU32(1, 4, 3, 0);

static constexpr bool IsClientVersionSupported(uint32_t client_version, uint32_t min_version = MIN_CLIENT_VERSION_U32)
{
    const uint8_t client_major = static_cast<uint8_t>((client_version >> 24) & 0xFF);
    return client_major == VERSION_MAJOR && client_version >= min_version;
}

static inline bool ParseVersionU32(const std::string& text, uint32_t& out)
{
    unsigned int maj = 0;
    unsigned int min = 0;
    unsigned int patch = 0;
    unsigned int build = 0;

    const int parsed = std::sscanf(text.c_str(), "%u.%u.%u.%u", &maj, &min, &patch, &build);
    if (parsed < 3 || maj > 255 || min > 255 || patch > 255 || build > 255)
        return false;

    out = MakeVersionU32(
        static_cast<uint8_t>(maj),
        static_cast<uint8_t>(min),
        static_cast<uint8_t>(patch),
        static_cast<uint8_t>(parsed >= 4 ? build : 0));
    return true;
}

static inline std::string VersionToString(uint32_t v)
{
    const uint8_t maj   = uint8_t((v >> 24) & 0xFF);
    const uint8_t min   = uint8_t((v >> 16) & 0xFF);
    const uint8_t patch = uint8_t((v >> 8) & 0xFF);
    const uint8_t build = uint8_t((v >> 0) & 0xFF);

    char buf[32]{};
    std::snprintf(buf, sizeof(buf), "%u.%u.%u.%u", maj, min, patch, build);
    return std::string(buf);
}