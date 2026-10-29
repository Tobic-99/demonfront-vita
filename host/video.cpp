// IGS023 video, software renderer (see video.h).
//
// Formats:
//   palette 0xa00000: 0xa00 words xRGB555; sprites 0x000-0x3ff (32 x 32), bg 0x400-0x7ff (32 x 32), text 0x800-0x9ff (32 x 16)
//   text map  (video RAM 0x904000): 64x32 entries of 2 words: tile, attr (bits 1-5 palette, 6 flip x, 7 flip y);
//             tiles 8x8 4 bpp, 32 bytes, two pixels per byte (left = low nibble); pen 15 transparent
//   bg map    (video RAM 0x900000): 64x16 entries, same format; tiles 32x32 5 bpp, 640 bytes, a bit stream read
//             LSB first, each pixel a 5-bit field LSB first; pen 31 transparent; scroll per line from 0x907000
//   both tile sets live in one ROM region: BIOS text tiles at 0, game tiles at 0x180000
//   sprites   list of 5 words per sprite in main RAM, copied by DMA at line 220, decoded at vblank;
//             B ROM ('mask'): at the sprite's word offset, 2 words = A word address << 2, then per line and per
//             16 pixels one mask word (bit set = transparent, LSB = leftmost); A ROM ('colour'): 5-bit pixels,
//             3 per 16-bit word, only for the opaque pixels; zoom tables at 0xb01000.
// Both tile sets are decoded to one byte per pixel at load time, so the layers are drawn as spans.
#include "video.h"
#include "romset.h"
#include "board.h"
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <vector>

