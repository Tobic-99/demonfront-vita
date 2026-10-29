// df_desktop: run the recompiled board (68000 + ARM7) on the desktop, headless, and report what it does.
//   build/cmake/df_desktop [frames]        (DF_BASE = project directory with rom/, default .)
#include "board.h"
#include "fiber.h"
#include "scheduler.h"
#include "png.h"
#include "video.h"
#include "romset.h"
#include "sound.h"
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

static std::string base() { const char* b = std::getenv("DF_BASE"); return b ? b : "."; }

static bool load(const char* name, std::vector<uint8_t>& out) { return romset::read(name, out); }
static bool load16(const char* name, std::vector<uint16_t>& out) {
    std::vector<uint8_t> b;
    if (!load(name, b)) return false;
    out.resize(b.size() / 2);
    for (size_t i = 0; i < out.size(); i++) out[i] = (uint16_t)(b[2 * i] | (b[2 * i + 1] << 8));   // LE words
    return true;
}

static int frame_no, shot_every;

// Scripted input for tests: DF_INPUT="600:coin,640:start,700-2000:right+a" (frame or frame range: buttons).
// Buttons: coin start up down left right a b c d.
struct InputEvent { int from, to; uint16_t p1, svc; };
static std::vector<InputEvent> script;
static void parse_input(const char* s) {
    std::string all(s);
    size_t pos = 0;
    while (pos < all.size()) {
        size_t end = all.find(',', pos);
        std::string e = all.substr(pos, end == std::string::npos ? std::string::npos : end - pos);
        pos = end == std::string::npos ? all.size() : end + 1;
        size_t colon = e.find(':');
        if (colon == std::string::npos) continue;
        InputEvent ev{};
        std::string range = e.substr(0, colon), btns = e.substr(colon + 1);
        size_t dash = range.find('-');
        ev.from = std::atoi(range.c_str());
        ev.to = dash == std::string::npos ? ev.from + 5 : std::atoi(range.c_str() + dash + 1);   // a tap lasts 5 frames
        size_t b = 0;
        while (b <= btns.size()) {
            size_t plus = btns.find('+', b);
            std::string n = btns.substr(b, plus == std::string::npos ? std::string::npos : plus - b);
            if (n == "coin") ev.svc |= 0x0001; else if (n == "start") ev.p1 |= 0x0001;
            else if (n == "up") ev.p1 |= 0x0002; else if (n == "down") ev.p1 |= 0x0004;
            else if (n == "left") ev.p1 |= 0x0008; else if (n == "right") ev.p1 |= 0x0010;
            else if (n == "a") ev.p1 |= 0x0020; else if (n == "b") ev.p1 |= 0x0040; else if (n == "c") ev.p1 |= 0x0080;
            else if (n == "d") ev.svc |= 0x0100;
            if (plus == std::string::npos) break;
            b = plus + 1;
        }
        script.push_back(ev);
    }
}
static void apply_input(int frame) {
    uint16_t p1 = 0, svc = 0;
    for (auto& e : script)
        if (frame >= e.from && frame < e.to) { p1 |= e.p1; svc |= e.svc; }
    board::in_p1p2 = (uint16_t)~p1;
    board::in_service = (uint16_t)~svc;
}
static bool bench;
static double render_s;
static void vblank() {
    frame_no++;
    apply_input(frame_no);
    if (bench) {
        static uint32_t px[video::W * video::H];
        auto t0 = std::chrono::steady_clock::now();
        video::render(px);
        render_s += std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    }
    if (shot_every && frame_no % shot_every == 0) {
        static uint32_t px[video::W * video::H];
        video::render(px);
        char name[64];
        std::snprintf(name, sizeof name, "/shot_%05d.png", frame_no);
        write_png(base() + "/build/shots" + name, px, video::W, video::H);
    }
}

