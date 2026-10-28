// Descent on the m9b SoC: video (D1X's arch/sdl/gr.c's interface).
//
// The game draws into its screen canvas (screen_buf, 8 bpp) and calls
// gr_update() when a frame is done; that copies it into one of three
// display buffers, cleans it from the data cache and has the
// vertical-blank interrupt show it from the next frame, with the palette
// loaded at the same time. The screen's lines are stretched to all 480
// (320 x 200: 2 or 3 each, as DOS's 320 x 200 filled a 4:3 screen), columns
// doubled (40..679 through VIDEO_HWIN) up to 360 wide, as in the Doom port.
//
// Palette: Descent's 6-bit entries (plus gr_palette_gamma) as RGB565.
// The fades step it once per vertical blank.
//
// m9b_screenshot() (0x1C on the UART, see m9b_input.c) dumps the screen
// for tools/d1.py: "\nM9BSHOT\n", 256 RGB565 palette entries
// (little-endian), then w * h pixels of the buffer on screen (w and h as
// two 16-bit words first).
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include "error.h"
#include "gr.h"
#include "gamefont.h"
#include "grdef.h"
#include "palette.h"
#include "u_mem.h"
#include "m9b_map.h"

#define VRAM          ((volatile uint32_t*)M9B_VRAM)
#define VRAM_PALETTE  (VRAM + 512)   // 256 x RGB565, two per word
#define VRAM_TABLE    (VRAM + 640)   // one entry per line
#define VIDEO_CTRL    (*(volatile uint32_t*)M9B_CTRL)
#define VIDEO_HWIN    (*(volatile uint32_t*)(M9B_CTRL + 0x18u))
#define UART_DATA     (*(volatile uint32_t*)M9B_UART)
#define UART_STATUS   (*(volatile uint32_t*)(M9B_UART + 0x4u))
#define PSRAM_BASE    M9B_PSRAM_BASE
#define LINE_BLANK    (0u << 22)
#define LINE_8BPP     (2u << 22)
#define LINE_2X       (1u << 28)

int gr_installed = 0;

static uint32_t scr_w, scr_h;
static uint8_t *screen_buf;          // the game's canvas
#define NBUF 3
static uint8_t *disp[NBUF];
static volatile int shown = -1, pending = -1;
static uint32_t pal_words[128];      // the palette in the hardware's format
static volatile uint32_t pal_dirty;  // load it at the next vertical blank
static volatile uint32_t vblanks;
static uint16_t row_of[480];         // the screen row each of the 480 lines shows

uint32_t m9b_copy_cycles;   // gr_update's copy (the profile report)
unsigned int m9b_ph[4];     // frame phases (game.c, profile builds)

static uint32_t line_8bpp(uintptr_t addr) {
    uint32_t off = (uint32_t)addr - PSRAM_BASE;
    return LINE_8BPP | ((off & 255u) / 2u) << 15 | (off >> 8);
}

static uint32_t irq_off(void) {
    uint32_t m;
    asm volatile("csrrci %0, mstatus, 8" : "=r"(m));
    return m & 8u;
}

static void irq_restore(uint32_t m) {
    if (m) asm volatile("csrsi mstatus, 8");
}

static void cache_clean(const void *p, uint32_t len) {
    for (uintptr_t a = (uintptr_t)p & ~63u; a < (uintptr_t)p + len; a += 64u)
        asm volatile("cbo.clean (%0)" :: "r"(a) : "memory");
}

// From the trap handler (../../core/runtime/trap.c) on each vertical blank.
void vblank_flip(void) {
    vblanks++;
    if (pal_dirty) {
        for (uint32_t i = 0; i < 128u; i++) VRAM_PALETTE[i] = pal_words[i];
        pal_dirty = 0;
    }
    int p = pending;
    if (p < 0) return;
    const uint8_t *buf = disp[p];
    // All 480 lines (the 43 blank lines leave about 1.3 ms before line 0
    // is fetched), each showing screen row row_of[y] (set by gr_set_mode).
    uint32_t x2 = scr_w <= 360u ? LINE_2X : 0u;
    for (uint32_t y = 0; y < 480u; y++)
        VRAM_TABLE[y] = line_8bpp((uintptr_t)&buf[row_of[y] * scr_w]) | x2;
    shown = p;
    pending = -1;
}

static void wait_vblank(void) {
    uint32_t v = vblanks;
    while (vblanks == v) {
    }
}

