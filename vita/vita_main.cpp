// Demon Front - PS Vita platform layer: boot, ROM loading, input, frame pacing, presentation.
// The ROMs are NOT part of the VPK; they are read from ux0:data/demonfront/rom/ (see README.md).
//
// Threads:
//   emu    (core 0, priority 0x60): the scheduler with both recompiled CPUs on fibers; at every vblank it copies
//          the video state into a free Frame slot and publishes it. Paced to the PGM's 59.17 Hz.
//   render (core 1, priority 0x48): owns vitaGL; draws the newest published Frame in software into a texture
//          and presents it (vsync).
//   audio  (core 2, priority 0x40): resamples the ICS2115 output (33075 Hz) to 48 kHz and feeds sceAudioOut.
#include "board.h"
#include "fiber.h"
#include "scheduler.h"
#include "video.h"
#include "romset.h"
#include <dirent.h>
#include "sound.h"
#include <psp2/audioout.h>
#include <psp2/ctrl.h>
#include <psp2/io/stat.h>
#include <psp2/io/fcntl.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/power.h>
#include <vitaGL.h>
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

int _newlib_heap_size_user = 160 * 1024 * 1024;   // ROMs + decoded tiles ~85 MB; vitaGL takes the rest

static const std::string BASE = "ux0:data/demonfront";

[[noreturn]] static void fatal(const char* what) {
    std::printf("[vita] fatal: %s\n", what);
    sceKernelDelayThread(3 * 1000 * 1000);
    sceKernelExitProcess(1);
    for (;;) {}
}

static bool load(const std::string& name, std::vector<uint8_t>& out) { return romset::read(name, out); }
static bool load16(const std::string& name, std::vector<uint16_t>& out) {
    std::vector<uint8_t> b;
    if (!load(name, b)) return false;
    out.resize(b.size() / 2);
    std::memcpy(out.data(), b.data(), out.size() * 2);   // little-endian words, like the host
    return true;
}

// ---- frames: the emu thread fills a slot nobody is reading, the render thread draws the newest ----
static video::Frame frames[3];
static std::atomic<int> newest{-1}, drawing{-1};
static std::atomic<uint32_t> published{0};

// ---- profiling (printed every 60 presented frames) ----
static std::atomic<uint64_t> emu_us_acc{0}, emu_frames{0};

// ---- input ----
// Cross = A (shoot), Circle = B (jump), Square = C (special), Triangle = D; L = C, R = D as well; d-pad and left
// stick move; START starts (free play: the BIOS's "Free" DIP is set, no coins); SELECT (short press) pauses.
static bool aspect43;            // ux0:data/demonfront/aspect43: 4:3 like the arcade monitor; default full screen
static std::atomic<bool> paused{false};
static void poll_input() {
    SceCtrlData c{};
    sceCtrlPeekBufferPositive(0, &c, 1);
    uint32_t b = c.buttons;
    if (c.lx < 64) b |= SCE_CTRL_LEFT;
    if (c.lx > 192) b |= SCE_CTRL_RIGHT;
    if (c.ly < 64) b |= SCE_CTRL_UP;
    if (c.ly > 192) b |= SCE_CTRL_DOWN;
    uint16_t p = 0xffff;                       // P1 in the low byte, active low
    if (b & SCE_CTRL_START)    p &= ~0x0001;
    if (b & SCE_CTRL_UP)       p &= ~0x0002;
    if (b & SCE_CTRL_DOWN)     p &= ~0x0004;
    if (b & SCE_CTRL_LEFT)     p &= ~0x0008;
    if (b & SCE_CTRL_RIGHT)    p &= ~0x0010;
    if (b & SCE_CTRL_CROSS)    p &= ~0x0020;   // A: shoot (also confirms in the menus)
    if (b & SCE_CTRL_CIRCLE)   p &= ~0x0040;   // B: jump
    if (b & (SCE_CTRL_SQUARE | SCE_CTRL_LTRIGGER)) p &= ~0x0080;   // C: special
    board::in_p1p2 = p;
    uint16_t sv = 0xffff;
    if (b & (SCE_CTRL_TRIANGLE | SCE_CTRL_RTRIGGER)) sv &= ~0x0100;   // D (P1 button 4)
    board::in_service = sv;
    // SELECT: a short press on its own toggles pause
    static bool sel_down, sel_other; static uint64_t sel_t0;
    uint64_t now = sceKernelGetProcessTimeWide();
    bool sel = b & SCE_CTRL_SELECT, others = b & ~SCE_CTRL_SELECT & 0xFFFF;
    if (sel && !sel_down) { sel_down = true; sel_other = false; sel_t0 = now; }
    if (sel && others) sel_other = true;
    if (!sel && sel_down) {
        sel_down = false;
        if (!sel_other && now - sel_t0 < 700000) paused = !paused.load();
    }
}

