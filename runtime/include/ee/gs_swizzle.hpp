// SPDX-License-Identifier: GPL-3.0-only
#pragma once

// GS VRAM swizzle: converts between linear addresses and the PS2 GS block-based
// VRAM layout. The GS uses a page-based structure where each page is 64x32
// pixels (for CT32), divided into 8x8 blocks, each 8x4 pixels.
// Reference: PCSX2 GS.h / GS transfer logic.

#include <ee/types.hpp>
#include <cstring>

namespace ee::rt {
namespace gs {

// Page dimensions in pixels (for PSM_CT32).
static constexpr u32 kPageWidth = 64;
static constexpr u32 kPageHeight = 32;
static constexpr u32 kBlockWidth = 8;
static constexpr u32 kBlockHeight = 8;

// Convert (x, y, dbw) to a byte offset in VRAM for PSM_CT32 (32-bit).
// dbw = display buffer width in pixels (must be multiple of 64).
inline u32 addr_ct32(u32 x, u32 y, u32 dbw) {
    u32 page_x = x / kPageWidth;
    u32 page_y = y / kPageHeight;
    u32 pages_per_row = dbw / kPageWidth;
    u32 page_idx = page_y * pages_per_row + page_x;
    u32 bx = (x % kPageWidth) / kBlockWidth;
    u32 by = (y % kPageHeight) / kBlockHeight;
    u32 block_idx = by * (kPageWidth / kBlockWidth) + bx;
    u32 px = x % kBlockWidth;
    u32 py = y % kBlockHeight;
    u32 pixel_idx = (px / 2) * 8 + (py * 2) + (px % 2);
    return (page_idx * kPageWidth * kPageHeight + block_idx * kBlockWidth * kBlockHeight +
            pixel_idx) * 4;
}

// Read a CT32 pixel from VRAM.
inline u32 read_pixel_ct32(const u8* vram, u32 x, u32 y, u32 dbw, u32 vram_size) {
    u32 addr = addr_ct32(x, y, dbw);
    if (addr + 4 > vram_size) return 0;
    u32 v;
    std::memcpy(&v, vram + addr, 4);
    return v;
}

// Write a CT32 pixel to VRAM.
inline void write_pixel_ct32(u8* vram, u32 x, u32 y, u32 dbw, u32 vram_size, u32 pixel) {
    u32 addr = addr_ct32(x, y, dbw);
    if (addr + 4 > vram_size) return;
    std::memcpy(vram + addr, &pixel, 4);
}

} // namespace gs
} // namespace ee::rt
