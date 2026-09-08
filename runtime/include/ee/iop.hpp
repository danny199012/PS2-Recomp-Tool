// SPDX-License-Identifier: GPL-3.0-only
#pragma once

// IOP (Input/Output Processor) HLE: SIF (SIF0/SIF1/SIF2 DMA services),
// CDVD, pad, memory card, and audio (SPU2) stubs. The IOP is a R3000A
// that handles I/O; we HLE it at the SIF/RPC level like PS2Recomp's ps2xIOP.

#include <ee/types.hpp>

#include <functional>
#include <mutex>
#include <string>

#include <unordered_map>
#include <vector>

namespace ee::rt {

class Hw;

class Iop {
public:
    Iop();

    // SIF DMA: called when SIF0/SIF1/SIF2 channels transfer.
    // SIF0: IOP -> EE (receive), SIF1: EE -> IOP (send), SIF2: control.
    void sif0_recv(Hw& hw, const u8* data, u32 size);
    void sif1_send(Hw& hw, u8* dst, u32 size);
    void sif2_control(Hw& hw, u32 cmd);

    // SIF registers (0x1000F200..0x1000F300 area).
    u32 sif_read_reg(u32 addr) const;
    void sif_write_reg(Hw& hw, u32 addr, u32 value);

    // CDVD: minimal HLE - returns fixed disc info.
    u32 cdvd_read(u32 lba, u8* buf, u32 sectors);
    void cdvd_init() { m_cdvd_ready = true; }

    // Pad: returns a fixed/programmatic pad state.
    struct PadState { u32 buttons; u8 lx, ly, rx, ry; };
    PadState get_pad(int port) const;
    void set_pad(int port, PadState state);

    // Memory card: empty (no save data).
    bool mc_read(int port, u32 sector, u8* buf);
    bool mc_write(int port, u32 sector, const u8* buf);

    // SPU2: audio stub (no output for now).
    void spu2_write(u32 addr, u32 value) { (void)addr; (void)value; }
    u32 spu2_read(u32 addr) { return 0; }

    // RPC services table (function name -> handler).
    using RpcHandler = std::function<u32(const u8* in, u32 in_size, u8* out, u32 out_size)>;
    void register_rpc(const std::string& name, RpcHandler handler);
    u32 call_rpc(const std::string& name, const u8* in, u32 in_size, u8* out, u32 out_size);

    // SIF mailbox communication.
    struct SifCmd {
        u32 data[4]; // command header
        std::vector<u8> payload;
    };
    void send_sif_cmd(const SifCmd& cmd);
    bool recv_sif_cmd(SifCmd& cmd);

private:
    bool m_cdvd_ready = false;
    PadState m_pad[2] = {};
    std::mutex m_mutex;
    std::vector<SifCmd> m_sif_cmd_queue;

    // SIF registers
    u32 m_sif_ms_f200 = 0;
    u32 m_sif_ms_f204 = 0;
    u32 m_sif_ms_f220 = 0;
    u32 m_sif_sm_f200 = 0;
    u32 m_sif_sm_f204 = 0;
    u32 m_sif_sm_f220 = 0;

    // RPC handlers
    std::unordered_map<std::string, RpcHandler> m_rpc_handlers;
};

} // namespace ee::rt
