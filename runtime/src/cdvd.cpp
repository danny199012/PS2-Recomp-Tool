// SPDX-License-Identifier: GPL-3.0-only
#include <ee/cdvd.hpp>

#include <algorithm>
#include <cstring>
#include <cstdio>

namespace ee::rt {

static constexpr u32 kSectorSize = 2048;

bool Cdvd::open(const std::string& path) {
    m_file.open(path, std::ios::binary | std::ios::ate);
    if (!m_file)
        return false;
    const auto size = m_file.tellg();
    m_file.seekg(0);
    m_sectors = u32(size / kSectorSize);
    parse_volume_descriptor();
    return true;
}

void Cdvd::close() {
    m_file.close();
    m_sectors = 0;
    m_root_lba = 0;
    m_root_size = 0;
}

bool Cdvd::read_sectors(u32 lba, u32 count, u8* buf) {
    if (!m_file.is_open() || lba + count > m_sectors)
        return false;
    m_file.clear();
    m_file.seekg(u64(lba) * kSectorSize);
    m_file.read(reinterpret_cast<char*>(buf), count * kSectorSize);
    return m_file.good();
}

void Cdvd::parse_volume_descriptor() {
    // ISO9660 primary volume descriptor at sector 16 (0x10).
    u8 buf[kSectorSize];
    if (!read_sectors(16, 1, buf))
        return;
    // Check for "CD001" at offset 1.
    if (std::memcmp(buf + 1, "CD001", 5) != 0)
        return;
    if (buf[0] != 1) // PVD type = 1
        return;
    // Root directory record at offset 156, 34 bytes.
    // Layout: length(1), ext_len(1), lba_le(4), lba_be(4), size_le(4), size_be(4), ...
    const u8* rec = buf + 156;
    m_root_lba = u32(rec[2]) | (u32(rec[3]) << 8) | (u32(rec[4]) << 16) | (u32(rec[5]) << 24);
    m_root_size = u32(rec[10]) | (u32(rec[11]) << 8) | (u32(rec[12]) << 16) | (u32(rec[13]) << 24);
}

std::vector<Cdvd::DirEntry> Cdvd::read_directory(u32 lba, u32 size) {
    std::vector<DirEntry> entries;
    std::vector<u8> data(size);
    if (!read_sectors(lba, (size + kSectorSize - 1) / kSectorSize, data.data()))
        return entries;

    size_t pos = 0;
    while (pos < data.size()) {
        const u8 len = data[pos];
        if (len == 0) {
            // Skip to next sector boundary.
            pos = (pos / kSectorSize + 1) * kSectorSize;
            if (pos >= data.size()) break;
            continue;
        }
        DirEntry e;
        e.lba = u32(data[pos + 2]) | (u32(data[pos + 3]) << 8) | (u32(data[pos + 4]) << 16) | (u32(data[pos + 5]) << 24);
        e.size = u32(data[pos + 10]) | (u32(data[pos + 11]) << 8) | (u32(data[pos + 12]) << 16) | (u32(data[pos + 13]) << 24);
        e.is_dir = (data[pos + 25] & 0x02) != 0;
        const u8 name_len = data[pos + 32];
        e.name.assign(reinterpret_cast<const char*>(data.data() + pos + 33), name_len);
        // Strip ;1 version suffix from file names.
        auto sc = e.name.find(';');
        if (sc != std::string::npos)
            e.name = e.name.substr(0, sc);
        entries.push_back(std::move(e));
        pos += len;
    }
    return entries;
}

bool Cdvd::find_file_recursive(const std::string& path, u32 root_lba, u32 root_size, u32& lba, u32& size) {
    // Split path by '/'.
    std::vector<std::string> parts;
    size_t start = 0;
    while (start < path.size()) {
        auto end = path.find('/', start);
        if (end == std::string::npos) end = path.size();
        parts.push_back(path.substr(start, end - start));
        start = end + 1;
    }
    if (parts.empty()) return false;

    u32 cur_lba = root_lba;
    u32 cur_size = root_size;

    for (const auto& part : parts) {
        auto entries = read_directory(cur_lba, cur_size);
        bool found = false;
        for (const auto& e : entries) {
            // ISO9660 is case-insensitive.
            std::string ename = e.name;
            std::string pname = part;
            std::transform(ename.begin(), ename.end(), ename.begin(), ::toupper);
            std::transform(pname.begin(), pname.end(), pname.begin(), ::toupper);
            if (ename == pname) {
                if (&part == &parts.back()) {
                    lba = e.lba;
                    size = e.size;
                    return true;
                }
                if (e.is_dir) {
                    cur_lba = e.lba;
                    cur_size = e.size;
                    found = true;
                    break;
                }
            }
        }
        if (!found) return false;
    }
    return false;
}

std::optional<std::string> Cdvd::find_boot_elf() {
    // Read SYSTEM.CNF from the root directory.
    u32 lba, size;
    if (!find_file_recursive("SYSTEM.CNF", m_root_lba, m_root_size, lba, size))
        return std::nullopt;
    // Read whole sectors (read_sectors always writes full 2048-byte sectors),
    // then truncate to the file's actual size.
    std::vector<u8> data(size_t((u64(size) + kSectorSize - 1) / kSectorSize) * kSectorSize);
    if (!read_sectors(lba, u32(data.size() / kSectorSize), data.data()))
        return std::nullopt;
    data.resize(std::min<size_t>(data.size(), size));
    // Parse for BOOT2 = cdrom0:\path;1
    std::string text(data.begin(), data.begin() + std::min<size_t>(data.size(), 4096));
    auto pos = text.find("BOOT2");
    if (pos == std::string::npos)
        return std::nullopt;
    auto eq = text.find('=', pos);
    if (eq == std::string::npos)
        return std::nullopt;
    // Extract the path, strip whitespace, cdrom0:\ prefix, ;1 suffix.
    std::string line = text.substr(eq + 1);
    // Trim leading whitespace.
    size_t s = line.find_first_not_of(" \t");
    if (s == std::string::npos) return std::nullopt;
    line = line.substr(s);
    // Find end of line.
    auto eol = line.find_first_of("\r\n");
    if (eol != std::string::npos) line = line.substr(0, eol);
    // Strip cdrom0:\ etc.
    if (line.substr(0, 8) == "cdrom0:\\") line = line.substr(8);
    else if (line.substr(0, 7) == "cdrom0:") line = line.substr(7);
    // Strip leading slashes.
    while (!line.empty() && (line.front() == 0x5C || line.front() == '/'))
        line.erase(line.begin());
    // Strip ;1.
    auto sc = line.find(';');
    if (sc != std::string::npos) line = line.substr(0, sc);
    std::fprintf(stderr, "[cdvd] boot ELF: %s\n", line.c_str());
    return line;
}

std::optional<std::vector<u8>> Cdvd::read_file(const std::string& path) {
    u32 lba, size;
    if (!find_file_recursive(path, m_root_lba, m_root_size, lba, size))
        return std::nullopt;
    // Read whole sectors, then truncate to the file's actual size.
    std::vector<u8> data(size_t((u64(size) + kSectorSize - 1) / kSectorSize) * kSectorSize);
    if (!read_sectors(lba, u32(data.size() / kSectorSize), data.data()))
        return std::nullopt;
    data.resize(size);
    return data;
}

} // namespace ee::rt
