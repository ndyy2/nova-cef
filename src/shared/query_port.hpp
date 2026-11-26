#pragma once

#include <array>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <string>

namespace cef_query
{
    inline constexpr const char* kCefPortRuleName = "cef_port";
    inline constexpr size_t kSampQueryHeaderSize = 11;

    enum class QueryParseResult : uint8_t
    {
        Invalid = 0,
        NotFound = 1,
        Found = 2
    };

    inline std::array<char, kSampQueryHeaderSize> BuildSampRulesQuery(
        const std::array<unsigned char, 4>& ipv4_bytes,
        uint16_t game_port)
    {
        std::array<char, kSampQueryHeaderSize> pkt{};
        pkt[0] = 'S';
        pkt[1] = 'A';
        pkt[2] = 'M';
        pkt[3] = 'P';
        pkt[4] = static_cast<char>(ipv4_bytes[0]);
        pkt[5] = static_cast<char>(ipv4_bytes[1]);
        pkt[6] = static_cast<char>(ipv4_bytes[2]);
        pkt[7] = static_cast<char>(ipv4_bytes[3]);
        pkt[8] = static_cast<char>(game_port & 0xFF);
        pkt[9] = static_cast<char>((game_port >> 8) & 0xFF);
        pkt[10] = 'r';
        return pkt;
    }

    inline bool EqualsIgnoreCase(const char* a, size_t a_len, const char* b)
    {
        const size_t b_len = std::strlen(b);
        if (a_len != b_len)
            return false;

        for (size_t i = 0; i < a_len; ++i)
        {
            const auto ca = static_cast<unsigned char>(a[i]);
            const auto cb = static_cast<unsigned char>(b[i]);
            if (std::tolower(ca) != std::tolower(cb))
                return false;
        }
        return true;
    }

    inline bool ParseStrictPort(const char* s, size_t len, uint16_t& out_port)
    {
        if (!s || len == 0 || len > 5)
            return false;

        uint32_t value = 0;
        for (size_t i = 0; i < len; ++i)
        {
            const unsigned char c = static_cast<unsigned char>(s[i]);
            if (c < '0' || c > '9')
                return false;
            value = value * 10u + static_cast<uint32_t>(c - '0');
        }

        if (value < 1u || value > 65535u)
            return false;

        out_port = static_cast<uint16_t>(value);
        return true;
    }

    // Parses a SA-MP / open.mp 'r' (rules) query reply and extracts the `cef_port` rule.
    inline QueryParseResult ParseSampRulesCefPort(const char* data, size_t len, uint16_t& out_port)
    {
        out_port = 0;

        if (!data || len < kSampQueryHeaderSize + 2)
            return QueryParseResult::Invalid;

        if (data[0] != 'S' || data[1] != 'A' || data[2] != 'M' || data[3] != 'P')
            return QueryParseResult::Invalid;

        if (data[10] != 'r')
            return QueryParseResult::Invalid;

        const uint16_t rule_count = static_cast<uint16_t>(
            static_cast<uint8_t>(data[11]) |
            (static_cast<uint16_t>(static_cast<uint8_t>(data[12])) << 8));

        uint16_t found_port = 0;
        size_t pos = kSampQueryHeaderSize + 2;
        for (uint16_t i = 0; i < rule_count; ++i)
        {
            if (pos >= len)
                return QueryParseResult::Invalid;

            const uint8_t name_len = static_cast<uint8_t>(data[pos++]);
            if (pos + name_len > len)
                return QueryParseResult::Invalid;
            const char* name_ptr = data + pos;
            pos += name_len;

            if (pos >= len)
                return QueryParseResult::Invalid;

            const uint8_t val_len = static_cast<uint8_t>(data[pos++]);
            if (pos + val_len > len)
                return QueryParseResult::Invalid;
            const char* val_ptr = data + pos;
            pos += val_len;

            if (found_port == 0 &&
                (EqualsIgnoreCase(name_ptr, name_len, kCefPortRuleName) ||
                 EqualsIgnoreCase(name_ptr, name_len, "cef.port")))
            {
                uint16_t parsed_port = 0;
                if (ParseStrictPort(val_ptr, val_len, parsed_port))
                {
                    found_port = parsed_port;
                }
            }
        }

        if (found_port != 0)
        {
            out_port = found_port;
            return QueryParseResult::Found;
        }

        return QueryParseResult::NotFound;
    }
}
