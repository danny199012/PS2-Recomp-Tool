// SPDX-License-Identifier: GPL-3.0-only
#include <ee/game_overrides.hpp>

#include <cstdio>

namespace ee::rt {

GameOverrideRegistry& GameOverrideRegistry::instance() {
    static GameOverrideRegistry inst;
    return inst;
}

void GameOverrideRegistry::register_override(const std::string& elf_name, u32 crc32,
                                             GameOverride::ApplyFunc apply) {
    m_overrides.push_back({elf_name, crc32, std::move(apply)});
}

void GameOverrideRegistry::apply_for_game(Runtime& rt, const std::string& elf_name, u32 crc32) {
    for (const auto& ov : m_overrides) {
        std::string a = ov.elf_name, b = elf_name;
        for (char& c : a) c = char(std::toupper(c));
        for (char& c : b) c = char(std::toupper(c));
        if (!a.empty() && a != b) continue;
        if (ov.crc32 != 0 && ov.crc32 != crc32) continue;
        std::fprintf(stderr, "[overrides] applying game override for %s (crc=0x%08X)\n",
                     elf_name.c_str(), crc32);
        ov.apply(rt);
    }
}

void GameOverrideRegistry::bind_address_handler(Runtime& rt, u32 addr, const std::string& handler_name) {
    // Note: Function is a raw function pointer, not std::function.
    // Game overrides that need lambdas should register stubs directly.
    if (auto it = rt.stubs.find(handler_name); it != rt.stubs.end()) {
        rt.add(addr, it->second);
    }
}

} // namespace ee::rt
