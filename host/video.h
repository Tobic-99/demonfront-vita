// IGS023 video (our implementation; hardware behaviour as documented by MAME's igs023_video.cpp): background
// tilemap (32x32, 5 bpp, per-line scroll), text tilemap (8x8, 4 bpp), zoomable sprites from the A/B ROMs.
// Rendered in software into a 448x224 RGBA frame, on any thread and in bands (the Vita draws two bands at once).
#pragma once
#include <cstdint>
#include <string>

namespace video {
constexpr int W = 448, H = 224;

struct Sprite {
    int x, y, w, h, color, flip, pri;
    uint32_t offs;
    uint32_t xzoom, yzoom;
    bool xgrow, ygrow;
};
// Everything a frame needs, copied at vblank so another thread (core) can draw it while the CPUs go on.
struct Frame {
    uint16_t video_ram[0x4000];
    uint16_t palette[0xa00];
    uint16_t bg_yscroll, bg_xscroll, tx_yscroll, tx_xscroll, ctrl;
    int nsprites;
    Sprite sprites[256];
};

bool load_gfx();                          // graphics ROMs through romset
void sprite_dma();                        // line 220: main RAM 0x800000 -> sprite buffer (if enabled in ctrl bit 0)
void latch_sprites();                     // vblank: decode the sprite buffer into the list drawn next frame
void snapshot(Frame& f);                  // vblank: copy the current video state
// draw lines y0..y1-1 of the W x H frame as 0xAABBGGRR; disjoint bands may run on different threads at once
void render(const Frame& f, uint32_t* rgba, int pitch = W, int y0 = 0, int y1 = H);
void render(uint32_t* rgba);              // snapshot + render (desktop convenience)
}
