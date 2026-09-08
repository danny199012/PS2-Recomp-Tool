// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <cstdint>

namespace ee {

using u8 = std::uint8_t;
using u16 = std::uint16_t;
using u32 = std::uint32_t;
using u64 = std::uint64_t;
using s8 = std::int8_t;
using s16 = std::int16_t;
using s32 = std::int32_t;
using s64 = std::int64_t;

// 128-bit value (R5900 GPRs/LO/HI, VU registers). Layout matches little-endian
// guest memory: lo is the lower 64 bits.
struct u128 {
    u64 lo = 0;
    u64 hi = 0;

    bool operator==(const u128&) const = default;
};

static_assert(sizeof(u128) == 16);

} // namespace ee
