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
    std::unordered_map<u32, u32>& sif_regs() { return m_sif_regs; }

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

    // SIF DMA engine: SIF0 (IOP->EE) and SIF1 (EE->IOP) channels.
    struct SifChannel {
        u32 madr = 0;  // memory address
        u32 tadr = 0;  // tag address (SIF0)
        u32 qwc = 0;   // quadword count
        u32 chcr = 0;  // channel control (bit 0 = STR start, bit 8 = tag)
        u32 tag_last = 0; // last tag marker
    };
    void sif_dma_trigger(Hw& hw, int ch);  // ch: 0=SIF0, 1=SIF1
    void sif0_complete(Hw& hw);             // signal SIF0 done (IOP->EE)
    bool m_sif_init_done = false;           // SifInitRpc completed

    // --- SIFCMD: system command processing (Play! Iop_SifCmd / SIF.cpp port) ---
    //
    // The EE game (old libkernel, SCE protocol) sends SIF commands through the
    // sceSifSetDma syscall (0x77): a list of SifDmaTransfer_t descriptors
    // {src(EE), dest(IOP), size, attr}. The last descriptor carries the command
    // packet itself (attr = SIF_DMA_ERT|SIF_DMA_INT_O); earlier descriptors are
    // raw payload transfers into IOP RAM (call arguments), or a stage to the
    // RPC_RECVADDR mailbox (0xDEADBEF0).
    //
    // System command ids (SifCmdHeader.cid, bit 31 set = system):
    //   0x80000000 CHANGE_SADDR  {hdr, buf}      buf = EE packet buffer address
    //   0x80000001 SET_SREG      {hdr, idx, val}
    //   0x80000002 INIT_CMD      {hdr, buf}      buf = EE packet buffer address
    //   0x80000003 RESET_CMD     {hdr, arglen, mode, arg[]}  (IRX module reset)
    //   0x80000008 RPC_END (REND) — completion packet built by the IOP
    //   0x80000009 RPC_BIND      {hdr, rec_id, pkt_addr, rpc_id, cd, sid}
    //   0x8000000A RPC_CALL      {hdr, rec_id, pkt_addr, rpc_id, cd, rpc_number,
    //                             send_size, recvbuf, recv_size, rmode, sd}
    //   0x8000000C RPC_RDATA     {hdr, rec_id, pkt_addr, rpc_id, recvbuf, src,
    //                             dest, size}
    // All fields are 32-bit words; offsets below are relative to the packet start.
    static constexpr u32 SIF_CMD_ID_SYSTEM   = 0x80000000;
    static constexpr u32 SIF_CMD_CHANGE_SADDR = 0x80000000;
    static constexpr u32 SIF_CMD_SET_SREG    = 0x80000001;
    static constexpr u32 SIF_CMD_INIT_CMD    = 0x80000002;
    static constexpr u32 SIF_CMD_RESET_CMD   = 0x80000003;
    static constexpr u32 SIF_CMD_RPC_END     = 0x80000008;
    static constexpr u32 SIF_CMD_RPC_BIND    = 0x80000009;
    static constexpr u32 SIF_CMD_RPC_CALL    = 0x8000000A;
    static constexpr u32 SIF_CMD_RPC_RDATA   = 0x8000000C;

    // SIF status flag bits (sifreg-common.h).
    static constexpr u32 SIF_STAT_SIFINIT = 0x10000;
    static constexpr u32 SIF_STAT_CMDINIT = 0x20000;
    static constexpr u32 SIF_STAT_BOOTEND = 0x40000;

    // Process one sceSifSetDma call: walk the descriptor list, perform the
    // transfers, dispatch command packets. Returns a nonzero DMA id (for
    // SifDmaStat) or 0 if nothing was queued.
    u32 sif_set_dma(Hw& hw, u32 dmat_addr, s32 count);

    // SIF system registers, as returned by the old libkernel SifGetReg/SifSetReg
    // syscalls (register ids: 1=MAINADDR 2=SUBADDR 3=MSFLAG 4=SMFLAG,
    // 0x80000000=RESETADDR, 0x80000002=RPCINIT).
    u32 sif_get_sysreg(u32 reg) const;
    void sif_set_sysreg(Hw& hw, u32 reg, u32 value);

    // Command dispatch (packet read from EE RAM at ee_src).
    void sif_process_cmd(Hw& hw, u32 ee_src, u32 size, u32 dest);
    void sif_cmd_change_addr(Hw& hw, const u8* pkt);
    void sif_cmd_set_sreg(Hw& hw, const u8* pkt);
    void sif_cmd_init(Hw& hw, const u8* pkt);
    void sif_cmd_reset(Hw& hw, const u8* pkt);
    void sif_cmd_bind(Hw& hw, const u8* pkt);
    void sif_cmd_call(Hw& hw, const u8* pkt);
    void sif_cmd_other_data(Hw& hw, const u8* pkt);
    void sif_cmd_rend(Hw& hw, const u8* pkt);

    // Build + deliver an SIFRPCRENDPKT (0x30 bytes) completion packet for a
    // request packet, into the EE's registered packet buffer (INTC 13 fires).
    void sif_send_rend(Hw& hw, const u8* request_pkt, u32 sd, u32 buf, u32 cbuf);

    // Deliver a command packet to the EE's registered packet buffer (the IOP's
    // SIF0 DMA IOP->EE + SIF interrupt). No-op if the EE has not registered a
    // buffer yet (packet queued until it does).
    void sif_deliver_to_ee(Hw& hw, const void* pkt, u32 size);

    // HLE IOP RPC server registry (sid -> module name). Modules are registered
    // with Play!'s SIF service ids; BINDs/CALLs to these sids are answered
    // directly by the IOP HLE (Play!'s RPC_SERVERID_XOR server-data scheme).
    bool sif_rpc_module_lookup(u32 sid, const char*& name) const;
    std::unordered_map<u32, std::string> m_sif_modules;

    // SIF state
    u32 m_ee_recv_addr = 0; // EE packet buffer (from CHANGE_SADDR / INIT_CMD)
    u32 m_main_addr = 0;    // MAINADDR: EE cmd_data address
    u32 m_sub_addr = 0;     // SUBADDR: IOP command receive buffer address
    u32 m_ms_flag = 0;      // MSFLAG: EE status flag (as seen by the IOP)
    u32 m_sm_flag = 0;      // SMFLAG: IOP status flag (as seen by the EE)
    u32 m_data_addr = 0;    // last RPC_RECVADDR stage (call arguments EE src)
    u32 m_sreg[0x20] = {};  // SIF software registers (sreg 0 = RPCINIT)
    u32 m_next_dma_id = 1;  // fake SIF DMA ids for SifDmaStat
    u32 m_bootend_sent = 0; // one-shot SET_SREG(RPCINIT) delivery guard
    u32 m_unknown_cids = 0; // log-once counter for unknown commands
    mutable u32 m_reg_trace = 0; // bring-up: first-N SIF reg access logging
    std::vector<u8> m_pending_ee; // packets queued before the EE registered a buffer

    // SIF registers
    // SIF register file (IOP-side SBUS as seen by the EE, base 0x1FFFFF80):
    // offset -> last written 32-bit value.
    std::unordered_map<u32, u32> m_sif_regs;
    SifChannel m_sif0, m_sif1;

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
    bool m_trace = true; // bring-up tracing for SIF DMA
};

} // namespace ee::rt
