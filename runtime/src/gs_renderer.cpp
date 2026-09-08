// SPDX-License-Identifier: GPL-3.0-only
#include <ee/gs_renderer.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace ee::rt {

// GS PSM (pixel storage format) constants.
static constexpr u32 PSM_CT32 = 0x00;  // 32-bit RGBA
static constexpr u32 PSM_CT24 = 0x01; // 24-bit RGB
static constexpr u32 PSM_CT16 = 0x02; // 16-bit RGB
static constexpr u32 PSM_CT16S = 0x0A; // 16-bit RGB (signed)

u32 GsRenderer::read_vram_pixel(u32 addr) const {
    if (addr + 4 > m_gs.vram.size())
        return 0;
    u32 v;
    std::memcpy(&v, m_gs.vram.data() + addr, 4);
    return v;
}

void GsRenderer::write_vram_pixel(u32 addr, u32 rgba) {
    if (addr + 4 > m_gs.vram.size())
        return;
    std::memcpy(m_gs.vram.data() + addr, &rgba, 4);
}

void GsRenderer::draw_point(int x, int y, u32 color) {
    if (x < 0 || y < 0 || x >= int(fb_width) || y >= int(fb_height))
        return;
    // TODO: use current framebuffer base + PSM from FRAME register
    // For now, write to a simple linear framebuffer at VRAM offset 0
    write_vram_pixel((y * fb_width + x) * 4, color);
}

void GsRenderer::draw_line(int x0, int y0, int x1, int y1, u32 color) {
    int dx = std::abs(x1 - x0), dy = std::abs(y1 - y0);
    int sx = x0 < x1 ? 1 : -1, sy = y0 < y1 ? 1 : -1;
    int err = dx - dy;
    while (true) {
        draw_point(x0, y0, color);
        if (x0 == x1 && y0 == y1) break;
        int e2 = 2 * err;
        if (e2 > -dy) { err -= dy; x0 += sx; }
        if (e2 < dx) { err += dx; y0 += sy; }
    }
}

void GsRenderer::draw_tri_filled(float x0f, float y0f, float x1f, float y1f,
                                  float x2f, float y2f, u32 c0, u32 c1, u32 c2) {
    int x0 = int(x0f), y0 = int(y0f), x1 = int(x1f), y1 = int(y1f);
    int x2 = int(x2f), y2 = int(y2f);

    // Bounding box
    int minx = std::max(0, std::min({x0, x1, x2}));
    int maxx = std::min(int(fb_width) - 1, std::max({x0, x1, x2}));
    int miny = std::max(0, std::min({y0, y1, y2}));
    int maxy = std::min(int(fb_height) - 1, std::max({y0, y1, y2}));

    // Edge function
    auto edge = [](int px, int py, int ax, int ay, int bx, int by) {
        return int64_t(bx - ax) * (py - ay) - int64_t(px - ax) * (by - ay);
    };

    int64_t area = edge(x0, y0, x1, y1, x2, y2);
    if (area == 0)
        return; // degenerate

    for (int py = miny; py <= maxy; ++py) {
        for (int px = minx; px <= maxx; ++px) {
            int64_t w0 = edge(px, py, x1, y1, x2, y2);
            int64_t w1 = edge(px, py, x2, y2, x0, y0);
            int64_t w2 = edge(px, py, x0, y0, x1, y1);
            if ((w0 >= 0 && w1 >= 0 && w2 >= 0) || (w0 <= 0 && w1 <= 0 && w2 <= 0)) {
                // Barycentric interpolation
                float a = float(w0) / float(area);
                float b = float(w1) / float(area);
                float c = float(w2) / float(area);
                u8 r = u8(a * (c0 & 0xFF) + b * (c1 & 0xFF) + c * (c2 & 0xFF));
                u8 g = u8(a * ((c0 >> 8) & 0xFF) + b * ((c1 >> 8) & 0xFF) + c * ((c2 >> 8) & 0xFF));
                u8 bl = u8(a * ((c0 >> 16) & 0xFF) + b * ((c1 >> 16) & 0xFF) + c * ((c2 >> 16) & 0xFF));
                u8 al = u8(a * ((c0 >> 24) & 0xFF) + b * ((c1 >> 24) & 0xFF) + c * ((c2 >> 24) & 0xFF));
                draw_point(px, py, u32(r) | (u32(g) << 8) | (u32(bl) << 16) | (u32(al) << 24));
            }
        }
    }
}