// ---- battery RAM: the PGM's 128 KB main RAM is battery-backed (BIOS settings, high scores). It is loaded from
// ux0:data/demonfront/sram.bin at start and, when it changed, written back every 10 s and on pause by a
// low-priority thread (copy taken between frames; temp file + rename, so a crash never leaves no save). ----
static const std::string SRAM_PATH = BASE + "/sram.bin";
static uint16_t sram_copy[0x10000];
static SceUID sram_sema;
static std::atomic<bool> sram_busy{false};
static void sram_load() {
    if (FILE* f = std::fopen(SRAM_PATH.c_str(), "rb")) {
        size_t n = std::fread(board::main_ram, 1, sizeof board::main_ram, f);
        std::fclose(f);
        std::printf("[save] sram.bin loaded (%u bytes)\n", (unsigned)n);
    }
}
static void sram_request() {   // emu thread, between frames
    if (sram_busy.exchange(true)) return;
    std::memcpy(sram_copy, board::main_ram, sizeof sram_copy);
    sceKernelSignalSema(sram_sema, 1);
}
static int sram_thread(SceSize, void*) {
    uint64_t saved_hash = 0;
    for (;;) {
        sceKernelWaitSema(sram_sema, 1, nullptr);
        uint64_t h = 1469598103934665603ull;
        const uint8_t* p = (const uint8_t*)sram_copy;
        for (size_t i = 0; i < sizeof sram_copy; i++) h = (h ^ p[i]) * 1099511628211ull;
        if (h != saved_hash) {
            std::string tmp = SRAM_PATH + ".tmp";
            bool ok = false;
            if (FILE* f = std::fopen(tmp.c_str(), "wb")) {
                ok = std::fwrite(sram_copy, 1, sizeof sram_copy, f) == sizeof sram_copy;
                ok = std::fclose(f) == 0 && ok;
            }
            if (ok) { sceIoRemove(SRAM_PATH.c_str()); ok = sceIoRename(tmp.c_str(), SRAM_PATH.c_str()) >= 0; }
            if (ok) saved_hash = h;
            std::printf("[save] sram.bin %s\n", ok ? "written" : "write failed");
        }
        sram_busy = false;
    }
}

static void on_vblank() {
    int slot = 0;
    while (slot == newest.load() || slot == drawing.load()) slot++;
    video::snapshot(frames[slot]);
    newest.store(slot);
    published++;
    poll_input();
    static uint32_t frames_seen;
    if ((++frames_seen & 31) == 0) sceKernelPowerTick(SCE_KERNEL_POWER_TICK_DEFAULT);   // keep the screen on
    if (frames_seen % 592 == 0) sram_request();   // ~10 s
}

static int emu_main() {
    std::vector<uint8_t> tmp;
    romset::init(BASE);
    if (!romset::arm_rom(board::arm_rom)) fatal("v105_32m.u26 missing (Demon Front ROMs, see README)");
    if (!load16("pgm_p02s.u20", board::bios) || !load16("v105_16m.u5", board::game)) fatal("68000 ROMs missing");
    board::arm_rom.resize(0x800000, 0xff);
    if (!video::load_gfx()) fatal("graphics ROMs missing");
    if (!sound::load()) fatal("sample ROMs missing (pgm_m01s.rom, w04501.u5)");
    sound::set_threaded(true);
    std::printf("[vita] ROMs loaded\n");
    board::arm_init();
    board::m68k_init();
    sram_load();
    sched::init();
    sched::on_vblank = on_vblank;
    const uint64_t period_us = 16896;          // 264 lines of 64 us: 59.19 Hz
    uint64_t next = sceKernelGetProcessTimeWide();
    board::in_dsw &= (uint16_t)~0x0008;        // DIP SW1:4 "Free" on: free play, START alone starts a game
    for (;;) {
        if (paused) {                           // the CPUs and the sound chips stop; the last frame stays on screen
            std::printf("[vita] paused\n");
            sram_request();
            while (paused) {
                sceKernelDelayThread(16667);
                poll_input();
                sceKernelPowerTick(SCE_KERNEL_POWER_TICK_DEFAULT);
            }
            std::printf("[vita] resumed\n");
            next = sceKernelGetProcessTimeWide();   // real time went on while paused
        }
        uint64_t t0 = sceKernelGetProcessTimeWide();
        sched::run_frame();
        uint64_t t1 = sceKernelGetProcessTimeWide();
        emu_us_acc += t1 - t0; emu_frames++;
        next += period_us;
        if (t1 < next) sceKernelDelayThread((SceUInt)(next - t1));
        else if (t1 - next > 4 * period_us) next = t1;   // far behind: do not try to catch up
    }
}