// 256 RGB entries (0..63 each) to the hardware, at the next vertical blank.
static void set_colors(const ubyte *rgb, int gamma, int dr, int dg, int db) {
    uint32_t words[128];
    for (int i = 0; i < 256; i++) {
        int r = rgb[3 * i] + gamma + dr, g = rgb[3 * i + 1] + gamma + dg, b = rgb[3 * i + 2] + gamma + db;
        r = r < 0 ? 0 : r > 63 ? 63 : r;
        g = g < 0 ? 0 : g > 63 ? 63 : g;
        b = b < 0 ? 0 : b > 63 ? 63 : b;
        uint32_t c = (uint32_t)(r >> 1) << 11 | (uint32_t)g << 5 | (uint32_t)(b >> 1);
        if (i & 1) words[i / 2] |= c << 16;
        else words[i / 2] = c;
    }
    uint32_t m = irq_off();
    memcpy(pal_words, words, sizeof(words));
    pal_dirty = 1;
    irq_restore(m);
}

void gr_update(void) {
    if (!screen_buf) return;
    int b = 0;
    while (b == shown || b == pending) b++;
#ifdef M9B_PROFILE
    extern uint32_t m9b_copy_cycles;
    uint32_t c0 = *(volatile uint32_t *)(M9B_TIMING + 0xCu);
#endif
    memcpy(disp[b], screen_buf, scr_w * scr_h);
    cache_clean(disp[b], scr_w * scr_h);
#ifdef M9B_PROFILE
    m9b_copy_cycles += *(volatile uint32_t *)(M9B_TIMING + 0xCu) - c0;
#endif
    uint32_t m = irq_off();
    pending = b;
    irq_restore(m);
#ifdef M9B_PROFILE
    // Every 5 s: frames, sound time, misaligned accesses emulated; every
    // 15 s the PC samples (../../core/runtime/trap.c).
    extern uint32_t sound_cycles, misaligned_traps;
    static uint32_t frames, t0, snd0, mis0, reports, cp0;
    uint32_t now = *(volatile uint32_t *)(M9B_TIMING + 0xCu);
    if (frames++ == 0) { t0 = now; snd0 = sound_cycles; mis0 = misaligned_traps; cp0 = m9b_copy_cycles; }
    else if (now - t0 >= 5u * 36000000u) {
        uint32_t dt = now - t0;
        printf("%lu.%lu fps, sound %lu%%, frame copy %lu%%, %lu misaligned\n",
               (unsigned long)((frames - 1) * 360000000ull / dt / 10), (unsigned long)((frames - 1) * 360000000ull / dt % 10),
               (unsigned long)((uint64_t)(sound_cycles - snd0) * 100 / dt),
               (unsigned long)((uint64_t)(m9b_copy_cycles - cp0) * 100 / dt), (unsigned long)(misaligned_traps - mis0));
#ifdef M9B_PERF
        {   // L1 bus counters (a PERF_COUNTERS bitstream, ../../core/rtl/soc.v)
            volatile uint32_t *perf = (volatile uint32_t *)M9B_PERF;
            static uint32_t p0[5];
            uint32_t p[5];
            for (int i = 0; i < 5; i++) p[i] = perf[i];
            printf("  data L1 bus %lu%% (%lu refills, %lu write-backs), fetch L1 bus %lu%% (%lu refills)\n",
                   (unsigned long)((uint64_t)(p[0] - p0[0]) * 100 / dt), (unsigned long)(p[2] - p0[2]), (unsigned long)(p[3] - p0[3]),
                   (unsigned long)((uint64_t)(p[1] - p0[1]) * 100 / dt), (unsigned long)(p[4] - p0[4]));
            for (int i = 0; i < 5; i++) p0[i] = p[i];
        }
#endif
        frames = 0;
        {
            extern unsigned int m9b_mask_in, m9b_mask_out, m9b_mask_sets;
            printf("  cockpit mask: %u frames, %u pixels asked, %u drawn\n", m9b_mask_sets, m9b_mask_in, m9b_mask_out);
            m9b_mask_in = m9b_mask_out = m9b_mask_sets = 0;
            extern unsigned int m9b_tmap_cycles, m9b_tmap_calls, m9b_tmap_pixels;
            printf("  tmap: %u%% of the time, %u calls, %u pixels, %u cycles/pixel\n",
                   (unsigned)((uint64_t)m9b_tmap_cycles * 100 / dt), m9b_tmap_calls, m9b_tmap_pixels,
                   m9b_tmap_pixels ? m9b_tmap_cycles / m9b_tmap_pixels : 0);
            m9b_tmap_cycles = m9b_tmap_calls = m9b_tmap_pixels = 0;
            extern unsigned int m9b_ph[4];
            printf("  render_frame %u%%, hud %u%%, cockpit+gauges %u%%, game_render_frame %u%% (the rest: game logic, gr_update)\n",
                   (unsigned)((uint64_t)m9b_ph[0] * 100 / dt), (unsigned)((uint64_t)m9b_ph[1] * 100 / dt),
                   (unsigned)((uint64_t)m9b_ph[2] * 100 / dt), (unsigned)((uint64_t)m9b_ph[3] * 100 / dt));
            m9b_ph[0] = m9b_ph[1] = m9b_ph[2] = m9b_ph[3] = 0;
            extern unsigned int m9b_draw_tmap_cycles, m9b_draw_tmap_calls;
            printf("  draw_tmap %u%%, %u polygons\n", (unsigned)((uint64_t)m9b_draw_tmap_cycles * 100 / dt), m9b_draw_tmap_calls);
            m9b_draw_tmap_cycles = m9b_draw_tmap_calls = 0;
        }
#ifdef M9B_MEMPROF
        extern void m9b_memprof_report(void);
        m9b_memprof_report();
#endif
        if (++reports % 3u == 0) {
            extern void prof_report(void);
            prof_report();
        }
    }
#endif
}