void GsRenderer::draw_rect_filled(int x, int y, int w, int h, u32 color) {
    for (int py = y; py < y + h && py < int(fb_height); ++py)
        for (int px = x; px < x + w && px < int(fb_width); ++px)
            draw_point(px, py, color);
}

void GsRenderer::draw_triangle(float x0, float y0, float x1, float y1, float x2, float y2,
                                u32 c0, u32 c1, u32 c2) {
    draw_tri_filled(x0, y0, x1, y1, x2, y2, c0, c1, c2);
}

void GsRenderer::draw_sprite(float x, float y, float w, float h, u32 color) {
    draw_rect_filled(int(x), int(y), int(w), int(h), color);
}

void GsRenderer::clear(u32 x, u32 y, u32 w, u32 h, u32 color) {
    draw_rect_filled(int(x), int(y), int(w), int(h), color);
}

void GsRenderer::draw_pending() {
    // PRIM: bits 0-2 = prim type (0=Point,1=Line,2=LineStrip,3=Triangle,
    //   4=TriStrip,5=TriFan,6=Sprite,7=None)
    switch (m_prim_type & 7) {
    case 0: // Point
        for (auto& v : m_vertices)
            draw_point(int(v.x), int(v.y), v.rgba);
        m_vertices.clear();
        break;
    case 1: case 2: // Line / LineStrip
        for (size_t i = 1; i < m_vertices.size(); ++i)
            draw_line(int(m_vertices[i - 1].x), int(m_vertices[i - 1].y),
                      int(m_vertices[i].x), int(m_vertices[i].y), m_vertices[i].rgba);
        if (m_prim_type == 1) m_vertices.clear(); // Line: reset each pair
        else m_vertices.erase(m_vertices.begin(), m_vertices.end() - 1); // LineStrip: keep last
        break;
    case 3: // Triangle
        if (m_vertices.size() >= 3) {
            draw_tri_filled(m_vertices[0].x, m_vertices[0].y, m_vertices[1].x, m_vertices[1].y,
                            m_vertices[2].x, m_vertices[2].y, m_vertices[0].rgba, m_vertices[1].rgba,
                            m_vertices[2].rgba);
            m_vertices.clear();
        }
        break;
    case 4: // TriangleStrip
        for (size_t i = 2; i < m_vertices.size(); ++i) {
            draw_tri_filled(m_vertices[i - 2].x, m_vertices[i - 2].y, m_vertices[i - 1].x,
                            m_vertices[i - 1].y, m_vertices[i].x, m_vertices[i].y,
                            m_vertices[i - 2].rgba, m_vertices[i - 1].rgba, m_vertices[i].rgba);
        }
        m_vertices.erase(m_vertices.begin(), m_vertices.end() - 2);
        break;
    case 5: // TriangleFan
        if (m_vertices.size() >= 3) {
            for (size_t i = 2; i < m_vertices.size(); ++i) {
                draw_tri_filled(m_vertices[0].x, m_vertices[0].y, m_vertices[i - 1].x,
                                m_vertices[i - 1].y, m_vertices[i].x, m_vertices[i].y,
                                m_vertices[0].rgba, m_vertices[i - 1].rgba, m_vertices[i].rgba);
            }
            m_vertices.erase(m_vertices.begin() + 1, m_vertices.end());
        }
        break;
    case 6: // Sprite
        if (m_vertices.size() >= 2) {
            float sx = std::min(m_vertices[0].x, m_vertices[1].x);
            float sy = std::min(m_vertices[0].y, m_vertices[1].y);
            float sw = std::abs(m_vertices[1].x - m_vertices[0].x);
            float sh = std::abs(m_vertices[1].y - m_vertices[0].y);
            draw_sprite(sx, sy, sw, sh, m_vertices[0].rgba);
            m_vertices.clear();
        }
        break;
    default:
        m_vertices.clear();
        break;
    }
}