// The emu thread: the CPUs' fiber stacks are carved out of this thread's own stack (the kernel stops a thread
// whose sp leaves its stack, error 0x10006).
__attribute__((noinline)) static int emu_thread(SceSize, void*) {
    uint8_t fiber_stacks[3 << 20] __attribute__((aligned(16)));
    fiber::set_pool(fiber_stacks, sizeof fiber_stacks, 1 << 20);
    return emu_main();
}

// ---- audio: the Z80 + ICS2115 run here, as fast as the output needs them (sound.cpp, threaded mode);
// 33075 Hz -> 48 kHz by linear interpolation ----
static std::atomic<uint64_t> audio_us_acc{0};
static int audio_thread(SceSize, void*) {
    constexpr int GRAIN = 512, OUT_HZ = 48000;
    int port = sceAudioOutOpenPort(SCE_AUDIO_OUT_PORT_TYPE_BGM, GRAIN, OUT_HZ, SCE_AUDIO_OUT_MODE_STEREO);
    if (port < 0) { std::printf("[audio] open port failed %08x\n", port); return 0; }
    int vol[2] = {SCE_AUDIO_VOLUME_0DB, SCE_AUDIO_VOLUME_0DB};
    sceAudioOutSetVolume(port, (SceAudioOutChannelFlag)(SCE_AUDIO_VOLUME_FLAG_L_CH | SCE_AUDIO_VOLUME_FLAG_R_CH), vol);
    static int16_t out[2][GRAIN * 2];
    static int16_t in[(GRAIN + 4) * 2];
    int have = 0;            // input frames in 'in' not consumed yet
    double pos = 0;          // read position in 'in'
    for (int b = 0;; b ^= 1) {
        uint64_t t0 = sceKernelGetProcessTimeWide();
        const double step = sound::rate() / (double)OUT_HZ;
        int need = (int)(pos + GRAIN * step) + 2;          // input frames this grain reads
        if (paused) {                           // silence while paused (the sound chips stop too)
            std::memset(out[b], 0, sizeof out[b]);
            sceAudioOutOutput(port, out[b]);
            continue;
        }
        if (need > have) { sound::generate(in + have * 2, need - have); have = need; }
        for (int i = 0; i < GRAIN; i++) {
            int k = (int)pos;
            double fr = pos - k;
            for (int ch = 0; ch < 2; ch++)
                out[b][2 * i + ch] = (int16_t)(in[2 * k + ch] + (in[2 * (k + 1) + ch] - in[2 * k + ch]) * fr);
            pos += step;
        }
        int used = (int)pos;
        std::memmove(in, in + used * 2, (have - used) * 4);
        have -= used; pos -= used;
        audio_us_acc += sceKernelGetProcessTimeWide() - t0;
        sceAudioOutOutput(port, out[b]);   // blocks until the hardware takes the grain: this paces the sound chips
    }
}

// The frame is drawn in two bands at once: the render thread (core 1) draws the top, this thread (core 0, next to
// the emulation, which needs ~1.1 ms per frame) the bottom. Disjoint lines: video::render keeps them independent.
static constexpr int BAND_SPLIT = video::H / 2;
static SceUID band_go, band_done;
static const video::Frame* band_frame;
static uint32_t* band_pixels;
static int band_thread(SceSize, void*) {
    for (;;) {
        sceKernelWaitSema(band_go, 1, nullptr);
        video::render(*band_frame, band_pixels, video::W, BAND_SPLIT, video::H);
        sceKernelSignalSema(band_done, 1);
    }
}

