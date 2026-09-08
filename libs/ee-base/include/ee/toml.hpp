// SPDX-License-Identifier: GPL-3.0-only
#pragma once

// Minimal TOML subset parser — covers the PS2Recomp-compatible config schema:
//   [table], key = "string" | bool | int (dec/hex) | ["strings"] | { "k" = int, ... }

#include <ee/text.hpp>
#include <ee/types.hpp>

#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace ee {

struct TomlValue {
    using StringArray = std::vector<std::string>;
    using IntTable = std::map<std::string, s64>;
    std::variant<std::string, s64, bool, StringArray, IntTable> data;

    std::string as_string(const std::string& fallback = {}) const {
        if (auto* v = std::get_if<std::string>(&data))
            return *v;
        return fallback;
    }
    s64 as_int(s64 fallback = 0) const {
        if (auto* v = std::get_if<s64>(&data))
            return *v;
        return fallback;
    }
    bool as_bool(bool fallback = false) const {
        if (auto* v = std::get_if<bool>(&data))
            return *v;
        return fallback;
    }
    const StringArray* as_string_array() const { return std::get_if<StringArray>(&data); }
    const IntTable* as_int_table() const { return std::get_if<IntTable>(&data); }
};

struct TomlDoc {
    std::map<std::string, std::map<std::string, TomlValue>> tables;

    const TomlValue* get(const std::string& table, const std::string& key) const {
        auto t = tables.find(table);
        if (t == tables.end())
            return nullptr;
        auto k = t->second.find(key);
        return k != t->second.end() ? &k->second : nullptr;
    }
};

namespace detail {

// Strip a trailing comment, respecting double-quoted strings.
inline std::string_view toml_strip_comment(std::string_view line) {
    bool in_string = false;
    for (size_t i = 0; i < line.size(); ++i) {
        const char c = line[i];
        if (c == '"' && (i == 0 || line[i - 1] != '\\'))
            in_string = !in_string;
        if (c == '#' && !in_string)
            return line.substr(0, i);
    }
    return line;
}

inline std::optional<std::string> toml_string(std::string_view& s) {
    if (s.empty() || s.front() != '"')
        return std::nullopt;
    s.remove_prefix(1);
    std::string out;
    while (!s.empty() && s.front() != '"') {
        char c = s.front();
        s.remove_prefix(1);
        if (c == '\\' && !s.empty()) {
            const char e = s.front();
            s.remove_prefix(1);
            switch (e) {
            case 'n': out += '\n'; break;
            case 't': out += '\t'; break;
            case '"': out += '"'; break;
            case '\\': out += '\\'; break;
            default: out += e; break;
            }
        } else {
            out += c;
        }
    }
    if (s.empty())
        return std::nullopt;
    s.remove_prefix(1); // closing quote
    return out;
}

inline std::optional<s64> toml_int(std::string_view sv) {
    sv = trim_sv(sv);
    if (sv.empty())
        return std::nullopt;
    bool neg = false;
    if (sv.front() == '-') {
        neg = true;
        sv.remove_prefix(1);
    }
    // hex?
    u32 base = 10;
    if (sv.size() > 2 && sv[0] == '0' && (sv[1] == 'x' || sv[1] == 'X')) {
        base = 16;
        sv.remove_prefix(2);
    }
    if (sv.empty())
        return std::nullopt;
    u64 value = 0;
    for (char c : sv) {
        if (c == '_')
            continue;
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
        if (value > 0xFFFFFFFFFFFFFFFFull / 2)
            return std::nullopt;
    }
    return neg ? -static_cast<s64>(value) : static_cast<s64>(value);
}

inline std::optional<TomlValue> toml_value(std::string_view sv) {
    sv = trim_sv(sv);
    if (sv.empty())
        return std::nullopt;
    if (sv.front() == '"') {
        auto str = toml_string(sv);
        if (!str)
            return std::nullopt;
        return TomlValue{std::move(*str)};
    }
    if (sv.front() == '[') {
        // array of strings (only form we need)
        TomlValue::StringArray arr;
        sv.remove_prefix(1);
        while (true) {
            sv = trim_sv(sv);
            if (!sv.empty() && sv.front() == ']')
                return TomlValue{std::move(arr)};
            auto str = toml_string(sv);
            if (!str)
                return std::nullopt;
            arr.push_back(std::move(*str));
            sv = trim_sv(sv);
            if (!sv.empty() && sv.front() == ',') {
                sv.remove_prefix(1);
                continue;
            }
            if (!sv.empty() && sv.front() == ']')
                return TomlValue{std::move(arr)};
            return std::nullopt;
        }
    }
    if (sv.front() == '{') {
        // inline table of string = int
        TomlValue::IntTable tbl;
        sv.remove_prefix(1);
        while (true) {
            sv = trim_sv(sv);
            if (!sv.empty() && sv.front() == '}')
                return TomlValue{std::move(tbl)};
            auto key = toml_string(sv);
            if (!key)
                return std::nullopt;
            sv = trim_sv(sv);
            if (sv.empty() || sv.front() != '=')
                return std::nullopt;
            sv.remove_prefix(1);
            // find value end (',' or '}')
            const size_t end = sv.find_first_of(",}");
            if (end == std::string_view::npos)
                return std::nullopt;
            auto val = toml_int(sv.substr(0, end));
            if (!val)
                return std::nullopt;
            tbl[*key] = *val;
            sv.remove_prefix(end);
            if (sv.front() == ',') {
                sv.remove_prefix(1);
                continue;
            }
            if (sv.front() == '}')
                return TomlValue{std::move(tbl)};
            return std::nullopt;
        }
    }
    if (sv.substr(0, 4) == "true")
        return TomlValue{true};
    if (sv.substr(0, 5) == "false")
        return TomlValue{false};
    if (auto i = toml_int(sv))
        return TomlValue{*i};
    return std::nullopt;
}

} // namespace detail

inline std::optional<TomlDoc> toml_parse(std::string_view text, std::string* error = nullptr) {
    TomlDoc doc;
    std::string current;
    size_t line_no = 0;
    for (const std::string& raw_line : split(text, '\n')) {
        ++line_no;
        const std::string_view line = trim_sv(detail::toml_strip_comment(raw_line));
        if (line.empty())
            continue;
        if (line.front() == '[') {
            if (line.size() < 3 || line.back() != ']') {
                if (error)
                    *error = "bad table header at line " + std::to_string(line_no);
                return std::nullopt;
            }
            current = trim(line.substr(1, line.size() - 2));
            continue;
        }
        const size_t eq = line.find('=');
        if (eq == std::string_view::npos) {
            if (error)
                *error = "expected key = value at line " + std::to_string(line_no);
            return std::nullopt;
        }
        const std::string key = trim(line.substr(0, eq));
        auto value = detail::toml_value(line.substr(eq + 1));
        if (!value) {
            if (error)
                *error = "bad value at line " + std::to_string(line_no);
            return std::nullopt;
        }
        doc.tables[current][key] = std::move(*value);
    }
    return doc;
}

} // namespace ee