void GsRenderer::flush_draws() {
    draw_pending();
}

u32 GsRenderer::get_display_base() const {
    // DISPFB1: DBP (base pointer) at bits 32-45 of the register (word units * 2048 bytes)
    // We stored PRIV regs as 8-byte values in priv[].
    // DISPFB1 = 0x12000070, offset 0x70 from priv base.
    u64 dispfb = m_gs.read_priv(0x12000070);
    // DISPFB2 = 0x12000090 if DISPLAY2 is active.
    u64 dispfb2 = m_gs.read_priv(0x12000090);
    u64 display1 = m_gs.read_priv(0x12000080); // DISPLAY1
    u64 display2 = m_gs.read_priv(0x120000A0); // DISPLAY2
    // Use whichever display is enabled (DW > 0)
    u32 dw1 = u32(display1 & 0xFFF);
    u32 dw2 = u32(display2 & 0xFFF);
    if (dw2 > 0 && dw2 >= dw1)
        return u32((dispfb2 >> 32) & 0x3FFF) << 11; // DBP in word units
    return u32((dispfb >> 32) & 0x3FFF) << 11;
}

u32 GsRenderer::get_display_psm() const {
    u64 dispfb = m_gs.read_priv(0x12000070);
    return u32((dispfb >> 15) & 0xF); // PSM field
}

u32 GsRenderer::get_display_width() const {
    u64 display = m_gs.read_priv(0x12000080);
    return u32(display & 0xFFF); // DW (display width in pixels)
}

u32 GsRenderer::get_display_height() const {
    u64 display = m_gs.read_priv(0x12000080);
    return u32((display >> 32) & 0x7FF); // DH (display height)
}

void GsRenderer::render_frame() {
    // The GS draws into VRAM based on draw calls. This function is called
    // per frame to flush any pending draws. The actual rasterization happens
    // in draw_pending() called from flush_draws().
    flush_draws();
}

void GsRenderer::vram_to_rgba(u32 dbp, u32 dbw, u32 psm, u32 dw, u32 dh, std::vector<u8>& out) {
    out.resize(size_t(dw) * dh * 4);
    for (u32 y = 0; y < dh; ++y) {
        for (u32 x = 0; x < dw; ++x) {
            // GS VRAM uses block-based addressing (swizzle), not linear.
            // For now, use linear addressing as an approximation (TODO: proper swizzle).
            u32 addr = dbp + (y * dbw + x) * 4;
            u32 pixel = read_vram_pixel(addr);
            size_t idx = (y * dw + x) * 4;
            out[idx + 0] = u8(pixel & 0xFF);        // R
            out[idx + 1] = u8((pixel >> 8) & 0xFF);  // G
            out[idx + 2] = u8((pixel >> 16) & 0xFF); // B
            out[idx + 3] = u8((pixel >> 24) & 0xFF); // A
        }
    }
}

std::vector<u8> GsRenderer::read_framebuffer_rgba() {
    u32 base = get_display_base();
    u32 w = get_display_width();
    u32 h = get_display_height();
    if (w == 0 || h == 0) {
        w = fb_width;
        h = fb_height;
    }
    fb_width = w;
    fb_height = h;
    u32 psm = get_display_psm();
    // DBW (display framebuffer width in words) from DISPFB: bits 14-23 (9-bit, * 64)
    u64 dispfb = m_gs.read_priv(0x12000070);
    u32 dbw = (u32((dispfb >> 14) & 0x3F) + 1) * 64;
    if (dbw == 0) dbw = w;

    std::vector<u8> fb;
    vram_to_rgba(base, dbw, psm, w, h, fb);
    return fb;
}

} // namespace ee::rt
