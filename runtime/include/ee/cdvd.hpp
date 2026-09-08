// SPDX-License-Identifier: GPL-3.0-only
#pragma once

// CDVD ISO reader: reads PS2 game disc images (.ISO / .BIN).
// Supports ISO9660 + UDF (basic), finds the SYSTEM.CNF / BOOT2 ELF,
// and provides sector reads for the IOP CDVD HLE.

#include <ee/types.hpp>

#include <fstream>
#include <optional>
#include <string>
#include <vector>

namespace ee::rt {

class Cdvd {
public:
    Cdvd() = default;

    bool open(const std::string& path);
    bool is_open() const { return m_file.is_open(); }
    void close();

    // Read raw sectors (2048 bytes each) starting at LBA.
    bool read_sectors(u32 lba, u32 count, u8* buf);

    // Find the boot ELF path from SYSTEM.CNF.
    std::optional<std::string> find_boot_elf();

    // Read a file by path from the ISO filesystem (ISO9660 Level 1).
    std::optional<std::vector<u8>> read_file(const std::string& path);

    // Get total sector count.
    u32 sector_count() const { return m_sectors; }

    // Current disc type (CD=0x12, DVD=0x14, etc.).
    u32 disc_type() const { return 0x14; } // DVD for now

private:
    std::ifstream m_file;
    u32 m_sectors = 0;

    // ISO9660 primary volume descriptor info.
    u32 m_root_lba = 0;
    u32 m_root_size = 0;

    void parse_volume_descriptor();
    struct DirEntry {
        std::string name;
        u32 lba;
        u32 size;
        bool is_dir;
    };
    std::vector<DirEntry> read_directory(u32 lba, u32 size);
    bool find_file_recursive(const std::string& path, u32 root_lba, u32 root_size, u32& lba, u32& size);
};

} // namespace ee::rt