namespace video {
static std::vector<uint8_t> bgpix;        // bg tiles, 32x32 bytes each
static std::vector<uint8_t> txpix;        // text tiles, 8x8 bytes each
static uint32_t bg_count;
static std::vector<uint16_t> sprA, sprB;  // colour / mask ROM words
static uint32_t maskA, maskB;
static Sprite list[256];
static int nsprites;

static bool load_into(const std::string& name, std::vector<uint8_t>& dst, size_t at) {
    std::vector<uint8_t> b;
    if (!romset::read(name, b)) return false;
    if (dst.size() < at + b.size()) dst.resize(at + b.size());
    std::memcpy(dst.data() + at, b.data(), b.size());
    return true;
}
static bool load_words(const std::vector<std::pair<std::string, size_t>>& parts, size_t bytes, std::vector<uint16_t>& w) {
    std::vector<uint8_t> b(bytes, 0);
    for (auto& p : parts)
        if (!load_into(p.first, b, p.second)) return false;
    b.resize(bytes);
    w.resize(bytes / 2);
    std::memcpy(w.data(), b.data(), bytes);   // little-endian words, like the host
    return true;
}

bool load_gfx() {
    std::vector<uint8_t> tiles(0xa00000, 0);
    if (!load_into("pgm_t01s.rom", tiles, 0) || !load_into("t04501.u29", tiles, 0x180000)) return false;
    tiles.resize(0xa00000);
    bg_count = (uint32_t)(tiles.size() / 640);
    bgpix.resize((size_t)bg_count * 1024);
    for (uint32_t t = 0; t < bg_count; t++)
        for (int i = 0; i < 1024; i++) {
            uint32_t bit = t * 5120u + i * 5;
            uint32_t v = tiles[bit >> 3] | ((bit >> 3) + 1 < tiles.size() ? tiles[(bit >> 3) + 1] << 8 : 0);
            bgpix[(size_t)t * 1024 + i] = (v >> (bit & 7)) & 31;
        }
    txpix.resize(0x10000 * 64);
    for (uint32_t t = 0; t < 0x10000; t++)
        for (int i = 0; i < 64; i++) {
            uint8_t b = tiles[t * 32 + i / 2];
            txpix[t * 64 + i] = (i & 1) ? (b >> 4) : (b & 15);
        }
    if (!load_words({{"a04501.u3", 0}, {"a04502.u4", 0x800000}, {"a04503.u6", 0x1000000}}, 0x2000000, sprA)) return false;
    if (!load_words({{"b04501.u9", 0}, {"b04502.u11", 0x800000}}, 0x1000000, sprB)) return false;
    maskA = (uint32_t)sprA.size() - 1; maskB = (uint32_t)sprB.size() - 1;
    return true;
}

static const uint16_t SPRITE_RAM_MASK[5] = {0xffff, 0xfbff, 0x7fff, 0xffff, 0xffff};

void sprite_dma() {
    using namespace board;
    if (!(vregs.ctrl & 1)) return;
    const uint16_t* src = main_ram;
    for (int i = 0, offs = 0; i < 256; i++) {
        for (int k = 0; k < 5; k++) sprite_buf[i * 8 + k] = src[offs++] & SPRITE_RAM_MASK[k];
        if ((sprite_buf[i * 8 + 4] & 0x7fff) == 0) break;
    }
}

static int sext(int v, int bits) { return (v & (1 << (bits - 1))) ? v - (1 << bits) : v; }

void latch_sprites() {
    using namespace board;
    nsprites = 0;
    for (int n = 0; n < 256; n++) {
        const uint16_t* s = &sprite_buf[n * 8];
        if ((s[4] & 0x7fff) == 0) break;
        Sprite& p = list[nsprites++];
        p.xgrow = s[0] >> 15; int xz = (s[0] >> 11) & 15; p.x = sext(s[0] & 0x7ff, 11);
        p.ygrow = s[1] >> 15; int yz = (s[1] >> 11) & 15; p.y = sext(s[1] & 0x3ff, 10);
        p.flip = (s[2] >> 13) & 3; p.color = (s[2] >> 8) & 31; p.pri = (s[2] >> 7) & 1;
        p.offs = ((uint32_t)(s[2] & 0x7f) << 16) | s[3];
        p.w = (s[4] >> 9) & 0x3f; p.h = s[4] & 0x1ff;
        if (p.xgrow) xz = 0x10 - xz;
        if (p.ygrow) yz = 0x10 - yz;
        auto zoom = [](int z) -> uint32_t {
            if (z >= 0x10) return 0;
            if (z == 0xf) return 1;
            return ((uint32_t)board::vregs.zoom[z * 2] << 16) | board::vregs.zoom[z * 2 + 1];
        };
        p.xzoom = zoom(xz); p.yzoom = zoom(yz);
    }
}

void snapshot(Frame& f) {
    using namespace board;
    std::memcpy(f.video_ram, video_ram, sizeof f.video_ram);
    std::memcpy(f.palette, palette, sizeof f.palette);
    f.bg_yscroll = vregs.bg_yscroll; f.bg_xscroll = vregs.bg_xscroll;
    f.tx_yscroll = vregs.tx_yscroll; f.tx_xscroll = vregs.tx_xscroll;
    f.ctrl = vregs.ctrl;
    f.nsprites = nsprites;
    std::memcpy(f.sprites, list, sizeof(Sprite) * nsprites);
}

// ---- frame buffer: palette indices + priority (bit 0 = sprite pixel here, bit 1 = opaque bg pixel) ----
static uint16_t fb[H][W];
static uint8_t pri[H][W];

static void draw_bg(const Frame& f, int y0, int y1) {
    const uint16_t* map = f.video_ram;                 // 0x900000
    const uint16_t* rowscroll = f.video_ram + 0x3800;  // 0x907000
    for (int y = y0; y < y1; y++) {
        int my = (y + (int16_t)f.bg_yscroll) & 511;
        int mx = ((int16_t)f.bg_xscroll + (int16_t)rowscroll[y]) & 2047;
        const uint16_t* row = map + (my >> 5) * 128;
        uint16_t* d = fb[y];
        uint8_t* p = pri[y];
        for (int x = 0; x < W;) {
            const uint16_t* e = row + ((mx >> 5) & 63) * 2;
            uint16_t code = e[0], attr = e[1];
            int py = (attr & 0x80) ? 31 - (my & 31) : (my & 31);
            const uint8_t* src = &bgpix[(size_t)(code % bg_count) * 1024 + py * 32];
            uint16_t base = (uint16_t)(0x400 + ((attr >> 1) & 31) * 32);
            int px = mx & 31, n = 32 - px;
            if (n > W - x) n = W - x;
            if (attr & 0x40) {
                for (int i = 0; i < n; i++) {
                    uint8_t c = src[31 - (px + i)];
                    if (c != 31) { d[x + i] = base + c; p[x + i] = 2; }
                }
            } else {
                for (int i = 0; i < n; i++) {
                    uint8_t c = src[px + i];
                    if (c != 31) { d[x + i] = base + c; p[x + i] = 2; }
                }
            }
            x += n; mx += n;
        }
    }
}

static void draw_text(const Frame& f, int y0, int y1) {
    const uint16_t* map = f.video_ram + 0x2000;   // 0x904000
    for (int y = y0; y < y1; y++) {
        int my = (y + (int16_t)f.tx_yscroll) & 255;
        int mx = (int16_t)f.tx_xscroll & 511;
        const uint16_t* row = map + (my >> 3) * 128;
        uint16_t* d = fb[y];
        for (int x = 0; x < W;) {
            const uint16_t* e = row + ((mx >> 3) & 63) * 2;
            uint16_t code = e[0], attr = e[1];
            int py = (attr & 0x80) ? 7 - (my & 7) : (my & 7);
            const uint8_t* src = &txpix[(size_t)code * 64 + py * 8];
            uint16_t base = (uint16_t)(0x800 + ((attr >> 1) & 31) * 16);
            int px = mx & 7, n = 8 - px;
            if (n > W - x) n = W - x;
            for (int i = 0; i < n; i++) {
                int sx = (attr & 0x40) ? 7 - (px + i) : px + i;
                uint8_t c = src[sx];
                if (c != 15) d[x + i] = base + c;
            }
            x += n; mx += n;
        }
    }
}

// ---- sprites ----
struct SpriteReader {
    uint32_t a, b;   // A word index, B word index
    int abit;
    int pix() {
        int v = (sprA[a & maskA] >> abit) & 31;
        abit += 5;
        if (abit >= 15) { a++; abit = 0; }
        return v;
    }
};

static inline void put(int x, int y, int p, uint16_t c) {
    if ((unsigned)x >= (unsigned)W) return;
    uint8_t& pr = pri[y][x];
    if (!(pr & 1)) {
        if (!p || !(pr & 2)) fb[y][x] = c;
    }
    pr |= 1;
}

// One source line of a sprite; xzoom/xgrow as in the hardware's zoom tables (bit set = shrink/stretch this column)
static void sprite_line(SpriteReader& r, const Sprite& s, int y, bool draw, int realx) {
    if (!s.xzoom) {   // unzoomed: the common case
        int xc = 0;
        for (int g = 0; g < s.w; g++) {
            uint16_t m = sprB[r.b & maskB];
            r.b++;
            if (m == 0xffff) { xc += 16; continue; }
            for (int i = 0; i < 16; i++, m >>= 1, xc++) {
                if (m & 1) continue;
                uint16_t c = (uint16_t)(r.pix() + s.color * 32);
                if (draw) put((s.flip & 1) ? s.x + realx - xc : s.x + xc, y, s.pri, c);
            }
        }
        return;
    }
    int xc = 0, xo = 0;
    for (int g = 0; g < s.w; g++) {
        uint16_t m = sprB[r.b & maskB];
        for (int i = 0; i < 16; i++, m >>= 1) {
            bool zb = (s.xzoom >> (xo & 31)) & 1;
            xo++;
            int reps = (s.xgrow || !zb) ? (zb ? 2 : 1) : 0;
            if (!(m & 1)) {
                uint16_t c = (uint16_t)(r.pix() + s.color * 32);
                for (int k = 0; k < reps; k++, xc++)
                    if (draw) put((s.flip & 1) ? s.x + realx - xc : s.x + xc, y, s.pri, c);
            } else {
                xc += reps;
            }
        }
        r.b++;
    }
}

// Only lines y0..y1-1 are drawn (bands render on different threads); the source is read the same way.
static void draw_sprite(const Sprite& s, int y0, int y1) {
    SpriteReader r;
    r.b = s.offs;
    r.a = (((uint32_t)sprB[(r.b + 1) & maskB] << 16) | sprB[r.b & maskB]) >> 2;
    r.abit = 0;
    r.b += 2;
    auto zbit = [](uint32_t z, int i) { return z ? (bool)((z >> (i & 31)) & 1) : false; };
    int realy = 0, realx = 0;
    for (int i = 0; i < s.h; i++) { bool zb = zbit(s.yzoom, i); if (s.ygrow || !zb) realy += zb ? 2 : 1; }
    for (int i = 0; i < s.w * 16; i++) { bool zb = zbit(s.xzoom, i); if (s.xgrow || !zb) realx += zb ? 2 : 1; }
    realy--; realx--;
    int yc = 0;
    for (int line = 0; line < s.h; line++) {
        bool zb = zbit(s.yzoom, line);
        int reps = (s.ygrow || !zb) ? (zb ? 2 : 1) : 0;
        if (reps == 0) { sprite_line(r, s, 0, false, realx); continue; }
        SpriteReader start = r;
        for (int k = 0; k < reps; k++, yc++) {
            if (k) r = start;   // a doubled line reads the same source again
            int y = (s.flip & 2) ? s.y + realy - yc : s.y + yc;
            bool vis = y >= y0 && y < y1;
            sprite_line(r, s, y, vis, realx);
            // past the band's bottom (or its top when flipped) nothing more can be drawn
            if (!vis && ((!(s.flip & 2) && y >= y1) || ((s.flip & 2) && y < y0))) return;
        }
    }
}

void render(const Frame& f, uint32_t* rgba, int pitch, int y0, int y1) {
    for (int y = y0; y < y1; y++) {
        for (int x = 0; x < W; x++) fb[y][x] = 0x3ff;
        std::memset(pri[y], 0, W);
    }
    if (!(f.ctrl & 0x1000)) draw_bg(f, y0, y1);
    for (int i = f.nsprites - 1; i >= 0; i--) {
        if ((f.ctrl & 0x2000) && !f.sprites[i].pri) continue;
        draw_sprite(f.sprites[i], y0, y1);
    }
    if (!(f.ctrl & 0x0800)) draw_text(f, y0, y1);
    uint32_t lut[0xa00];
    for (int i = 0; i < 0xa00; i++) {
        uint16_t c = f.palette[i];
        uint32_t r = (c >> 10) & 31, g = (c >> 5) & 31, b = c & 31;
        r = (r << 3) | (r >> 2); g = (g << 3) | (g >> 2); b = (b << 3) | (b >> 2);
        lut[i] = 0xff000000u | (b << 16) | (g << 8) | r;
    }
    for (int y = y0; y < y1; y++) {
        uint32_t* o = rgba + y * pitch;
        for (int x = 0; x < W; x++) o[x] = lut[fb[y][x] < 0xa00 ? fb[y][x] : 0];
    }
}

void render(uint32_t* rgba) {
    static Frame f;
    snapshot(f);
    render(f, rgba);
}
}