int gr_set_mode(u_int32_t mode) {
    if (mode <= 0) return 0;
    uint32_t w = SM_W(mode), h = SM_H(mode);
    if (w > 640u || h > 480u) Error("Can't set %dx%d video mode\n", (int)w, (int)h);
    if (w != scr_w || h != scr_h) {
        uint32_t m = irq_off();
        pending = -1;
        shown = -1;
        irq_restore(m);
        free(screen_buf);
        for (int i = 0; i < NBUF; i++) free(disp[i]);
        screen_buf = aligned_alloc(64, (w * h + 63u) & ~63u);
        for (int i = 0; i < NBUF; i++) disp[i] = aligned_alloc(256, (w * h + 255u) & ~255u);
        if (!screen_buf || !disp[NBUF - 1]) Error("No memory for a %dx%d screen\n", (int)w, (int)h);
        memset(screen_buf, 0, w * h);
        scr_w = w;
        scr_h = h;
        for (uint32_t y = 0; y < 480u; y++) {
            VRAM_TABLE[y] = LINE_BLANK;
            row_of[y] = (uint16_t)(y * h / 480u);
        }
        uint32_t cols = w <= 360u ? 2u * w : w;
        VIDEO_HWIN = (40u + cols) << 16 | 40u;
        VIDEO_CTRL = 1;
    }
    memset(grd_curscreen, 0, sizeof(grs_screen));
    grd_curscreen->sc_mode = mode;
    grd_curscreen->sc_w = w;
    grd_curscreen->sc_h = h;
    grd_curscreen->sc_aspect = fixdiv(grd_curscreen->sc_w * 3, grd_curscreen->sc_h * 4);
    grd_curscreen->sc_canvas.cv_bitmap.bm_x = 0;
    grd_curscreen->sc_canvas.cv_bitmap.bm_y = 0;
    grd_curscreen->sc_canvas.cv_bitmap.bm_w = w;
    grd_curscreen->sc_canvas.cv_bitmap.bm_h = h;
    grd_curscreen->sc_canvas.cv_bitmap.bm_rowsize = w;
    grd_curscreen->sc_canvas.cv_bitmap.bm_type = BM_LINEAR;
    grd_curscreen->sc_canvas.cv_bitmap.bm_data = screen_buf;
    gr_set_current_canvas(NULL);
    gamefont_choose_game_font(w, h);
    return 0;
}

int gr_check_fullscreen(void) { return 1; }
int gr_toggle_fullscreen(void) { return 1; }

