// SPDX-License-Identifier: GPL-3.0-only
#pragma once

// GS software renderer: rasterizes GIF draw calls into GS VRAM and provides
// framebuffer readback for display (via DISPFB registers). Designed to be
// presented through SDL3 (ee-studio) or any windowing system.

#include <ee/hw.hpp>
#include <ee/types.hpp>

#include <cstdint>
#include <cstring>
#include <vector>

namespace ee::rt {

class GsRenderer {
public:
    explicit GsRenderer(Gs& gs) : m_gs(gs) {}

    // Framebuffer dimensions for presentation.
    u32 fb_width = 640;
    u32 fb_height = 480;

    // Render: process the GS register state to draw a frame.
    void render_frame();

    // Read back the display framebuffer as RGBA8 (32-bit per pixel).
    // Uses DISPFB1/DISPLAY1 (or DISPFB2/DISPLAY2) to find the source.
    // Returns a buffer of fb_width * fb_height * 4 bytes.
    std::vector<u8> read_framebuffer_rgba();

    // Draw a triangle (PSM_CT32 format in VRAM).
    void draw_triangle(float x0, float y0, float x1, float y1, float x2, float y2,
                        u32 color0, u32 color1, u32 color2);

    // Clear the framebuffer region.
    void clear(u32 x, u32 y, u32 w, u32 h, u32 color);

    // Process pending GIF data that has been written to GS registers.
    void flush_draws();

    // Convert PS2 GS PSM format to RGBA8 for an entire VRAM region.
    void vram_to_rgba(u32 dbp, u32 dbw, u32 psm, u32 dw, u32 dh, std::vector<u8>& out);

    // Draw a filled rectangle (sprite / XRATIO).
    void draw_sprite(float x, float y, float w, float h, u32 color);

    // Pending draw queue: vertices accumulate from GIF XYZ2/XYZF2 writes.
    struct Vertex { float x, y, z; u32 rgba; };
    std::vector<Vertex> m_vertices;
    u32 m_prim_type = 0; // PRIM register: 0=point,1=line,2=line strip,3=triangle,4=tri strip,5=tri fan,6=sprite

    // After vertices are written, call this to rasterize based on prim_type.
    void draw_pending();

private:
    Gs& m_gs;

    u32 read_vram_pixel(u32 addr) const;
    void write_vram_pixel(u32 addr, u32 rgba);

    // Simple point rasterization.
    void draw_point(int x, int y, u32 color);
    // Bresenham line.
    void draw_line(int x0, int y0, int x1, int y1, u32 color);
    // Filled triangle (barycentric).
    void draw_tri_filled(float x0, float y0, float x1, float y1, float x2, float y2,
                         u32 c0, u32 c1, u32 c2);
    // Filled rectangle.
    void draw_rect_filled(int x, int y, int w, int h, u32 color);

    // Get current framebuffer base from GS DISPFB register.
    u32 get_display_base() const;
    u32 get_display_psm() const;
    u32 get_display_width() const;
    u32 get_display_height() const;
};

} // namespace ee::rt
