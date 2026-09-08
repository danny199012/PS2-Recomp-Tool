// SPDX-License-Identifier: GPL-3.0-only
#pragma once

// Per-game override system: matches games by ELF name and CRC32, and allows
// binding function addresses to runtime stub handlers. Mirrors PS2Recomp's
// game_overrides.h API (PS2_REGISTER_GAME_OVERRIDE).

#include <ee/runtime.hpp>

#include <functional>
#include <string>
#include <vector>

namespace ee::rt {

struct GameOverride {
    std::string elf_name;
    u32 crc32 = 0;
    using ApplyFunc = std::function<void(Runtime&)>;
    ApplyFunc apply;
};

class GameOverrideRegistry {
public:
    static GameOverrideRegistry& instance();
    void register_override(const std::string& elf_name, u32 crc32,
                           GameOverride::ApplyFunc apply);
    void apply_for_game(Runtime& rt, const std::string& elf_name, u32 crc32);
    static void bind_address_handler(Runtime& rt, u32 addr, const std::string& handler_name);
    static void ret0(EEContext& ctx) { set32(ctx, 2, 0); }
    static void ret1(EEContext& ctx) { set32(ctx, 2, 1); }
    static void reta0(EEContext& ctx) { set32(ctx, 2, gpr32(ctx, 4)); }

private:
    std::vector<GameOverride> m_overrides;
};

#define EE_REGISTER_GAME_OVERRIDE(name, elfName, crc, applyFn)                   \
    static struct _ee_game_override_##name {                                      \
        _ee_game_override_##name() {                                              \
            ee::rt::GameOverrideRegistry::instance().register_override(           \
                elfName, crc, applyFn);                                            \
        }                                                                         \
    } _ee_game_override_##name##_inst;

} // namespace ee::rt
