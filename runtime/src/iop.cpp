// SPDX-License-Identifier: GPL-3.0-only
#include <ee/iop.hpp>
#include <ee/hw.hpp>
#include <ee/kernel.hpp>
#include <ee/runtime.hpp>

#include <cstdio>
#include <cstring>
#include <set>

#include <ee/cdvd.hpp>

namespace ee::rt {

Iop::Iop() {
    // Default pad state: no buttons pressed, centered sticks.
    for (int i = 0; i < 2; ++i)
        m_pad[i] = {0x0000, 128, 128, 128, 128};

    // IOP boot state: the IOP sifcmd/sifrpc "modules" (Play! IopBios) are
    // initialized before the game boots, so the EE's SifInitCmd takes the
    // "already initialized" path (SUBADDR valid, SMFLAG set).
    m_sub_addr = 0x1FFFC000; // IOP command receive buffer (backed by iop_mem)
    m_sm_flag = SIF_STAT_SIFINIT | SIF_STAT_CMDINIT | SIF_STAT_BOOTEND; // 0x70000
    m_sreg[0] = 1;           // SIF_SREG_RPCINIT
    m_iop_mem_size = 0x200000; // real IOP RAM is 2 MiB (sysmem reportable size)

    // HLE IOP RPC services (Play!'s module service ids — the game BINDs these).
    m_sif_modules[0x80000001] = "fileio";
    m_sif_modules[0x80000006] = "loadcore";
    m_sif_modules[0x80000400] = "mcman";
    m_sif_modules[0x80000401] = "mcserv";
    for (u32 s = 0x80000592; s <= 0x8000059C; ++s)
        m_sif_modules[s] = "cdvdfsv";
    m_sif_modules[0x80000100] = "padman";
    m_sif_modules[0x80000101] = "padman";
    m_sif_modules[0x8000010F] = "padman";
    m_sif_modules[0x8000011F] = "padman";
    m_sif_modules[0x80000140] = "sio2man";
    m_sif_modules[0x80000003] = "sysmem";
}

// --- SIF DMA -------------------------------------------------------------------------

void Iop::sif0_recv(Hw& hw, const u8* data, u32 size) {
    // SIF0: data flowing from IOP -> EE. Process SIF commands.
    if (size >= 16) {
        SifCmd cmd;
        std::memcpy(cmd.data, data, 16);
        if (size > 16)
            cmd.payload.assign(data + 16, data + size);
        send_sif_cmd(cmd);
    }
    (void)hw;
}

void Iop::sif1_send(Hw& hw, u8* dst, u32 size) {
    // SIF1: data flowing from EE -> IOP. Process RPC requests.
    // For now, just zero the buffer (ack).
    if (dst && size > 0)
        std::memset(dst, 0, size);
    (void)hw;
}

void Iop::sif2_control(Hw& hw, u32 cmd) {
    // SIF2: control channel (reset, init).
    m_sif_sm_f200 = 0x10000; // signal initialized
    (void)hw;
    (void)cmd;
}

// --- SIF DMA engine ------------------------------------------------------------------
//
// The game kicks SIF DMA by writing to MADR/TADR/CHCR at the IOP-side SIF register
// file (base 0x1FFFFF80, as seen by the EE). On trigger we perform the transfer
// immediately (HLE shortcut: no real IOP) and raise the SIF interrupt (INTC 13).
//
// Register layout (offsets from SIF base 0x1FFFFF80):
//   +0x00  SIF0 MADR   (memory address)
//   +0x10  SIF0 TADR   (tag address)
//   +0x20  SIF0 CHCR   (control; bit0=STR start)
//   +0x30  SIF1 MADR
//   +0x40  SIF1 TADR
//   +0x50  SIF1 CHCR

void Iop::sif_dma_trigger(Hw& hw, int ch) {
    SifChannel& dma = (ch == 0) ? m_sif0 : m_sif1;
    if (!(dma.chcr & 1))
        return; // STR not set

    u32 addr = dma.madr & 0x1FFFFFFF;
    u32 qwc = dma.qwc;

    if (ch == 0) {
        // SIF0: IOP -> EE. The "IOP" delivers data into EE RAM at MADR.
        // HLE: we simulate the IOP having nothing meaningful to send, so just
        // acknowledge. A real implementation would run the IOP (R3000) and have
        // it DMA the response into EE RAM here.
        if (m_trace && qwc > 0)
            std::fprintf(stderr, "[sif] SIF0 IOP->EE madr=0x%08X qwc=%u (ack)\n", addr, qwc);
    } else {
        // SIF1: EE -> IOP. The EE sends a command/buffer to the IOP.
        // HLE: read the EE buffer at MADR and process it as an SIF RPC command.
        if (m_trace && qwc > 0)
            std::fprintf(stderr, "[sif] SIF1 EE->IOP madr=0x%08X qwc=%u\n", addr, qwc);
    }

    // Clear STR to signal completion (games poll CHCR or MADR/TADR for this).
    dma.chcr &= ~1u;
    dma.madr = 0;
    dma.tadr = 0;

    // Raise the SIF interrupt (INTC cause 13) so the EE's SIF handler runs.
    if (hw.runtime && hw.runtime->kernel)
        hw.runtime->kernel->raise_intc(1, 0);

    m_sif_init_done = true;
}

void Iop::sif0_complete(Hw& hw) {
    // Explicitly mark SIF0 as having completed a transfer (used by SifInitRpc
    // handshake where the EE waits for the IOP to respond).
    m_sif0.chcr &= ~1u;
    m_sif0.madr = 0;
    if (hw.runtime && hw.runtime->kernel)
        hw.runtime->kernel->raise_intc(1, 0);
    m_sif_init_done = true;
}

// --- SIF registers -------------------------------------------------------------------
//
// Register file at IOP-side SBUS base 0x1FFFFF80 as seen by the EE.
// Observed layout (offsets from base) from the Sims Urbz boot sequence:
//   +0x24/+0x28  SIF0 control bits (write 1 to kick)
//   +0x44/+0x48  SIF1 control bits (write 1 to kick)
//   +0x60        SIF0 TADR (tag address; game reads this to poll completion)
//   +0x70        SIF0 MADR (memory address)
// The remaining offsets are general SIF registers (stored and returned verbatim).

u32 Iop::sif_read_reg(u32 addr) const {
    // Semantic SIF registers (EE SBUS, stride 0x10 — ps2sdk ee_regs.h):
    //   0x1000F200 MADDR (MAINADDR)  0x1000F210 SADDR (SUBADDR)
    //   0x1000F220 MSFLAG (EE flag)  0x1000F230 SMFLAG (IOP flag)
    switch (addr) {
    case 0x1000F200: return m_main_addr;
    case 0x1000F210: return m_sub_addr;
    case 0x1000F220: return m_ms_flag;
    case 0x1000F230: return m_sm_flag;
    }
    auto it = m_sif_regs.find(addr);
    u32 v = (it != m_sif_regs.end()) ? it->second : 0;
    if (addr == 0x1000F300)
        v = 0x70000u; // SIF_STAT_SIFINIT | SIF_STAT_CMDINIT | SIF_STAT_BOOTEND
    if (m_trace && (m_reg_trace += 1) < 400) {
        std::fprintf(stderr, "[sif] reg read  0x%08X = 0x%08X\n", addr, v);
    }
    return v;
}

void Iop::sif_write_reg(Hw& hw, u32 addr, u32 value) {
    // Semantic registers first (bring-up trace: log the first accesses).
    switch (addr) {
    case 0x1000F200: m_main_addr = value; return;
    case 0x1000F210: m_sub_addr = value;  return;
    case 0x1000F220: m_ms_flag = value;   return;
    case 0x1000F230: return; // SMFLAG is IOP-owned
    }
    if (m_trace && (m_reg_trace += 1) < 60)
        std::fprintf(stderr, "[sif] reg write 0x%08X = 0x%08X\n", addr, value);
    m_sif_regs[addr] = value;

    // Detect a SIF DMA kick: writing 1 to the control bit offsets.
    const u32 off = addr - 0x1FFFFF80;
    if ((off == 0x24 || off == 0x28 || off == 0x44 || off == 0x48) && (value & 1)) {
        // Kick: the tag address (the EE packet address) was written to the
        // TADR register (base+0x60) — the old-libkernel SIF DMA pulls the
        // command packet from there into the IOP command buffer.
        const u32 tadr = m_sif_regs.count(0x1FFFFFE0) ? m_sif_regs[0x1FFFFFE0] : 0;
        const u32 madr = m_sif_regs.count(0x1FFFFFF0) ? m_sif_regs[0x1FFFFFF0] : 0;
        // Clear the kick bits + tag (the game polls TADR == 0 for completion).
        m_sif_regs[addr] = 0;
        m_sif_regs[0x1FFFFFE0] = 0;

        u32 tag = tadr;
        if (tag == 0)
            tag = madr; // some send paths use the MADR register
        const u32 esrc = tag & 0x1FFFFFFF;
        if (esrc < 0x2000000 && hw.mem) {
            const u8* pkt = hw.mem->translate(esrc);
            u32 hdr0 = 0, cid = 0;
            std::memcpy(&hdr0, pkt + 0, 4);
            std::memcpy(&cid, pkt + 8, 4);
            const u32 psize = hdr0 & 0xFF;
            const u32 xfer = psize ? (psize + 15) & ~15u : 16u;
            if (m_trace)
                std::fprintf(stderr, "[sif] DMA kick tag=0x%08X cid=0x%08X psize=0x%X\n",
                             tag, cid, psize);
            if (psize >= 16 && psize <= 0x80)
                sif_process_cmd(hw, esrc, xfer, m_sub_addr);
            else if (m_trace) {
                std::fprintf(stderr, "[sif] DMA kick tag=0x%08X (no valid cmd header) words:", tag);
                for (u32 i = 0; i < 16; i += 4) {
                    u32 w = 0;
                    std::memcpy(&w, pkt + i, 4);
                    std::fprintf(stderr, " %08X", w);
                }
                std::fprintf(stderr, "\n");
            }
        } else if (m_trace && tag == 0) {
            std::fprintf(stderr, "[sif] DMA kick (no tag; ack)\n");
        }

        // Raise the SIF interrupt (INTC cause 13).
        if (hw.runtime && hw.runtime->kernel)
            hw.runtime->kernel->raise_intc(1, 0);
        m_sif_init_done = true;
        if (m_trace)
            std::fprintf(stderr, "[sif] DMA kick done (tadr=0x%08X)\n", tag);
    }
}

// --- SIF command processing (Play! Iop_SifCmd / CSIF port, ps2sdk sifcmd) ---------
//
// The old-libkernel EE game drives the whole SIF RPC protocol through the
// sceSifSetDma syscall: each call passes a list of SifDmaTransfer_t descriptors
// {src(EE), dest(IOP), size, attr} (16 bytes each). The command packet itself is
// the descriptor with attr & SIF_DMA_ERT (0x40); its data is the SifCmdHeader_t:
//
//   word 0: psize:8 | dsize:24    word 1: dest (payload IOP addr)
//   word 2: cid                   word 3: opt
//
// Commands (cid & 0x7FFFFFFF): 0 CHANGE_SADDR, 1 SET_SREG, 2 INIT_CMD,
// 3 RESET_CMD, 8 RPC_END(REND), 9 RPC_BIND, 0xA RPC_CALL, 0xC RPC_RDATA.
// Responses are delivered to the EE's registered packet buffer (registered via
// CHANGE_SADDR/INIT_CMD) followed by the SIF interrupt (INTC cause 13).

u32 Iop::sif_set_dma(Hw& hw, u32 dmat_addr, s32 count) {
    if (count <= 0 || dmat_addr == 0)
        return 0;
    if (!hw.mem)
        return 0;

    u32 id = 0;
    for (s32 i = 0; i < count; ++i) {
        const u8* d = hw.mem->translate((dmat_addr & 0x1FFFFFFF) + 16 * i);
        u32 src = 0, dest = 0, size = 0, attr = 0;
        std::memcpy(&src, d + 0, 4);
        std::memcpy(&dest, d + 4, 4);
        std::memcpy(&size, d + 8, 4);
        std::memcpy(&attr, d + 12, 4);
        if (size == 0)
            continue;
        id = m_next_dma_id++;

        if (m_trace)
            std::fprintf(stderr,
                         "[sif] dma id=%u src=0x%08X dest=0x%08X size=0x%X attr=0x%X\n",
                         id, src, dest, size, attr);

        // Command packet descriptor (ERT bit): dispatch the SIF command.
        // (Payload descriptors first — by the time the packet dispatch runs,
        // the call arguments are already staged.)
        if (attr & 0x40) { // SIF_DMA_ERT
            sif_process_cmd(hw, src & 0x1FFFFFFF, size, dest);
        } else if (dest == 0xDEADBEF0) {
            // RPC args mailbox (Play! RPC_RECVADDR): remember the EE source.
            m_data_addr = src;
        } else if (dest != 0) {
            // Raw transfer EE -> IOP RAM (call arguments for IOP-side servers,
            // module data, etc.). Persists in the IOP backing store.
            const u32 off = (dest & 0x1FFFFFFF) - 0x1F000000;
            if (off < hw.iop_mem.size() && off + size <= hw.iop_mem.size())
                std::memcpy(hw.iop_mem.data() + off, hw.mem->translate(src & 0x1FFFFFFF), size);
        }
    }
    return id;
}

// Read a 32-bit word out of a command packet.
static u32 pkt_word(const u8* pkt, u32 off) {
    u32 v;
    std::memcpy(&v, pkt + off, 4);
    return v;
}

void Iop::sif_process_cmd(Hw& hw, u32 ee_src, u32 size, u32 dest) {
    if (!hw.mem || size < 16)
        return;
    const u8* pkt = hw.mem->translate(ee_src);
    const u32 cid = pkt_word(pkt, 8);
    if (m_trace)
        std::fprintf(stderr, "[sif] cmd cid=0x%08X size=0x%X dest=0x%08X src=0x%08X\n",
                     cid, size, dest, ee_src);

    switch (cid) {
    case SIF_CMD_CHANGE_SADDR: sif_cmd_change_addr(hw, pkt); return;
    case SIF_CMD_SET_SREG:     sif_cmd_set_sreg(hw, pkt);    return;
    case SIF_CMD_INIT_CMD:     sif_cmd_init(hw, pkt);        return;
    case SIF_CMD_RESET_CMD:    sif_cmd_reset(hw, pkt);       return;
    case SIF_CMD_RPC_END:      sif_cmd_rend(hw, pkt);        return;
    case SIF_CMD_RPC_BIND:     sif_cmd_bind(hw, pkt);        return;
    case SIF_CMD_RPC_CALL:     sif_cmd_call(hw, pkt);        return;
    case SIF_CMD_RPC_RDATA:    sif_cmd_other_data(hw, pkt);  return;
    default:
        // User commands (game's own IOP modules) and old-libkernel SIF packets
        // have no HLE implementation yet. Dump the header so the packet format
        // can be decoded from the trace (bring-up).
        if ((m_unknown_cids++) < 8) {
            std::fprintf(stderr, "[sif] unknown command cid=0x%08X (dropped) header:", cid);
            for (u32 i = 0; i < 16 && i < size; i += 4)
                std::fprintf(stderr, " %08X", pkt_word(pkt, i));
            std::fprintf(stderr, "\n");
        }
        return;
    }
}

// Deliver SET_SREG(RPCINIT, 1) to the EE (the game's SifInitRpc polls
// SifGetSreg(0) in old libkernel). One-shot.
static void deliver_rpcinit_sreg(Iop& iop, Hw& hw) {
    u8 sreg_pkt[0x18] = {};
    const u32 cid = Iop::SIF_CMD_SET_SREG;
    const u32 psize_dsize = 0x18;
    const u32 index = 0, value = 1; // SIF_SREG_RPCINIT = 1
    std::memcpy(sreg_pkt + 0x00, &psize_dsize, 4);
    std::memcpy(sreg_pkt + 0x08, &cid, 4);
    std::memcpy(sreg_pkt + 0x10, &index, 4);
    std::memcpy(sreg_pkt + 0x14, &value, 4);
    iop.sif_deliver_to_ee(hw, sreg_pkt, sizeof sreg_pkt);
}

void Iop::sif_cmd_change_addr(Hw& hw, const u8* pkt) {
    // SifCmdChgAddrData_t: {header(0x10), buf@0x10} — the EE's command packet
    // buffer. (buf sits directly after the 16-byte header; 0x14 was reading
    // the next word — garbage — which made every RPC reply DMA into RAM[5].)
    const u32 buf = pkt_word(pkt, 0x10);
    m_ee_recv_addr = buf;
    if (m_trace)
        std::fprintf(stderr, "[sif] CHANGE_SADDR ee_recv_addr=0x%08X\n", buf);

    // The IOP's sifcmd is "already initialized" from the EE's point of view
    // (SUBADDR was valid), so the game took the early path. Complete the
    // handshake the IOP firmware would: deliver SET_SREG(RPCINIT, 1).
    if (!m_bootend_sent) {
        m_bootend_sent = 1;
        deliver_rpcinit_sreg(*this, hw);
    }
}

void Iop::sif_cmd_set_sreg(Hw& hw, const u8* pkt) {
    // SifCmdSRegData_t: {header, index, value}
    const u32 index = pkt_word(pkt, 0x10);
    const u32 value = pkt_word(pkt, 0x14);
    if (index < 0x20)
        m_sreg[index] = value;
    (void)hw;
}

void Iop::sif_cmd_init(Hw& hw, const u8* pkt) {
    // SifCmdChgAddrData_t {header(0x10), buf@0x10}: buf = the EE packet buffer.
    // The IOP's init handler (sif_sys_cmd_handler_init_from_ee): record the
    // EE receive address, raise CMDINIT, and mark RPC init done.
    const u32 buf = pkt_word(pkt, 0x10);
    m_ee_recv_addr = buf;
    m_sm_flag |= SIF_STAT_CMDINIT;
    m_sreg[0] = 1; // SIF_SREG_RPCINIT
    if (m_trace)
        std::fprintf(stderr, "[sif] INIT_CMD ee_recv_addr=0x%08X opt=0x%X\n",
                     buf, pkt_word(pkt, 0x0C));

    if (!m_bootend_sent) {
        m_bootend_sent = 1;
        deliver_rpcinit_sreg(*this, hw);
    }
}

void Iop::sif_cmd_reset(Hw& hw, const u8* pkt) {
    // SifCmdResetData_t {header, arglen, mode, arg[]}: IOP module reset /
    // load request. Full module loading (Loadcore/FileIo) is a follow-up;
    // for now log the request and complete the DMA synchronously (the game's
    // SifDmaStat poll returns 0 = done).
    const u32 arglen = pkt_word(pkt, 0x10);
    const char* name = arglen > 0 && arglen < 0x50 ? (const char*)(pkt + 0x18) : "";
    if (m_trace)
        std::fprintf(stderr, "[sif] RESET_CMD module='%.79s' (arglen=%u)\n", name, arglen);
    (void)hw;
}

void Iop::sif_cmd_bind(Hw& hw, const u8* pkt) {
    // SifRpcBindPkt_t: {header, rec_id@10, pkt_addr@14, rpc_id@18, cd@1C, sid@20}
    const u32 sid = pkt_word(pkt, 0x20);
    const char* name = nullptr;
    if (sif_rpc_module_lookup(sid, name)) {
        // Known HLE module: answer with a valid server-data handle
        // (Play!'s RPC_SERVERID_XOR scheme — the sid comes back with CALLs).
        constexpr u32 RPC_SERVERID_XOR = 0xACACACACu;
        sif_send_rend(hw, pkt, sid ^ RPC_SERVERID_XOR, 0xDEADBEF0, 0xDEADCAFE);
        if (m_trace)
            std::fprintf(stderr, "[sif] BIND sid=0x%08X -> module '%s'\n", sid, name);
    } else {
        // Unknown service: bind fails (sd = 0) like the real IOP when no
        // server is registered for the sid.
        sif_send_rend(hw, pkt, 0, 0, 0);
        if (m_trace)
            std::fprintf(stderr, "[sif] BIND sid=0x%08X -> no server (failed)\n", sid);
    }
}

void Iop::sif_cmd_call(Hw& hw, const u8* pkt) {
    // SifRpcCallPkt_t: {header, rec_id@10, pkt_addr@14, rpc_id@18, cd@1C,
    //                   rpc_number@20, send_size@24, recvbuf@28, recv_size@2C,
    //                   rmode@30, sd@34}
    const u32 sd = pkt_word(pkt, 0x34);
    const u32 rpc_number = pkt_word(pkt, 0x20);

    // Route by the server-data address space (like the real IOP sifrpc):
    //  - EE address -> the server is an EE thread: forward the packet to the
    //    EE's sifcmd dispatcher, whose CALL handler enqueues the request,
    //    links the queue and wakes the server thread.
    //  - fabricated sd (sid ^ XOR) -> HLE IOP module: handle it here.
    if (sd != 0 && (sd & 0xE0000000) == 0) {
        // EE-side server (sd < 0x20000000, EE RAM): relay to the EE.
        sif_deliver_to_ee(hw, pkt, 0x40);
        return;
    }
    if (sd != 0 && (sd & 0x80000000)) {
        // HLE IOP module (fabricated server data from the BIND reply).
        constexpr u32 RPC_SERVERID_XOR = 0xACACACACu;
        const u32 sid = sd ^ RPC_SERVERID_XOR;
        const char* name = nullptr;
        if (sif_rpc_module_lookup(sid, name)) {
            if (m_trace)
                std::fprintf(stderr, "[sif] CALL module='%s' rpc=0x%X\n", name, rpc_number);
            if (name && std::strcmp(name, "cdvdfsv") == 0) {
                sif_cdvdfsv(hw, pkt, sid, rpc_number);
                return;
            }
            if (name && std::strcmp(name, "sysmem") == 0) {
                // sysmem RPC: args in the EE send buffer, reply to recvbuf.
                const u32 send = pkt_word(pkt, 0x38);
                u32 a0 = 0, a1 = 0;
                if (hw.mem && send != 0) {
                    const u8* req = hw.mem->translate(send & 0x1FFFFFFF);
                    std::memcpy(&a0, req + 0, 4);
                    std::memcpy(&a1, req + 4, 4);
                }
                u32 result = 0;
                switch (rpc_number) {
                case 0x01: result = sysmem_alloc(a0); break;          // AllocateMemory
                case 0x02: result = sysmem_free(a0); break;           // FreeMemory
                case 0x05: result = sysmem_query_mem_size(); break;   // QueryMemSize
                case 0x06: result = m_iop_mem_size - m_iop_heap_ptr; break; // QueryMaxFreeMemSize
                case 0x07: result = m_iop_mem_size - m_iop_heap_ptr; break; // QueryTotalFreeMemSize
                default:
                    static std::set<u32> g_sysmem_reported;
                    if (g_sysmem_reported.insert(rpc_number).second)
                        std::fprintf(stderr, "[sysmem] unknown rpc method 0x%X (ack 0)\n", rpc_number);
                    result = 0;
                    break;
                }
                if (m_trace)
                    std::fprintf(stderr, "[sysmem] rpc=0x%X a0=0x%08X -> 0x%08X\n", rpc_number, a0, result);
                sif_rpc_reply(hw, pkt, sd, &result, 4);
                return;
            }
            if (name && std::strcmp(name, "cdvdfsv") == 0) {
                sif_cdvdfsv(hw, pkt, sid, rpc_number);
                return;
            }
            if (name && std::strcmp(name, "padman") == 0) {
                sif_padman(hw, pkt, sid, rpc_number);
                return;
            }
            // Other modules (fileio/loadcore/mcman): complete the request
            // with a successful (empty) reply so waiting clients proceed.
            sif_send_rend(hw, pkt, sd, 0, 0);
            return;
        }
    }
    // Unknown/zero server data: fail the call (rend with sd = 0).
    sif_send_rend(hw, pkt, 0, 0, 0);
    if (m_trace)
        std::fprintf(stderr, "[sif] CALL sd=0x%08X rpc=0x%X -> no server (failed)\n",
                     sd, rpc_number);
}

void Iop::sif_cmd_other_data(Hw& hw, const u8* pkt) {
    // SifRpcOtherDataPkt_t: {header, rec_id@10, pkt_addr@14, rpc_id@18,
    //                        recvbuf@1C, src@20, dest@24, size@28}
    const u32 src = pkt_word(pkt, 0x20);
    const u32 dst = pkt_word(pkt, 0x24);
    const u32 size = pkt_word(pkt, 0x28);
    // SifGetOtherData copies `size` bytes from IOP RAM (src) into EE RAM (dst).
    // With no real IOP modules there is nothing to send yet; still answer the
    // request so the client's wait completes (Play! Cmd_GetOtherData).
    const u32 esrc = src & 0x1FFFFFFF;
    const u32 edst = dst & 0x1FFFFFFF;
    if (hw.mem && size > 0 && esrc >= 0x1F000000 && esrc < 0x20000000) {
        const u32 off = esrc - 0x1F000000;
        if (off + size <= hw.iop_mem.size())
            std::memcpy(hw.mem->translate(edst), hw.iop_mem.data() + off, size);
    }
    if (m_trace)
        std::fprintf(stderr, "[sif] OTHERDATA src=0x%08X dst=0x%08X size=0x%X\n", src, dst, size);
    sif_send_rend(hw, pkt, 0, 0, 0);
}

void Iop::sif_cmd_rend(Hw& hw, const u8* pkt) {
    // RPC_END from the EE: completes an IOP-side RPC client. Our HLE never
    // initiates IOP->EE calls yet, so this is informational.
    (void)hw;
    if (m_trace)
        std::fprintf(stderr, "[sif] REND cid=0x%08X sd=0x%08X\n",
                     pkt_word(pkt, 0x20), pkt_word(pkt, 0x24));
}

void Iop::sif_send_rend(Hw& hw, const u8* request_pkt, u32 sd, u32 buf, u32 cbuf) {
    // SifRpcRendPkt_t: {header(0x10), rec_id@10, pkt_addr@14, rpc_id@18,
    //                   cd@1C, cid@20, sd@24, buf@28, cbuf@2C} (0x30 bytes)
    u8 rend[0x30] = {};
    const u32 psize_dsize = 0x30;
    const u32 dest = request_pkt ? pkt_word(request_pkt, 4) : 0;
    const u32 cid = SIF_CMD_RPC_END;
    // The client's wait semaphore rides in the header's opt field: the game's
    // _SifRpcEnd does `if (rend->hdr.opt) iSignalSema(rend->hdr.opt)` — echo
    // the request's opt (the BIND/CALL packet's semaid) back, or the client
    // wait never completes and every bind/call retry-loops forever.
    const u32 opt = request_pkt ? pkt_word(request_pkt, 0x0C) : 0;
    std::memcpy(rend + 0x00, &psize_dsize, 4);
    std::memcpy(rend + 0x04, &dest, 4);
    std::memcpy(rend + 0x08, &cid, 4);
    std::memcpy(rend + 0x0C, &opt, 4);
    const u32 rec_id = request_pkt ? pkt_word(request_pkt, 0x10) : 0;
    const u32 pkt_addr = request_pkt ? pkt_word(request_pkt, 0x14) : 0;
    const u32 rpc_id = request_pkt ? pkt_word(request_pkt, 0x18) : 0;
    const u32 cd = request_pkt ? pkt_word(request_pkt, 0x1C) : 0;
    const u32 orig_cid = request_pkt ? pkt_word(request_pkt, 8) : 0;
    std::memcpy(rend + 0x10, &rec_id, 4);
    std::memcpy(rend + 0x14, &pkt_addr, 4);
    std::memcpy(rend + 0x18, &rpc_id, 4);
    std::memcpy(rend + 0x1C, &cd, 4);
    std::memcpy(rend + 0x20, &orig_cid, 4); // command that completed
    std::memcpy(rend + 0x24, &sd, 4);
    std::memcpy(rend + 0x28, &buf, 4);
    std::memcpy(rend + 0x2C, &cbuf, 4);
    sif_deliver_to_ee(hw, rend, sizeof rend);
    if (m_trace)
        std::fprintf(stderr, "[sif] REND sent cd=0x%08X opt(sema)=%u sd=0x%08X orig_cid=0x%08X\n",
                     request_pkt ? pkt_word(request_pkt, 0x1C) : 0, opt,
                     request_pkt ? pkt_word(request_pkt, 0x24) : 0,
                     request_pkt ? pkt_word(request_pkt, 0x20) : 0);
}

void Iop::sif_deliver_to_ee(Hw& hw, const void* pkt, u32 size) {
    if (m_ee_recv_addr == 0) {
        // The EE has not registered a packet buffer yet: queue the packet.
        m_pending_ee.insert(m_pending_ee.end(), (const u8*)pkt, (const u8*)pkt + size);
        if (m_pending_ee.size() > 0x1000)
            m_pending_ee.clear(); // bring-up safety
        return;
    }
    if (!hw.mem)
        return;
    u8* dst = hw.mem->translate(m_ee_recv_addr & 0x1FFFFFFF);
    std::memcpy(dst, pkt, size);
    // Old-libkernel sifcmd: incoming packets are delivered by a SIF0
    // (DMAC channel 5) DMA completion — the game registered
    // AddDmacHandler(5, _SifCmdIntHandler) + EnableDmac(5) in sceSifInitCmd,
    // so raise the DMAC channel-5 interrupt (the INTC SBUS raise is kept for
    // kernels that register a SBUS handler instead).
    if (hw.runtime && hw.runtime->kernel) {
        hw.runtime->kernel->raise_dmac(5, 0);
        hw.runtime->kernel->raise_intc(1, 0);
    }
    // The game also polls IOP-side SIF command registers (0x1FFFFF70/80/88/90
    // in a cycle); 0x1FFFFF88 stays 0 until we mark it — write the packet size
    // as the "packet available" flag so the polling path sees fresh data.
    m_sif_regs[0x1FFFFF88] = size;
}

bool Iop::sif_rpc_module_lookup(u32 sid, const char*& name) const {
    auto it = m_sif_modules.find(sid);
    if (it == m_sif_modules.end())
        return false;
    name = it->second.c_str();
    return true;
}

// --- SIF system registers (old libkernel SifGetReg/SifSetReg, syscalls 0x7A/0x79) ---

u32 Iop::sif_get_sysreg(u32 reg) const {
    switch (reg) {
    case 1: return m_main_addr;  // MAINADDR
    case 2: return m_sub_addr;   // SUBADDR
    case 3: return m_ms_flag;    // MSFLAG
    case 4: return m_sm_flag;    // SMFLAG
    case 0x80000000: return 0;   // SIF reset address
    case 0x80000001: return 0;   // (cmd library marker)
    case 0x80000002: return 1;   // RPC initialized
    default: return 0;
    }
}

void Iop::sif_set_sysreg(Hw& hw, u32 reg, u32 value) {
    switch (reg) {
    case 1: m_main_addr = value; break; // MAINADDR: EE cmd_data address
    case 2: m_sub_addr = value; break;  // SUBADDR (EE side; informational)
    case 3: m_ms_flag |= value; break;  // MSFLAG: OR-in (SifSetMSFlag semantics)
    case 4: break;                      // SMFLAG is IOP-owned: ignore EE writes
    default: break;                     // 0x80000000/1/2: ignored by the IOP
    }
    (void)hw;
}

// --- CDVD -----------------------------------------------------------------------------

u32 Iop::cdvd_read(u32 lba, u8* buf, u32 sectors) {
    // With a disc image loaded the reads come from the ISO filesystem.
    if (cdvd && cdvd->is_open()) {
        if (!cdvd->read_sectors(lba, sectors, buf))
            return 0; // read error
        return sectors;
    }
    // No disc loaded: return zeros (HLE "no disc" behaviour).
    if (buf && sectors > 0)
        std::memset(buf, 0, sectors * 2048);
    return sectors; // pretend success
}

// --- cdvdfsv RPC server -----------------------------------------------------------------
//
// The EE's SCE libcdvd (old libkernel) BINDs per-function servers in the
// 0x80000592..0x8000059C range ("cdvdfsv") and SifCallRpc's them. The request
// payload stays in EE RAM (the packet's send field points at it; the IOP reads
// it over the shared bus). Replies are written into the request's EE recv
// buffer, followed by the REND packet (sif_rpc_reply).
//
// Sid map (ps2sdk ee/rpc/cdvd/src/libcdvd.c + SCE old libcdvd):
//   0x80000592 CD_SERVER_INIT       sceCdInit        {mode} -> CdInitPkt (versions)
//   0x80000593 CD_SERVER_READ       sceCdRead        {lba, sectors, buf, mode}
//   0x80000594 CD_SERVER_SEEK/READ  (n-cmd family)
//   0x80000596 CD_SERVER_POFF       power-off (XCDVDFSV)
//   0x80000597 CD_SERVER_SEARCHFILE sceCdSearchFile  {pad32, name[256], dest}
//   0x8000059A CD_SERVER_DISKREADY  sceCdDiskReady   {mode} -> 2 (ready)
// Unknown sids get a logged generic success so the client's wait completes
// (bring-up: the log line identifies what to implement next).

void Iop::sif_rpc_reply(Hw& hw, const u8* request_pkt, u32 sd, const void* data, u32 size) {
    if (data && size > 0 && hw.mem) {
        const u32 recvbuf = pkt_word(request_pkt, 0x28);
        const u32 recv_size = pkt_word(request_pkt, 0x2C);
        const u32 n = size < recv_size ? size : recv_size;
        if (recvbuf != 0 && n > 0)
            std::memcpy(hw.mem->translate(recvbuf & 0x1FFFFFFF), data, n);
    }
    sif_send_rend(hw, request_pkt, sd, 0, 0);
}

void Iop::sif_cdvdfsv(Hw& hw, const u8* pkt, u32 sid, u32 rpc_number) {
    // The request payload: EE address in the CALL packet's send field (0x38).
    const u32 send = pkt_word(pkt, 0x38);
    const u32 send_size = pkt_word(pkt, 0x24);
    const u8* req = (hw.mem && send != 0) ? hw.mem->translate(send & 0x1FFFFFFF) : nullptr;
    auto req_word = [&](u32 off) -> u32 { u32 v = 0; if (req) std::memcpy(&v, req + off, 4); return v; };

    switch (sid) {
    case 0x80000592: { // CD_SERVER_INIT: sceCdInit(mode) -> CdInitPkt
        if (m_trace)
            std::fprintf(stderr, "[cdvd] INIT mode=%u\n", req_word(0));
        // CdInitPkt {init_result, cdvdfsv_version, cdvdman_version, verbose}:
        // report SDK 2.x module versions (like retail rom0 modules).
        const u32 reply[4] = {1, 0x0205, 0x0205, 0};
        sif_rpc_reply(hw, pkt, sid, reply, sizeof reply);
        return;
    }
    case 0x80000593:   // CD_SERVER_READ: sceCdRead(lba, sectors, buf, mode)
    case 0x80000594: { // CD_SERVER_SEEK/read family (same argument shape)
        const u32 lba = req_word(0);
        const u32 sectors = req_word(4);
        const u32 buf = req_word(8);
        if (m_trace)
            std::fprintf(stderr, "[cdvd] READ lba=%u sectors=%u buf=0x%08X (send_size=%u)\n",
                         lba, sectors, buf, send_size);
        u32 done = 0;
        if (buf != 0 && sectors > 0 && hw.mem && cdvd && cdvd->is_open()) {
            // Read in chunks directly into EE RAM (the request's buffer).
            done = cdvd_read(lba, hw.mem->translate(buf & 0x1FFFFFFF), sectors);
        }
        // sceCdRead reply: {sectors read, error}
        const u32 reply[2] = {done, done == sectors ? 0 : 1};
        sif_rpc_reply(hw, pkt, sid, reply, sizeof reply);
        return;
    }
    case 0x80000597: { // CD_SERVER_SEARCHFILE: sceCdSearchFile
        // SearchFilePkt {u8 padding[32]; char name[256]; void* dest}:
        // reply is the 32-byte sceCdlFILE {lsn, size, name[16], date[8]}.
        char name[256] = {};
        if (req)
            std::memcpy(name, req + 32, sizeof name - 1);
        name[sizeof name - 1] = 0;
        u8 reply[32] = {};
        u32 lba = 0, size = 0;
        bool found = false;
        if (cdvd && cdvd->is_open())
            found = cdvd->find_file(name, lba, size);
        if (found) {
            std::memcpy(reply + 0, &lba, 4);
            std::memcpy(reply + 4, &size, 4);
            // name[16] at +8, date[8] at +24 stay zero.
            if (m_trace)
                std::fprintf(stderr, "[cdvd] SEARCHFILE '%s' -> lba=%u size=%u\n", name, lba, size);
        } else {
            if (m_trace)
                std::fprintf(stderr, "[cdvd] SEARCHFILE '%s' -> not found\n", name);
        }
        sif_rpc_reply(hw, pkt, sid, reply, sizeof reply);
        return;
    }
    case 0x80000596: // CD_SERVER_POFF (power-off request): ack, do nothing.
        sif_rpc_reply(hw, pkt, sid, nullptr, 0);
        return;
    case 0x8000059A: { // CD_SERVER_DISKREADY: sceCdDiskReady(mode) -> 2 = ready
        const u32 ready = (cdvd && cdvd->is_open()) ? 2 : 0;
        sif_rpc_reply(hw, pkt, sid, &ready, 4);
        return;
    }
    default: {
        // Log-once per sid so the next needed command is visible in the trace.
        static std::set<u32> reported;
        if (reported.insert(sid).second)
            std::fprintf(stderr, "[cdvd] cdvdfsv sid=0x%08X rpc=0x%X send=0x%08X (generic ack)\n",
                         sid, rpc_number, send);
        u32 ack = 1;
        sif_rpc_reply(hw, pkt, sid, &ack, 4);
        return;
    }
    }
}

// --- IOP sysmem (sid 0x80000003) -------------------------------------------------------
//
// The EE BINDs sysmem to allocate IOP RAM (heap, DMA buffers, module load
// space). RPC methods (Play! CSysmem::Invoke, old-libkernel protocol):
//   0x01 AllocateMemory(size)           -> addr (old: SifAllocate)
//   0x02 FreeMemory(addr)               -> 0
//   0x05 QueryMemSize()                 -> total
//   0x06 QueryMaxFreeMemSize()          -> largest free
//   0x07 QueryTotalFreeMemSize()        -> total free
// Unknown methods get a generic ack + a log line so the next one is visible.

u32 Iop::sysmem_alloc(u32 size) {
    std::lock_guard lk(m_mutex);
    // Old-libkernel sysmem aligns allocations to 0x10 and rounds up.
    size = (size + 0xF) & ~0xFu;
    if (size == 0)
        size = 0x10;
    if (m_iop_heap_ptr + size > m_iop_mem_size)
        return 0; // out of IOP memory
    const u32 addr = m_iop_heap_ptr;
    m_iop_heap_ptr += size;
    m_iop_blocks.push_back({addr, size});
    if (m_trace)
        std::fprintf(stderr, "[sysmem] alloc 0x%X -> 0x%05X (heap 0x%05X/%05X)\n",
                     size, addr, m_iop_heap_ptr, m_iop_mem_size);
    return addr;
}

u32 Iop::sysmem_free(u32 addr) {
    std::lock_guard lk(m_mutex);
    for (size_t i = 0; i < m_iop_blocks.size(); ++i) {
        if (m_iop_blocks[i].addr == addr) {
            m_iop_blocks.erase(m_iop_blocks.begin() + i);
            if (m_trace)
                std::fprintf(stderr, "[sysmem] free 0x%05X\n", addr);
            return 0;
        }
    }
    return -1; // not found
}

// --- Pad -------------------------------------------------------------------------------

Iop::PadState Iop::get_pad(int port) const {
    if (port < 0 || port > 1) return {0, 128, 128, 128, 128};
    return m_pad[port];
}

void Iop::set_pad(int port, PadState state) {
    if (port < 0 || port > 1) return;
    m_pad[port] = state;
}

// --- Memory card -----------------------------------------------------------------------

bool Iop::mc_read(int port, u32 sector, u8* buf) {
    (void)port;
    (void)sector;
    if (buf) std::memset(buf, 0, 512); // empty card sector
    return true;
}

bool Iop::mc_write(int port, u32 sector, const u8* buf) {
    (void)port;
    (void)sector;
    (void)buf;
    return true; // pretend write succeeded
}

// --- RPC -------------------------------------------------------------------------------

void Iop::register_rpc(const std::string& name, RpcHandler handler) {
    std::lock_guard lk(m_mutex);
    m_rpc_handlers[name] = std::move(handler);
}

u32 Iop::call_rpc(const std::string& name, const u8* in, u32 in_size, u8* out, u32 out_size) {
    std::lock_guard lk(m_mutex);
    auto it = m_rpc_handlers.find(name);
    if (it == m_rpc_handlers.end())
        return 0; // unhandled
    return it->second(in, in_size, out, out_size);
}

// --- padman RPC (input) -------------------------------------------------------------
//
// libpad (EE) BINDs padman (sids 0x80000100/01/0F/1F) and SifCallRpc's it to
// open ports and read pad state. We answer the init calls and serve idle pad
// data (no buttons pressed, centered analog sticks) so input init proceeds.
// The pad data packet the EE expects (32 bytes at +4 of the recv buffer):
//   +0  u16 buttons (0 = none pressed)
//   +24 u8 rx, +25 u8 ry, +26 u8 lx, +27 u8 ly (0x80 = centered)
//   ... analog button pressure bytes follow.

void Iop::sif_padman(Hw& hw, const u8* pkt, u32 sid, u32 rpc_number) {
    // Args live in the EE send buffer (CALL packet send field @0x38).
    const u32 send = pkt_word(pkt, 0x38);
    u32 a0 = 0;
    if (hw.mem && send != 0) {
        const u8* req = hw.mem->translate(send & 0x1FFFFFFF);
        if (req)
            std::memcpy(&a0, req + 0, 4);
    }
    // Reply buffer for pad data (CALL recvbuf @0x28, recv_size @0x2C).
    const u32 recvbuf = pkt_word(pkt, 0x28);
    const u32 recv_size = pkt_word(pkt, 0x2C);

    switch (rpc_number) {
    case 0x01:
    case 0x80000100: // Open(port, slot, ...) -> return handle in a0
        if (m_trace)
            std::fprintf(stderr, "[padman] Open port=%u\n", a0);
        {
            u32 result = 0; // handle 0 = ok
            sif_rpc_reply(hw, pkt, sid, &result, 4);
        }
        return;
    case 0x80000105: // SetMainMode
    case 0x8000010D: // Close
        if (m_trace)
            std::fprintf(stderr, "[padman] rpc=0x%X ack\n", rpc_number);
        {
            u32 result = 0;
            sif_rpc_reply(hw, pkt, sid, &result, 4);
        }
        return;
    default:
        // Any other call (e.g. pad-data read): serve idle pad state into the
        // recv buffer and ack.
        if (recvbuf != 0 && recv_size >= 32 && hw.mem) {
            u8 pad[32] = {};
            // buttons @+0 already 0 (none pressed)
            pad[24] = 0x80; // rx centered
            pad[25] = 0x80; // ry centered
            pad[26] = 0x80; // lx centered
            pad[27] = 0x80; // ly centered
            std::memcpy(hw.mem->translate(recvbuf & 0x1FFFFFFF), pad, 32);
        }
        if (m_trace) {
            static std::set<u32> g_pad_reported;
            if (g_pad_reported.insert(rpc_number).second)
                std::fprintf(stderr, "[padman] rpc=0x%X (idle pad, %u bytes)\n", rpc_number, recv_size);
        }
        {
            u32 result = 0;
            sif_rpc_reply(hw, pkt, sid, &result, 4);
        }
        return;
    }
}

// --- SIF command queue ----------------------------------------------------------------

void Iop::send_sif_cmd(const SifCmd& cmd) {
    std::lock_guard lk(m_mutex);
    m_sif_cmd_queue.push_back(cmd);
}

bool Iop::recv_sif_cmd(SifCmd& cmd) {
    std::lock_guard lk(m_mutex);
    if (m_sif_cmd_queue.empty())
        return false;
    cmd = std::move(m_sif_cmd_queue.front());
    m_sif_cmd_queue.erase(m_sif_cmd_queue.begin());
    return true;
}

} // namespace ee::rt
