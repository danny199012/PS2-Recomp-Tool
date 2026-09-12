// SPDX-License-Identifier: GPL-3.0-only
// CDVD ISO reader tests: build a minimal ISO9660 image on disk, then verify
// volume parsing, SYSTEM.CNF/BOOT2 discovery, file search and sector reads.

#include <ee/cdvd.hpp>

#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

using namespace ee;
using ee::rt::Cdvd;

static int g_failures = 0;
#define CHECK(cond)                                                                      \
    do {                                                                                 \
        if (!(cond)) {                                                                   \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                  \
            ++g_failures;                                                                \
        }                                                                                \
    } while (0)

namespace {

constexpr u32 kSector = 2048;

// ISO9660 directory record (used to fabricate the root dir / SYSTEM.CNF / a
// test file).
struct Rec {
    u32 lba;
    u32 size;
    bool is_dir;
    const char* name;
};

void put_rec(u8* p, const Rec& r) {
    const u8 name_len = u8(std::strlen(r.name));
    // ISO9660: record length = 33 + name_len, rounded up to an even total.
    const u8 len = u8((33 + name_len + 1) & ~1u);
    std::memset(p, 0, len);
    p[0] = len;
    p[2] = u8(r.lba), p[3] = u8(r.lba >> 8), p[4] = u8(r.lba >> 16), p[5] = u8(r.lba >> 24);
    p[10] = u8(r.size), p[11] = u8(r.size >> 8), p[12] = u8(r.size >> 16), p[13] = u8(r.size >> 24);
    p[25] = r.is_dir ? 0x02 : 0x00;
    p[32] = name_len;
    std::memcpy(p + 33, r.name, name_len);
    if (name_len % 2 == 0)
        p[33 + name_len] = 0; // pad to even length
}

// Build a minimal image:
//   sector 16: PVD (root dir at LBA 18)
//   LBA 18-19: root dir: ".", "..", "SYSTEM.CNF;1" (LBA 20), "DATA" dir (LBA 22),
//              "TEST.DAT;1" (LBA 24)
//   LBA 20:    SYSTEM.CNF content
//   LBA 22:    DATA dir: "NESTED.TXT;1" (LBA 26)
//   LBA 24/26: file contents
bool build_test_iso(const std::string& path) {
    std::vector<u8> img(30 * kSector, 0);

    // Primary volume descriptor.
    u8* pvd = img.data() + 16 * kSector;
    pvd[0] = 1;
    std::memcpy(pvd + 1, "CD001", 5);
    const u32 root_lba = 18, root_size = 2 * kSector;
    pvd[156 + 0] = 34; // root record length
    pvd[156 + 2] = u8(root_lba), pvd[156 + 3] = u8(root_lba >> 8);
    pvd[156 + 4] = u8(root_lba >> 16), pvd[156 + 5] = u8(root_lba >> 24);
    pvd[156 + 10] = u8(root_size), pvd[156 + 11] = u8(root_size >> 8);
    pvd[156 + 12] = u8(root_size >> 16), pvd[156 + 13] = u8(root_size >> 24);
    pvd[156 + 25] = 0x02;

    // Root directory (spans 2 sectors).
    u8* root = img.data() + root_lba * kSector;
    put_rec(root + 0, {root_lba, root_size, true, "."});
    root[32] = 1;
    root[33] = 0; // "." is encoded as a single zero byte
    root[0] = 34;
    put_rec(root + 34, {root_lba, root_size, true, "."});
    (root + 34)[32] = 1;
    (root + 34)[33] = 0;
    (root + 34)[0] = 34;
    put_rec(root + 68, {20, 68, false, "SYSTEM.CNF;1"});
    put_rec(root + 114, {22, kSector, true, "DATA"});
    put_rec(root + 152, {24, 40, false, "TEST.DAT;1"});

    // DATA directory.
    u8* data = img.data() + 22 * kSector;
    put_rec(data + 0, {22, kSector, true, "."});
    data[32] = 1;
    data[33] = 0;
    data[0] = 34;
    put_rec(data + 34, {22, kSector, true, "."});
    (data + 34)[32] = 1;
    (data + 34)[33] = 0;
    (data + 34)[0] = 34;
    put_rec(data + 68, {26, 12, false, "NESTED.TXT;1"});
    // SYSTEM.CNF.
    const char* cnf = "BOOT2 = cdrom0:\\SLUS_210.66;1\r\nVER = 1.00\r\n";
    std::memcpy(img.data() + 20 * kSector, cnf, std::strlen(cnf));

    // File contents with recognizable markers.
    std::memcpy(img.data() + 24 * kSector, "TESTDAT@0x001800", 16);
    std::memcpy(img.data() + 26 * kSector, "NESTEDOK", 8);

    std::ofstream f(path, std::ios::binary);
    f.write(reinterpret_cast<const char*>(img.data()), img.size());
    return f.good();
}

} // namespace

int main() {
    const std::string path = "test_cdvd_tmp.iso";
    CHECK(build_test_iso(path));

    Cdvd cdvd;
    CHECK(cdvd.open(path));
    CHECK(cdvd.is_open());
    CHECK(cdvd.sector_count() >= 30);

    // Boot ELF discovery through SYSTEM.CNF.
    const auto boot = cdvd.find_boot_elf();
    CHECK(boot.has_value());
    if (boot)
        CHECK(*boot == "SLUS_210.66");

    // Root file read.
    const auto testdat = cdvd.read_file("TEST.DAT");
    CHECK(testdat.has_value());
    if (testdat) {
        CHECK(testdat->size() == 40);
        CHECK(std::memcmp(testdat->data(), "TESTDAT@0x001800", 16) == 0);
    }

    // Nested directory file.
    const auto nested = cdvd.read_file("DATA/NESTED.TXT");
    CHECK(nested.has_value());
    if (nested) {
        CHECK(nested->size() == 12);
        CHECK(std::memcmp(nested->data(), "NESTEDOK", 8) == 0);
    }

    // Extent lookup (cdvdfsv SearchFile path).
    u32 lba = 0, size = 0;
    CHECK(cdvd.find_file("data/nested.txt", lba, size));
    CHECK(lba == 26);
    CHECK(size == 12);

    // Missing file.
    CHECK(!cdvd.read_file("DOES.NOT.EXIST").has_value());
    CHECK(!cdvd.find_file("DOES.NOT.EXIST", lba, size));

    // Raw sector reads.
    u8 buf[kSector];
    CHECK(cdvd.read_sectors(24, 1, buf));
    CHECK(std::memcmp(buf, "TESTDAT@0x001800", 16) == 0);
    CHECK(!cdvd.read_sectors(9999, 4, buf)); // out of range
    CHECK(!cdvd.read_sectors(28, 4, buf));   // past EOF

    cdvd.close();
    CHECK(!cdvd.is_open());

    std::remove(path.c_str());

    if (g_failures == 0) {
        std::printf("cdvd: all tests passed\n");
        return 0;
    }
    std::printf("cdvd: %d failure(s)\n", g_failures);
    return 1;
}
