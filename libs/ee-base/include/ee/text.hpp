// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <ee/types.hpp>

#include <cstdio>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace ee {

inline std::string_view trim_sv(std::string_view s) {
    const auto is_space = [](char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; };
    while (!s.empty() && is_space(s.front()))
        s.remove_prefix(1);
    while (!s.empty() && is_space(s.back()))
        s.remove_suffix(1);
    return s;
}

inline std::string trim(std::string_view s) { return std::string(trim_sv(s)); }

inline std::vector<std::string> split(std::string_view s, char delim) {
    std::vector<std::string> out;
    size_t pos = 0;
    while (pos <= s.size()) {
        const size_t next = s.find(delim, pos);
        if (next == std::string_view::npos) {
            out.emplace_back(s.substr(pos));
            break;
        }
        out.emplace_back(s.substr(pos, next - pos));
        pos = next + 1;
    }
    return out;
}

// Parse a u32 from "0x1A2B" (hex) or "1234" (decimal).
inline std::optional<u32> parse_u32(std::string_view sv) {
    sv = trim_sv(sv);
    if (sv.empty())
        return std::nullopt;
    u32 base = 10;
    if (sv.size() > 2 && sv[0] == '0' && (sv[1] == 'x' || sv[1] == 'X')) {
        base = 16;
        sv.remove_prefix(2);
    }
    if (sv.empty())
        return std::nullopt;
    u64 value = 0;
    for (char c : sv) {
        int d;
        if (c >= '0' && c <= '9')
            d = c - '0';
        else if (c >= 'a' && c <= 'f')
            d = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F')
            d = c - 'A' + 10;
        else
            return std::nullopt;
        if (u32(d) >= base)
            return std::nullopt;
        value = value * base + u32(d);
        if (value > 0xFFFFFFFF)
            return std::nullopt;
    }
    return u32(value);
}

inline std::string hex32(u32 v) {
    char buf[16];
    std::snprintf(buf, sizeof buf, "0x%08X", v);
    return buf;
}

} // namespace ee