int gr_init(int mode) {
    if (gr_installed) return -1;
    MALLOC(grd_curscreen, grs_screen, 1);
    memset(grd_curscreen, 0, sizeof(grs_screen));
    int r = gr_set_mode(mode);
    if (r) return r;
    grd_curscreen->sc_canvas.cv_color = 0;
    grd_curscreen->sc_canvas.cv_drawmode = 0;
    grd_curscreen->sc_canvas.cv_font = NULL;
    grd_curscreen->sc_canvas.cv_font_fg_color = 0;
    grd_curscreen->sc_canvas.cv_font_bg_color = 0;
    gr_set_current_canvas(&grd_curscreen->sc_canvas);
    gr_installed = 1;
    return 0;
}

void gr_close(void) {
    if (gr_installed) {
        gr_installed = 0;
        free(grd_curscreen);
    }
}

// -------------------------------------------------------------------------
// Palette (as arch/sdl/gr.c, with gr_palette_gamma)
// -------------------------------------------------------------------------
static int last_r, last_g, last_b;
static ubyte shown_pal[768];   // what the hardware has (0..63, before gamma), for gr_palette_read

void gr_palette_clear(void) {
    static const ubyte black[768];
    set_colors(black, 0, 0, 0, 0);
    memset(shown_pal, 0, sizeof(shown_pal));
    gr_palette_faded_out = 1;
}

void gr_palette_step_up(int r, int g, int b) {
    if (gr_palette_faded_out) return;
    if (r == last_r && g == last_g && b == last_b) return;
    last_r = r; last_g = g; last_b = b;
    set_colors(gr_palette, gr_palette_gamma, r, g, b);
}

void gr_palette_load(ubyte *pal) {
    for (int i = 0; i < 768; i++) gr_current_pal[i] = pal[i] > 63 ? 63 : pal[i];
    set_colors(gr_current_pal, gr_palette_gamma, 0, 0, 0);
    memcpy(shown_pal, gr_current_pal, sizeof(shown_pal));
    gr_palette_faded_out = 0;
    init_computed_colors();
}

int gr_palette_fade_out(ubyte *pal, int nsteps, int allow_keys) {
    (void)allow_keys;
    if (gr_palette_faded_out) return 0;
    if (!pal) pal = gr_current_pal;
    ubyte step[768];
    for (int i = 0; i < 768; i++) gr_current_pal[i] = pal[i];
    for (int j = nsteps - 1; j >= 0; j--) {
        for (int i = 0; i < 768; i++) step[i] = (ubyte)((pal[i] + gr_palette_gamma) * j / nsteps);
        set_colors(step, 0, 0, 0, 0);
        wait_vblank();
    }
    memset(shown_pal, 0, sizeof(shown_pal));
    gr_palette_faded_out = 1;
    return 0;
}

int gr_palette_fade_in(ubyte *pal, int nsteps, int allow_keys) {
    (void)allow_keys;
    if (!gr_palette_faded_out) return 0;
    ubyte step[768];
    for (int i = 0; i < 768; i++) gr_current_pal[i] = pal[i];
    for (int j = 1; j <= nsteps; j++) {
        for (int i = 0; i < 768; i++) step[i] = (ubyte)((pal[i] + gr_palette_gamma) * j / nsteps);
        set_colors(step, 0, 0, 0, 0);
        wait_vblank();
    }
    gr_palette_load(pal);
    gr_palette_faded_out = 0;
    return 0;
}

void gr_palette_read(ubyte *pal) {
    memcpy(pal, shown_pal, 768);
}

// -------------------------------------------------------------------------
// Screenshots over the UART
// -------------------------------------------------------------------------
static void uart_raw(uint8_t c) {
    while (UART_STATUS & 1u) {
    }
    UART_DATA = c;
}

void m9b_screenshot(void) {
    static const char tag[] = "\nM9BSHOT\n";
    for (const char *s = tag; *s; s++) uart_raw((uint8_t)*s);
    uart_raw((uint8_t)scr_w); uart_raw((uint8_t)(scr_w >> 8));
    uart_raw((uint8_t)scr_h); uart_raw((uint8_t)(scr_h >> 8));
    for (uint32_t i = 0; i < 128u; i++) {
        uint32_t w = pal_words[i];
        uart_raw((uint8_t)w); uart_raw((uint8_t)(w >> 8));
        uart_raw((uint8_t)(w >> 16)); uart_raw((uint8_t)(w >> 24));
    }
    const uint8_t *px = shown >= 0 ? disp[shown] : screen_buf;
    for (uint32_t i = 0; px && i < scr_w * scr_h; i++) uart_raw(px[i]);
}
