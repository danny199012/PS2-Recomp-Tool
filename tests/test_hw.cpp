// SPDX-License-Identifier: GPL-3.0-only
// Hardware pipeline tests: DMAC chain walking, GIF tag processing, GS register
// writes / VRAM, VIF command stream + UNPACK, VU memory upload.
#include <ee/hw.hpp>
#include <ee/runtime.hpp>

#include <cstdio>
#include <cstring>
#include <memory>

static int g_failures = 0;
static int g_checks = 0;

#define CHECK(cond)                                                            \
    do {                                                                       \
        ++g_checks;                                                            \
        if (!(cond)) {                                                         \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);        \
            ++g_failures;                                                      \
        }                                                                      \
    } while (0)

using namespace ee;
using namespace ee::rt;

int main() {
    Runtime rt;
    auto hw_ptr = std::make_unique<Hw>();
    Hw& hw = *hw_ptr;
    hw.mem = &rt.mem;

    // --- GIF: PACKED tag with PRIM + RGBAQ + XYZ2 (a single vertex)
    {
        u8 buf[64] = {};
        // GIF tag: NLOOP=1, PRE=1, PRIM=6 (triangle), FLG=0 (PACKED), NREG=2
        // REGS nibbles: 0x1 (RGBAQ), 0x4 (XYZF2) -> hi = 0x41
        // NREG at bits 47-50, PRE at bit 46, FLG at bits 44-45, PRIM at bits 33-43
        u64 lo = 1 | (u64(2) << 47) | (u64(6) << 33) | (u64(1) << 46);
        u64 hi = 0x41ULL;
        std::memcpy(&buf[0], &lo, 8);
        std::memcpy(&buf[8], &hi, 8);
        // data: RGBAQ (16 bytes), XYZ2 (16 bytes)
        u64 rgba_lo = 0xFF00FF00FFULL | (u64(0x3F800000) << 32);
        u64 rgba_hi = 0;
        std::memcpy(&buf[16], &rgba_lo, 8);
        std::memcpy(&buf[24], &rgba_hi, 8);
        u64 xyz_lo = 0;
        u64 xyz_hi = 0;
        std::memcpy(&buf[32], &xyz_lo, 8);
        std::memcpy(&buf[40], &xyz_hi, 8);

        Gif gif;
        gif.feed(hw.gs, buf, 48);
        CHECK(gif.tags_processed == 1);
        CHECK(hw.gs.vertices_received == 1);
        CHECK(hw.gs.read_hwreg(0x00) == 6);
        CHECK(hw.gs.read_hwreg(0x01) == rgba_lo);
    }

    // --- GIF: IMAGE mode (1 quad of pixel data -> VRAM) ---
    {
        auto gs2_obj = std::make_unique<Gs>();
        Gs& gs2 = *gs2_obj;
        gs2.image_dst = 0x100;
        u8 buf[32] = {};
        // tag: NLOOP=1, FLG=2 (IMAGE), EOP=1
        u64 lo = 1 | (u64(2) << 46) | (u64(1) << 15); // nloop=1, eop=1, flg=2 shifted? Actually FLG is bits 44-45.
        lo = 1 | (u64(2) << 44); // nloop=1, FLG=2 (IMAGE)
        u64 hi = 0;
        std::memcpy(&buf[0], &lo, 8);
        std::memcpy(&buf[8], &hi, 8);
        // 1 quad of pixel data
        u64 dlo = 0xDEADBEEFCAFE, dhi = 0x1234567;
        std::memcpy(&buf[16], &dlo, 8);
        std::memcpy(&buf[24], &dhi, 8);

        Gif gif2;
        gif2.feed(gs2, buf, 32);
        CHECK(gs2.image_bytes_written == 16);
        // verify VRAM content
        u64 v0, v1;
        std::memcpy(&v0, gs2.vram.data() + 0x100, 8);
        std::memcpy(&v1, gs2.vram.data() + 0x108, 8);
        CHECK(v0 == dlo && v1 == dhi);
    }

    // --- VIF: UNPACK V4_32 (4 floats into VU1 data memory) ---
    {
        auto hw2_ptr = std::make_unique<Hw>(); Hw& hw2 = *hw2_ptr;
        hw2.mem = &rt.mem;
        u8 buf[40] = {};
        // STCYCL(WL=1, CL=1)
        u32 cmd = 0x01000100;
        std::memcpy(&buf[0], &cmd, 4);
        // UNPACK V4-32 (0x6C): num=2, imm=0 (addr=0)
        cmd = (0x6C << 24) | (2 << 16) | 0;
        std::memcpy(&buf[4], &cmd, 4);
        // 2 quads of data (each = 4x u32 = 16 bytes)
        u32 data[8] = {1, 2, 3, 4, 5, 6, 7, 8};
        std::memcpy(&buf[8], data, 32);

        hw2.vif[1].feed(hw2, 1, buf, 40);
        CHECK(hw2.vif[1].commands_processed == 2);
        // verify VU1 data memory
        u32 v[4];
        std::memcpy(v, hw2.vu1_dmem.data(), 16);
        CHECK(v[0] == 1 && v[1] == 2 && v[2] == 3 && v[3] == 4);
        std::memcpy(v, hw2.vu1_dmem.data() + 16, 16);
        CHECK(v[0] == 5 && v[1] == 6 && v[2] == 7 && v[3] == 8);
    }

    // --- VIF: V2-16 unpack (v1v0v1v0 per PCSX2) ---
    {
        auto hw2_ptr = std::make_unique<Hw>(); Hw& hw2 = *hw2_ptr;
        hw2.mem = &rt.mem;
        u8 buf[20] = {};
        u32 cmd = (0x65 << 24) | (1 << 16) | 0; // V2-16 (us=0,vn=1,vl=1), num=1, addr=0
        std::memcpy(&buf[0], &cmd, 4);
        // 2 x u16 = 4 bytes
        u16 vals[2] = {0x1111, 0x2222};
        std::memcpy(&buf[4], vals, 4);

        hw2.vif[0].feed(hw2, 0, buf, 8);
        u32 v[4];
        std::memcpy(v, hw2.vu0_dmem.data(), 16);
        // V2 repeats: x=v0, y=v1, z=v0, w=v1 (sign-extended 16-bit)
        CHECK(v[0] == 0x00001111u && v[1] == 0x00002222u);
        CHECK(v[2] == 0x00001111u && v[3] == 0x00002222u);
    }

    // --- DMAC: GIF channel chain transfer (NEXT tag + END) ---
    {
        auto hw2_obj = std::make_unique<Hw>();
        Hw& hw2 = *hw2_obj;
        hw2.mem = &rt.mem;
        // Build a chain in guest RAM:
        // tag0 @ 0x100000: NEXT, qwc=1, addr=0x100040 (tag1 follows data at 0x100010)
        // data @ 0x100010: 1 GIF tag (PRIM AD, 0x0E reg)
        // tag1 @ 0x100040: END, qwc=0
        u8 gif_tag[32] = {};
        // GIF tag: nloop=1, flg=PACKED, nreg=1, regs=[0x0E] (AD)
        u64 lo = 1 | (u64(1) << 47); // nloop=1, nreg=1
        u64 hi = 0x0EULL;
        std::memcpy(&gif_tag[0], &lo, 8);
        std::memcpy(&gif_tag[8], &hi, 8);
        // AD data: value=0x1234, addr=0x05 (CLAMP_1)
        u64 ad_lo = 0x1234;
        u64 ad_hi = 0x05;
        std::memcpy(&gif_tag[16], &ad_lo, 8); // we'll place this right after the tag
        std::memcpy(&gif_tag[24], &ad_hi, 8);

        // tag0 (NEXT): qwc=1, id=2(NEXT), addr=0x100040
        u32 tag0 = 1 | (2u << 28) | 0x100040;
        std::memcpy(rt.mem.translate(0x100000), &tag0, 4);
        // data follows at 0x100010: the GIF tag + AD data (1 quad = 16 bytes of tag + 16 bytes of AD)
        // Actually 1 QWC = 16 bytes. So data is 1 quad.
        // But PRIM AD needs: GIF tag (16B) + 1 reg * 1 loop = 16B of data = 32B total = 2 quads.
        // Let's use qwc=2.
        tag0 = 2 | (2u << 28) | 0x100050;
        std::memcpy(rt.mem.translate(0x100000), &tag0, 4);
        // data at 0x100010: 2 quads = gif_tag(16B) + ad_data(16B)
        std::memcpy(rt.mem.translate(0x100010), gif_tag, 16);
        std::memcpy(rt.mem.translate(0x100020), gif_tag + 16, 16);
        // tag1 @ 0x100050: END, qwc=0
        u32 tag1 = (7u << 28); // id=7 (END), qwc=0
        std::memcpy(rt.mem.translate(0x100050), &tag1, 4);

        // Set up GIF channel: CHCR=chain mode + STR, TADR=0x100000
        hw2.dmac.ch[2].tadr = 0x100000;
        hw2.dmac.write(hw2, 0x1000A000, 0x104); // GIF CHCR: chain mode + STR (rising edge)

        CHECK(hw2.dmac.transfers == 1);
        CHECK(hw2.gif.tags_processed >= 1);
        // CLAMP_1 register should have been written via AD
        CHECK(hw2.gs.read_hwreg(0x05) == 0x1234);
    }

    // --- GS FINISH -> Runtime::on_frame (drives the launcher's display) ---
    {
        Runtime rtt; // its Hw is wired to it in the Runtime ctor
        int frames = 0;
        rtt.on_frame = [&frames]() { ++frames; };

        // GIF stream: PACKED tag with one AD write to GS register 0x61 (FINISH).
        u8 buf[32] = {};
        u64 lo = 1 | (u64(1) << 47); // nloop=1, nreg=1
        u64 hi = 0x0EULL;            // regs=[AD]
        std::memcpy(&buf[0], &lo, 8);
        std::memcpy(&buf[8], &hi, 8);
        u64 ad_lo = 0x12345678;
        u64 ad_hi = 0x61; // FINISH
        std::memcpy(&buf[16], &ad_lo, 8);
        std::memcpy(&buf[24], &ad_hi, 8);

        rtt.hw->gif.feed(rtt.hw->gs, buf, 32);
        rtt.hw->on_gif_frame_done();
        CHECK(frames == 1); // FINISH triggered the frame callback
        CHECK(rtt.hw->gs.read_hwreg(0x61) == 0x12345678);

        // A GIF stream WITHOUT FINISH must not call on_frame.
        ad_hi = 0x05; // CLAMP_1
        std::memcpy(&buf[24], &ad_hi, 8);
        rtt.hw->gif.feed(rtt.hw->gs, buf, 32);
        rtt.hw->on_gif_frame_done();
        CHECK(frames == 1); // unchanged
    }

    // --- IOP: thread-safe pad get/set + CDVD fallback without a host ISO ---
    {
        Iop iop;
        Iop::PadState p{0x4000, 100, 200, 50, 250};
        iop.set_pad(0, p);
        Iop::PadState got = iop.get_pad(0);
        CHECK(got.buttons == 0x4000);
        CHECK(got.lx == 100 && got.ly == 200 && got.rx == 50 && got.ry == 250);
        CHECK(iop.disc_type() == 0x14); // DVD default when no ISO attached
        u8 buf[4096];
        std::memset(buf, 0xAB, sizeof buf);
        const u32 r = iop.cdvd_read(0, buf, 2); // no disc: "success" + zeroed buffer
        CHECK(r == 2);
        CHECK(buf[0] == 0 && buf[2048] == 0);
    }

    std::printf("test_hw: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