namespace sound { extern uint64_t prof_z80_cycles; }
int main(int argc, char** argv) {
    int frames = argc > 1 ? std::atoi(argv[1]) : 600;
    shot_every = argc > 2 ? std::atoi(argv[2]) : 0;      // write build/shots/shot_NNNNN.png every N frames
    romset::init(base());
    if (!video::load_gfx() || !sound::load()) return 1;
    // DF_WAV=file: write the sound output (mono board, 16-bit) to a WAV file
    const char* wav_path = std::getenv("DF_WAV");
    std::vector<int16_t> wav;
    const bool threaded_sound = std::getenv("DF_THREADED_SOUND") != nullptr;
    sound::set_threaded(threaded_sound);
    sched::on_vblank = vblank;
    if (const char* in = std::getenv("DF_INPUT")) parse_input(in);
    if (std::getenv("DF_FREEPLAY")) board::in_dsw &= (uint16_t)~0x0008;   // DIP SW1:4 "Free" on (active low)
    bench = std::getenv("DF_BENCH") != nullptr;   // render every frame and report the time split
    sched::stats.timing = bench;
    if (bench) sched::stats.sample_pcs = false;
    auto t_start = std::chrono::steady_clock::now();
    if (!romset::arm_rom(board::arm_rom) || !load16("pgm_p02s.u20", board::bios) || !load16("v105_16m.u5", board::game))
        return 1;
    board::arm_rom.resize(0x800000, 0xff);
    board::arm_init();
    board::m68k_init();
    sched::init();
    sched::stats.sample_pcs = true;
    for (int f = 1; f <= frames; f++) {
        sched::run_frame();
        if (threaded_sound) {   // test of the Vita's threaded mode: generate one frame's worth after each frame
            static double credit;
            credit += sound::rate() / (15625.0 / 264);
            int n = (int)credit;
            credit -= n;
            static int16_t buf[2 * 1024];
            sound::generate(buf, n);
            if (wav_path) for (int i = 0; i < n; i++) wav.push_back(buf[2 * i]);
        } else {
            int16_t buf[2048];
            int n;
            while ((n = sound::pull(buf, 1024)) > 0)
                if (wav_path) for (int i = 0; i < n; i++) wav.push_back(buf[2 * i]);
        }
        if (f % 60 == 0 || f == frames) {
            auto h = sched::take_pc_hist();
            std::printf("[frame %d] irq4 %llu irq6 %llu ctrl %04x sr %04x | latch 68k->arm %04x arm->68k %08x ram_sel %d | arm yields %llu\n",
                        f, (unsigned long long)sched::stats.irq4, (unsigned long long)sched::stats.irq6, board::vregs.ctrl,
                        sched::m68k_ctx.sr_hi << 8, board::latch_to_arm, board::latch_to_68k, board::ram_sel,
                        (unsigned long long)board::arm_yields);
            std::printf("   68k seq [81dcc4] %02x, arm seq [18000447] %02x\n", m68k::rd8(0x81dcc4), arm::rd8(0x18000447));
            std::printf("   68k waits at:");
            for (size_t i = 0; i < h.size() && i < 6; i++) std::printf(" %06x(%u)", h[i].first, h[i].second);
            std::printf("\n");
        }
    }
    if (std::getenv("DF_Z80DUMP")) {   // the Z80's RAM (its program) for inspection
        FILE* z = std::fopen((base() + "/build/z80ram.bin").c_str(), "wb");
        if (z) { std::fwrite(board::z80_ram, 1, sizeof board::z80_ram, z); std::fclose(z); }
    }
    if (wav_path) {
        FILE* w = std::fopen(wav_path, "wb");
        if (w) {
            uint32_t rate = (uint32_t)sound::rate(), bytes = (uint32_t)wav.size() * 2;
            auto u32 = [&](uint32_t x) { std::fwrite(&x, 4, 1, w); };
            auto u16 = [&](uint16_t x) { std::fwrite(&x, 2, 1, w); };
            std::fwrite("RIFF", 1, 4, w); u32(36 + bytes); std::fwrite("WAVEfmt ", 1, 8, w);
            u32(16); u16(1); u16(1); u32(rate); u32(rate * 2); u16(2); u16(16);
            std::fwrite("data", 1, 4, w); u32(bytes);
            std::fwrite(wav.data(), 2, wav.size(), w);
            std::fclose(w);
            std::printf("[wav] %zu samples at %u Hz -> %s\n", wav.size(), rate, wav_path);
        }
    }
    double total = std::chrono::duration<double>(std::chrono::steady_clock::now() - t_start).count();
    std::printf("[bench] %d frames in %.2f s: %.3f ms/frame total, render %.3f ms/frame\n", frames, total,
                1000 * total / frames, 1000 * render_s / frames);
    if (bench)
        std::printf("[bench] per frame: 68000 %.3f ms, ARM %.3f ms, sound %.3f ms, syncs %.1f\n",
                    sched::stats.ns_68k / 1e6 / frames, sched::stats.ns_arm / 1e6 / frames,
                    sched::stats.ns_sound / 1e6 / frames, (double)sched::stats.syncs / frames);
    if (bench)
        std::printf("[bench] sound: z80 %.0f cycles/frame\n", (double)sound::prof_z80_cycles / frames);
}