// Drawing and presenting overlap: the render thread (core 1, with the band thread on core 0) draws frame N into one
// of two pixel buffers while the present thread (core 2) uploads frame N-1 and waits for the display's vblank.
static uint32_t pix[2][video::W * video::H];
static SceUID buf_free, buf_ready;          // counting semaphores over the two buffers
static std::atomic<uint64_t> render_us_acc{0};
static int render_thread(SceSize, void*) {
    uint32_t seen = 0;
    int wi = 0;
    for (;;) {
        if (published.load() == seen) { sceKernelDelayThread(1000); continue; }
        seen = published.load();
        sceKernelWaitSema(buf_free, 1, nullptr);
        int slot;
        do { slot = newest.load(); drawing.store(slot); } while (newest.load() != slot);   // pin it before the emu reuses it
        uint64_t t0 = sceKernelGetProcessTimeWide();
        band_frame = &frames[slot]; band_pixels = pix[wi];
        sceKernelSignalSema(band_go, 1);
        video::render(frames[slot], pix[wi], video::W, 0, BAND_SPLIT);
        sceKernelWaitSema(band_done, 1, nullptr);
        drawing.store(-1);
        render_us_acc += sceKernelGetProcessTimeWide() - t0;
        wi ^= 1;
        sceKernelSignalSema(buf_ready, 1);
    }
}

static int present_thread(SceSize, void*) {
    vglInitExtended(0x100000, 960, 544, 16 * 1024 * 1024, SCE_GXM_MULTISAMPLE_NONE);
    vglWaitVblankStart(GL_TRUE);
    GLuint tex;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 512, 256, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glMatrixMode(GL_PROJECTION); glLoadIdentity(); glOrtho(0, 960, 544, 0, -1, 1);
    glMatrixMode(GL_MODELVIEW); glLoadIdentity();
    glClearColor(0, 0, 0, 1);
    uint32_t shown = 0;
    int ri = 0;
    uint64_t t_last = sceKernelGetProcessTimeWide();
    for (;;) {
        sceKernelWaitSema(buf_ready, 1, nullptr);
        glBindTexture(GL_TEXTURE_2D, tex);
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, video::W, video::H, GL_RGBA, GL_UNSIGNED_BYTE, pix[ri]);
        ri ^= 1;
        sceKernelSignalSema(buf_free, 1);   // the texture holds a copy: the buffer can be drawn again
        glClear(GL_COLOR_BUFFER_BIT);
        glEnable(GL_TEXTURE_2D);
        float w = aspect43 ? 725.f : 960.f, x0 = (960.f - w) / 2, x1 = x0 + w;
        float u1 = video::W / 512.f, v1 = video::H / 256.f;
        glBegin(GL_TRIANGLE_STRIP);
        glTexCoord2f(0, 0);   glVertex2f(x0, 0);
        glTexCoord2f(u1, 0);  glVertex2f(x1, 0);
        glTexCoord2f(0, v1);  glVertex2f(x0, 544);
        glTexCoord2f(u1, v1); glVertex2f(x1, 544);
        glEnd();
        vglSwapBuffers(GL_FALSE);
        if (++shown % 60 == 0) {
            uint64_t now = sceKernelGetProcessTimeWide();
            uint64_t ef = emu_frames.exchange(0), eu = emu_us_acc.exchange(0);
            double secs = (now - t_last) / 1e6;
            std::printf("[prof] fps %.1f | emu %.2f ms/frame (%llu frames) | render %.2f ms | sound %.0f%% of a core\n",
                        60 / secs, ef ? eu / 1000.0 / ef : 0.0, (unsigned long long)ef,
                        render_us_acc.exchange(0) / 1000.0 / 60, audio_us_acc.exchange(0) / 1e4 / secs);
            t_last = now;
        }
    }
}

