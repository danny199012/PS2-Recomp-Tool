// SPDX-License-Identifier: GPL-3.0-only
#include <ee/iop.hpp>
#include <ee/hw.hpp>

#include <cstring>

namespace ee::rt {

Iop::Iop() {
    // Default pad state: no buttons pressed, centered sticks.
    for (int i = 0; i < 2; ++i)
        m_pad[i] = {0x0000, 128, 128, 128, 128};
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

// --- SIF registers -------------------------------------------------------------------

u32 Iop::sif_read_reg(u32 addr) const {
    // SIF EE-side registers: 0x1000F200..0x1000F260
    // SIF IOP-side registers: 0x1000F300..0x1000F340
    switch (addr) {
    case 0x1000F200: return m_sif_ms_f200;
    case 0x1000F204: return m_sif_ms_f204;
    case 0x1000F220: return m_sif_ms_f220;
    case 0x1000F300: return m_sif_sm_f200;
    case 0x1000F304: return m_sif_sm_f204;
    case 0x1000F320: return m_sif_sm_f220;
    default: return 0;
    }
}

void Iop::sif_write_reg(Hw& hw, u32 addr, u32 value) {
    switch (addr) {
    case 0x1000F200: m_sif_ms_f200 = value; break;
    case 0x1000F204: m_sif_ms_f204 = value; break;
    case 0x1000F220: m_sif_ms_f220 = value; break;
    case 0x1000F300: m_sif_sm_f200 = value; break;
    case 0x1000F304: m_sif_sm_f204 = value; break;
    case 0x1000F320: m_sif_sm_f220 = value; break;
    case 0x1000F240: // SIF_INIT (reset SIF)
        m_sif_ms_f200 = 0;
        m_sif_sm_f200 = 0;
        break;
    default: break;
    }
    (void)hw;
}

// --- CDVD -----------------------------------------------------------------------------

u32 Iop::cdvd_read(u32 lba, u8* buf, u32 sectors) {
    // HLE: return zeros (no disc loaded). Real implementation would read from ISO.
    if (buf && sectors > 0)
        std::memset(buf, 0, sectors * 2048);
    return sectors; // pretend success
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
