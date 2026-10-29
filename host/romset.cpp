// ROM access: loose files, then zip archives (stored or deflated entries, read with zlib), and the decryption of
// the IGS027A external ROM (own implementation; key table and bit pattern from MAME pgmcrypt.cpp, BSD-3-Clause,
// LICENSE-MAME.txt).
#include "romset.h"
#include <zlib.h>
#include <dirent.h>
#include <cstdio>
#include <cstring>
#include <strings.h>

namespace romset {
static std::string g_base;
struct Entry { std::string zip, name; uint32_t method, csize, usize, local_off; };
static std::vector<Entry> g_entries;

static uint16_t u16(const uint8_t* p) { return p[0] | p[1] << 8; }
static uint32_t u32(const uint8_t* p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }

static void scan_zip(const std::string& path) {
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return;
    std::fseek(f, 0, SEEK_END);
    long size = std::ftell(f);
    long tail = size < 66000 ? size : 66000;          // end-of-central-directory record + comment
    std::vector<uint8_t> t(tail);
    std::fseek(f, size - tail, SEEK_SET);
    if (std::fread(t.data(), 1, tail, f) != (size_t)tail) { std::fclose(f); return; }
    long e = -1;
    for (long i = tail - 22; i >= 0; i--) if (u32(&t[i]) == 0x06054b50) { e = i; break; }
    if (e < 0) { std::fclose(f); return; }
    uint32_t cd_size = u32(&t[e + 12]), cd_off = u32(&t[e + 16]);
    std::vector<uint8_t> cd(cd_size);
    std::fseek(f, cd_off, SEEK_SET);
    if (std::fread(cd.data(), 1, cd_size, f) != cd_size) { std::fclose(f); return; }
    std::fclose(f);
    for (uint32_t p = 0; p + 46 <= cd_size && u32(&cd[p]) == 0x02014b50;) {
        uint32_t nlen = u16(&cd[p + 28]), xlen = u16(&cd[p + 30]), clen = u16(&cd[p + 32]);
        Entry en{path, std::string((const char*)&cd[p + 46], nlen), u16(&cd[p + 10]), u32(&cd[p + 20]),
                 u32(&cd[p + 24]), u32(&cd[p + 42])};
        size_t slash = en.name.rfind('/');
        if (slash != std::string::npos) en.name = en.name.substr(slash + 1);
        g_entries.push_back(en);
        p += 46 + nlen + xlen + clen;
    }
}

static void scan_dir(const std::string& dir) {
    if (DIR* d = opendir(dir.c_str())) {
        while (dirent* e = readdir(d)) {
            std::string n = e->d_name;
            if (n.size() > 4 && strcasecmp(n.c_str() + n.size() - 4, ".zip") == 0) scan_zip(dir + "/" + n);
        }
        closedir(d);
    }
}

void init(const std::string& base) {
    g_base = base;
    g_entries.clear();
    scan_dir(base);
    scan_dir(base + "/rom");
}

static bool read_zip(const Entry& en, std::vector<uint8_t>& out) {
    FILE* f = std::fopen(en.zip.c_str(), "rb");
    if (!f) return false;
    uint8_t h[30];
    bool ok = std::fseek(f, en.local_off, SEEK_SET) == 0 && std::fread(h, 1, 30, f) == 30 && u32(h) == 0x04034b50;
    if (ok) ok = std::fseek(f, en.local_off + 30 + u16(h + 26) + u16(h + 28), SEEK_SET) == 0;
    std::vector<uint8_t> c(en.csize);
    if (ok) ok = std::fread(c.data(), 1, en.csize, f) == en.csize;
    std::fclose(f);
    if (!ok) return false;
    out.resize(en.usize);
    if (en.method == 0) { if (en.csize != en.usize) return false; std::memcpy(out.data(), c.data(), en.usize); return true; }
    if (en.method != 8) return false;
    z_stream z{};
    if (inflateInit2(&z, -MAX_WBITS) != Z_OK) return false;
    z.next_in = c.data(); z.avail_in = en.csize;
    z.next_out = out.data(); z.avail_out = en.usize;
    int r = inflate(&z, Z_FINISH);
    inflateEnd(&z);
    return r == Z_STREAM_END && z.total_out == en.usize;
}

bool read(const std::string& name, std::vector<uint8_t>& out) {
    if (FILE* f = std::fopen((g_base + "/rom/" + name).c_str(), "rb")) {
        std::fseek(f, 0, SEEK_END); long n = std::ftell(f); std::fseek(f, 0, SEEK_SET);
        out.resize(n);
        bool ok = std::fread(out.data(), 1, n, f) == (size_t)n;
        std::fclose(f);
        return ok;
    }
    for (const Entry& en : g_entries)
        if (strcasecmp(en.name.c_str(), name.c_str()) == 0 && read_zip(en, out)) return true;
    std::printf("[rom] %s not found in %s/rom or a .zip in %s\n", name.c_str(), g_base.c_str(), g_base.c_str());
    return false;
}

#include "df_key.inc"
// Each 16-bit word is XORed with single bits chosen by its word index (mask, value, bit, equal: xor when
// (i & mask) == value if equal, else when it differs) and with the key table in the high byte.
bool arm_rom(std::vector<uint8_t>& out) {
    if (FILE* f = std::fopen((g_base + "/rom/arm_ext.bin").c_str(), "rb")) { std::fclose(f); return read("arm_ext.bin", out); }
    if (!read("v105_32m.u26", out) || out.size() != 0x400000) return false;
    static const struct { uint32_t mask, value; uint16_t bit; bool equal; } kBits[] = {
        {0x040080, 0x000080, 0x0001, false}, {0x104008, 0x104008, 0x0002, true}, {0x080030, 0x080010, 0x0004, true},
        {0x000042, 0x000042, 0x0008, false}, {0x008100, 0x008000, 0x0010, true}, {0x002004, 0x000004, 0x0020, false},
        {0x011800, 0x010000, 0x0040, false}, {0x004820, 0x004820, 0x0080, true}};
    for (uint32_t i = 0; i < out.size() / 2; i++) {
        uint16_t x = (uint16_t)(out[2 * i] | out[2 * i + 1] << 8);
        for (const auto& b : kBits) if (((i & b.mask) == b.value) == b.equal) x ^= b.bit;
        x ^= (uint16_t)(kDfrontKey[(i >> 1) & 0xff] << 8);
        out[2 * i] = (uint8_t)x; out[2 * i + 1] = (uint8_t)(x >> 8);
    }
    return true;
}
}
