// SPDX-License-Identifier: GPL-3.0-only
#pragma once

// Minimal JSON parser — just enough for tool interchange (Aura exports, configs).

#include <ee/types.hpp>

#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace ee {

struct JsonValue;
using JsonArray = std::vector<JsonValue>;
using JsonObject = std::map<std::string, JsonValue>;

struct JsonValue {
    std::variant<std::nullptr_t, bool, double, std::string, JsonArray, JsonObject> data;

    bool is_number() const { return std::holds_alternative<double>(data); }
    bool is_string() const { return std::holds_alternative<std::string>(data); }
    bool is_array() const { return std::holds_alternative<JsonArray>(data); }
    bool is_object() const { return std::holds_alternative<JsonObject>(data); }

    double as_number(double fallback = 0.0) const {
        if (auto* v = std::get_if<double>(&data))
            return *v;
        return fallback;
    }
    std::string as_string(const std::string& fallback = {}) const {
        if (auto* v = std::get_if<std::string>(&data))
            return *v;
        return fallback;
    }
    const JsonArray* as_array() const { return std::get_if<JsonArray>(&data); }
    const JsonObject* as_object() const { return std::get_if<JsonObject>(&data); }

    const JsonValue* find(const std::string& key) const {
        if (auto* obj = std::get_if<JsonObject>(&data)) {
            auto it = obj->find(key);
            if (it != obj->end())
                return &it->second;
        }
        return nullptr;
    }
};

namespace detail {
struct JsonParser {
    std::string_view s;
    size_t pos = 0;

    void ws() {
        while (pos < s.size() && (s[pos] == ' ' || s[pos] == '\t' || s[pos] == '\r' || s[pos] == '\n'))
            ++pos;
    }
    bool expect(char c) {
        ws();
        if (pos < s.size() && s[pos] == c) {
            ++pos;
            return true;
        }
        return false;
    }
    bool literal(std::string_view lit) {
        if (s.substr(pos, lit.size()) == lit) {
            pos += lit.size();
            return true;
        }
        return false;
    }
    std::optional<JsonValue> value() {
        ws();
        if (pos >= s.size())
            return std::nullopt;
        const char c = s[pos];
        if (c == '{')
            return object();
        if (c == '[')
            return array();
        if (c == '"')
            return string();
        if (literal("true"))
            return JsonValue{true};
        if (literal("false"))
            return JsonValue{false};
        if (literal("null"))
            return JsonValue{nullptr};
        return number();
    }
    std::optional<JsonValue> object() {
        JsonObject obj;
        ++pos; // {
        ws();
        if (pos < s.size() && s[pos] == '}') {
            ++pos;
            return JsonValue{std::move(obj)};
        }
        while (true) {
            ws();
            auto key = string_raw();
            if (!key || !expect(':'))
                return std::nullopt;
            auto val = value();
            if (!val)
                return std::nullopt;
            obj.emplace(std::move(*key), std::move(*val));
            if (expect(','))
                continue;
            if (expect('}'))
                return JsonValue{std::move(obj)};
            return std::nullopt;
        }
    }
    std::optional<JsonValue> array() {
        JsonArray arr;
        ++pos; // [
        ws();
        if (pos < s.size() && s[pos] == ']') {
            ++pos;
            return JsonValue{std::move(arr)};
        }
        while (true) {
            auto val = value();
            if (!val)
                return std::nullopt;
            arr.push_back(std::move(*val));
            if (expect(','))
                continue;
            if (expect(']'))
                return JsonValue{std::move(arr)};
            return std::nullopt;
        }
    }
    std::optional<std::string> string_raw() {
        if (pos >= s.size() || s[pos] != '"')
            return std::nullopt;
        ++pos;
        std::string out;
        while (pos < s.size() && s[pos] != '"') {
            char c = s[pos++];
            if (c == '\\' && pos < s.size()) {
                const char e = s[pos++];
                switch (e) {
                case 'n': out += '\n'; break;
                case 't': out += '\t'; break;
                case 'r': out += '\r'; break;
                case 'u': // skip 4 hex digits, emit '?'
                    if (pos + 4 > s.size())
                        return std::nullopt;
                    pos += 4;
                    out += '?';
                    break;
                default: out += e; break;
                }
            } else {
                out += c;
            }
        }
        if (pos >= s.size())
            return std::nullopt;
        ++pos; // closing quote
        return out;
    }
    std::optional<JsonValue> string() {
        auto str = string_raw();
        if (!str)
            return std::nullopt;
        return JsonValue{std::move(*str)};
    }
    std::optional<JsonValue> number() {
        const size_t start = pos;
        if (pos < s.size() && (s[pos] == '-' || s[pos] == '+'))
            ++pos;
        bool any = false;
        while (pos < s.size() && ((s[pos] >= '0' && s[pos] <= '9') || s[pos] == '.' || s[pos] == 'e' ||
                                  s[pos] == 'E' || s[pos] == '+' || s[pos] == '-')) {
            ++pos;
            any = true;
        }
        if (!any)
            return std::nullopt;
        try {
            return JsonValue{std::stod(std::string(s.substr(start, pos - start)))};
        } catch (...) {
            return std::nullopt;
        }
    }
};
} // namespace detail

inline std::optional<JsonValue> json_parse(std::string_view text) {
    detail::JsonParser p{text};
    auto v = p.value();
    if (!v)
        return std::nullopt;
    p.ws();
    if (p.pos != text.size())
        return std::nullopt;
    return v;
}

} // namespace ee