// vitaGL compiles its fixed-function shaders with libshacccg and caches them in ux0:data/shader_cache/v31/.
// The two this game uses ship in the VPK (app0:shader_cache, compiled once with libshacccg); copying them into
// the cache before vitaGL starts means libshacccg is not needed.
static void seed_shader_cache() {
    static const char* kSub[] = {"v", "f"};
    sceIoMkdir("ux0:data/shader_cache", 0777);
    sceIoMkdir("ux0:data/shader_cache/v31", 0777);
    for (const char* sub : kSub) {
        std::string src = std::string("app0:shader_cache/") + sub, dst = std::string("ux0:data/shader_cache/v31/") + sub;
        sceIoMkdir(dst.c_str(), 0777);
        DIR* d = opendir(src.c_str());
        if (!d) continue;
        while (dirent* e = readdir(d)) {
            if (e->d_name[0] == '.') continue;
            std::string to = dst + "/" + e->d_name;
            SceIoStat st;
            if (sceIoGetstat(to.c_str(), &st) >= 0) continue;
            std::vector<uint8_t> b;
            if (FILE* in = std::fopen((src + "/" + e->d_name).c_str(), "rb")) {
                std::fseek(in, 0, SEEK_END); b.resize(std::ftell(in)); std::fseek(in, 0, SEEK_SET);
                size_t n = std::fread(b.data(), 1, b.size(), in); std::fclose(in);
                if (n != b.size()) continue;
            }
            if (FILE* out = std::fopen(to.c_str(), "wb")) { std::fwrite(b.data(), 1, b.size(), out); std::fclose(out); }
        }
        closedir(d);
    }
}

int main() {
    sceIoMkdir(BASE.c_str(), 0777);
    seed_shader_cache();
    { SceIoStat st; aspect43 = sceIoGetstat((BASE + "/aspect43").c_str(), &st) >= 0; }
    std::printf("Demon Front (native 68000 + ARM7 recompilation) - Vita build\n");
    scePowerSetArmClockFrequency(444); scePowerSetBusClockFrequency(222);
    scePowerSetGpuClockFrequency(222); scePowerSetGpuXbarClockFrequency(166);
    sceCtrlSetSamplingMode(SCE_CTRL_MODE_ANALOG);
    sram_sema = sceKernelCreateSema("sram", 0, 0, 1, nullptr);
    SceUID st = sceKernelCreateThread("sram", sram_thread, 0xA0, 64 * 1024, 0, SCE_KERNEL_CPU_MASK_USER_2, nullptr);
    if (st >= 0) sceKernelStartThread(st, 0, nullptr);
    band_go = sceKernelCreateSema("band_go", 0, 0, 1, nullptr);
    band_done = sceKernelCreateSema("band_done", 0, 0, 1, nullptr);
    SceUID bt = sceKernelCreateThread("band", band_thread, 0x70, 256 * 1024, 0, SCE_KERNEL_CPU_MASK_USER_0, nullptr);
    if (bt < 0) fatal("band thread create failed");
    sceKernelStartThread(bt, 0, nullptr);
    buf_free = sceKernelCreateSema("buf_free", 0, 2, 2, nullptr);
    buf_ready = sceKernelCreateSema("buf_ready", 0, 0, 2, nullptr);
    SceUID pt = sceKernelCreateThread("present", present_thread, 0x48, 1024 * 1024, 0, SCE_KERNEL_CPU_MASK_USER_2, nullptr);
    if (pt < 0) fatal("present thread create failed");
    sceKernelStartThread(pt, 0, nullptr);
    SceUID rt = sceKernelCreateThread("render", render_thread, 0x48, 4 * 1024 * 1024, 0, SCE_KERNEL_CPU_MASK_USER_1, nullptr);
    if (rt < 0) fatal("render thread create failed");
    sceKernelStartThread(rt, 0, nullptr);
    SceUID at = sceKernelCreateThread("audio", audio_thread, 0x40, 256 * 1024, 0, SCE_KERNEL_CPU_MASK_USER_2, nullptr);
    if (at >= 0) sceKernelStartThread(at, 0, nullptr);
    SceUID et = sceKernelCreateThread("emu", emu_thread, 0x60, 4 * 1024 * 1024, 0, SCE_KERNEL_CPU_MASK_USER_0, nullptr);
    if (et < 0) fatal("emu thread create failed");
    sceKernelStartThread(et, 0, nullptr);
    sceKernelWaitThreadEnd(et, nullptr, nullptr);
    sceKernelExitProcess(0);
    return 0;
}
