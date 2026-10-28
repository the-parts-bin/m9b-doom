// doomgeneric platform layer for the m9b SoC (see ../README.md).
//
// Video: triple buffered, as on DOS. Doom draws straight into one of three
// display buffers (I_VideoBuffer; i_video.c is built with M9B_DIRECT and
// copies nothing). At the end of a frame it is cleaned from the data cache
// and shown from the next vertical blank (vblank_flip(), from the
// interrupt): all 480 lines are 8 bpp 2x line-table entries, the 200 Doom
// lines stretched to 480 (each shown 2 or 3 times: the 4:3 of DOS's
// 320 x 200, near enough), doubled into columns 40..679 by VIDEO_HWIN. Doom then
// goes on in a buffer that isn't on screen, brought up to date by copying
// only what changed (see dirty[] below).
//
// Input: USB keyboards and mice on either of the core's two USB ports
// (usb_poll()), and keys from the UART (tools/run.py's terminal): arrows,
// Enter, Esc, space = use, f or ',' = fire, other printable characters as
// themselves. A terminal only sends key presses, so a UART key counts as
// held until 150 ms after its last repeat.
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "doomgeneric.h"
#include "doomkeys.h"
#include "i_video.h"
#include "clock.h"                // CLOCK_HZ: ../core/firmware
#include "d_event.h"
#include "m9b_map.h"

extern void trap_entry(void);     // port/trap_entry.S
extern uint32_t misaligned_traps;  // port/trap.c

#define CLOCK_CNT     (*(volatile uint32_t*)(M9B_TIMING + 0xCu))   // CLOCK_HZ (clock.h)
#define UART_DATA     (*(volatile uint32_t*)M9B_UART)
#define UART_RX_VALID (1u << 8)
#define VRAM          ((volatile uint32_t*)M9B_VRAM)
#define VRAM_PALETTE  (VRAM + 512)   // 256 x RGB565, two per word
#define VRAM_TABLE    (VRAM + 640)   // one entry per line
#define VIDEO_CTRL    (*(volatile uint32_t*)M9B_CTRL)
#define PSRAM_BASE    M9B_PSRAM_BASE

#define LINE_BLANK    (0u << 22)
#define LINE_8BPP     (2u << 22)

extern boolean palette_changed;   // i_video.c (CMAP256)

// Line-table entry for 8 bpp data at a (cached, even) PSRAM address: the
// 256-byte block and the offset into it in 2-byte units.
static uint32_t line_8bpp(uintptr_t addr) {
    uint32_t off = (uint32_t)addr - PSRAM_BASE;
    return LINE_8BPP | ((off & 255u) / 2u) << 15 | (off >> 8);
}

#define LINE_2X       (1u << 28)
#define VIDEO_HWIN    (*(volatile uint32_t*)(M9B_CTRL + 0x18u))
#define SCREEN_W      320u
#define SCREEN_H      200u

extern byte *I_VideoBuffer;   // i_video.c: allocated after DG_Init


// Display buffers: Doom draws straight into one (I_VideoBuffer), one is on
// screen, and one may be waiting for the vertical blank.
#define NBUF 3
#ifndef M9B_DISP_OFS
#define M9B_DISP_OFS 0
#endif
static uint8_t *disp[NBUF];       // 64-byte aligned
static int cur;                   // I_VideoBuffer == disp[cur]
static volatile int shown = -1;   // on screen
static volatile int pending = -1; // to be shown at the next vertical blank

void DG_Init(void) {
    for (uint32_t y = 0; y < 480u; y++) VRAM_TABLE[y] = LINE_BLANK;
    VIDEO_HWIN = (40u + 2u * SCREEN_W) << 16 | 40u;
    palette_changed = true;
    VIDEO_CTRL = 1;
}

// The flip, done by the vertical-blank interrupt (line 480; the engine
// fetches line L during line L - 2, so the 43 lines of blanking leave about
// 1.3 ms before line 0 is fetched, and the rewrite takes about 0.15 ms):
// points the 480 table entries at the new buffer (row_of[] maps each line
// to its Doom row), and loads its palette if it changed.
static uint32_t pal_words[128];
static volatile uint32_t pal_pending;
static uint8_t row_of[480];

void vblank_flip(void) {   // from trap.c
    int p = pending;
    if (p < 0) return;
    const uint8_t *buf = disp[p];
    if (!row_of[479])
        for (uint32_t y = 0; y < 480u; y++) row_of[y] = (uint8_t)(y * SCREEN_H / 480u);
    for (uint32_t y = 0; y < 480u; y++)
        VRAM_TABLE[y] = line_8bpp((uintptr_t)&buf[row_of[y] * SCREEN_W]) | LINE_2X;
    if (pal_pending) {
        for (uint32_t i = 0; i < 128u; i++) VRAM_PALETTE[i] = pal_words[i];
        pal_pending = 0;
    }
    shown = p;
    pending = -1;
}

// Keeping the buffers up to date. Doom expects its screen to hold the last
// frame (the status bar, border and so on are only redrawn when they
// change). Everything it draws is marked (V_MarkRect, and the 3D view
// window when the view was drawn); per buffer, dirty[] holds each row's
// columns x0..x1 that changed since that buffer was last drawn into, and
// switching to it copies those from the newest frame. The part inside the
// view window is copied only once D_Display has said whether it will
// redraw the whole view (M9B_FrameBegin), which it usually does.
struct span { uint16_t x0, x1; };   // x0 > x1: none
static struct span dirty[NBUF][SCREEN_H];
static struct span marks[SCREEN_H];      // this frame
static struct span deferred[SCREEN_H];   // in the view window: copied or dropped by M9B_FrameBegin
static const uint8_t *deferred_src;
static int view_drawn;
int M9B_can_defer;                       // d_main.c: set before a normal I_FinishUpdate

extern int viewwindowx, viewwindowy, scaledviewwidth, viewheight;   // r_main.c, r_draw.c
extern byte *ylookup[];                                              // r_draw.c
extern void V_RestoreBuffer(void);                                   // v_video.c

static void span_clear(struct span *s) {
    for (uint32_t y = 0; y < SCREEN_H; y++) { s[y].x0 = 0xFFFFu; s[y].x1 = 0; }
}

void M9B_MarkRect(int x, int y, int w, int h) {   // v_video.c
    int x1 = x + w - 1, y1 = y + h - 1;
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    if (x1 >= (int)SCREEN_W) x1 = SCREEN_W - 1;
    if (y1 >= (int)SCREEN_H) y1 = SCREEN_H - 1;
    for (; y <= y1 && x <= x1; y++) {
        if (x < marks[y].x0) marks[y].x0 = (uint16_t)x;
        if (x1 > marks[y].x1) marks[y].x1 = (uint16_t)x1;
    }
}

static void copy_span(uint8_t *dst, const uint8_t *src, uint32_t y, uint32_t x0, uint32_t x1) {
    if (x0 <= x1) memcpy(dst + y * SCREEN_W + x0, src + y * SCREEN_W + x0, x1 - x0 + 1u);
}

static uint32_t prof_copy;

static void resolve_deferred(int view_covers) {
    if (!deferred_src) return;
    uint32_t t0 = CLOCK_CNT;
    if (!view_covers)
        for (uint32_t y = 0; y < SCREEN_H; y++)
            copy_span(I_VideoBuffer, deferred_src, y, deferred[y].x0, deferred[y].x1);
    span_clear(deferred);
    deferred_src = NULL;
    prof_copy += CLOCK_CNT - t0;
}

void M9B_FrameBegin(int view, int view_covers) {   // d_main.c: D_Display
    view_drawn = view;
    resolve_deferred(view_covers);
}

// Points Doom at another buffer: its row pointers, and V_DrawPatch's target.
static void use_buffer(uint8_t *buf) {
    intptr_t d = buf - I_VideoBuffer;
    for (uint32_t i = 0; i < SCREEN_H; i++)
        if (ylookup[i] >= I_VideoBuffer && ylookup[i] < I_VideoBuffer + SCREEN_W * SCREEN_H)
            ylookup[i] += d;
    I_VideoBuffer = buf;
    V_RestoreBuffer();
}

#ifdef M9B_PROFILE
static uint32_t frames, frame_t0;
extern unsigned int prof_tics, prof_display;   // d_main.c
#endif
static uint32_t prof_clean;

void DG_DrawFrame(void) {
    if (!disp[0]) {
        // Placed 64 KB apart at a fixed offset from a 16 KB boundary, so all
        // three map the same way into the (direct-mapped, 16 KB) data cache.
        uint8_t *p = malloc(NBUF * 65536u + 16384u);
        uintptr_t base = (((uintptr_t)p + 16383u) & ~(uintptr_t)16383) + M9B_DISP_OFS;
        for (int i = 0; i < NBUF; i++) {
            disp[i] = (uint8_t *)(base + (uintptr_t)i * 65536u);
            for (uint32_t y = 0; y < SCREEN_H; y++) { dirty[i][y].x0 = 0; dirty[i][y].x1 = SCREEN_W - 1u; }
        }
        span_clear(marks);
        span_clear(deferred);
        memcpy(disp[0], I_VideoBuffer, SCREEN_W * SCREEN_H);   // what Doom drew so far
        use_buffer(disp[0]);
        cur = 0;
    }
    resolve_deferred(0);   // (only if no D_Display came in between)
    uint8_t *buf = disp[cur];
    uint32_t t0 = CLOCK_CNT;
    for (uintptr_t a = (uintptr_t)buf; a < (uintptr_t)buf + SCREEN_W * SCREEN_H; a += 64u)
        asm volatile("cbo.clean (%0)" :: "r"(a) : "memory");
    uint32_t t1 = CLOCK_CNT;
    prof_clean += t1 - t0;

    // Queue it (replacing a frame that wasn't shown yet), with its palette.
    uint32_t mstatus;
    asm volatile("csrrc %0, mstatus, %1" : "=r"(mstatus) : "r"(1u << 3));
    if (palette_changed) {
        palette_changed = false;
        for (uint32_t i = 0; i < 256u; i += 2u) {
            uint32_t w = 0;
            for (uint32_t k = 0; k < 2u; k++) {
                struct color c = colors[i + k];
                uint32_t rgb565 = (c.r >> 3) << 11 | (c.g >> 2) << 5 | (c.b >> 3);
                w |= rgb565 << (16u * k);
            }
            pal_words[i / 2u] = w;
        }
        pal_pending = 1;
    }
    pending = cur;
    int on_screen = shown;   // until the flip, after which it's cur
    asm volatile("csrs mstatus, %0" :: "r"(mstatus & (1u << 3)));

    // What this frame changed, for the other buffers.
    if (view_drawn) M9B_MarkRect(viewwindowx, viewwindowy, scaledviewwidth, viewheight);
#ifdef M9B_FULLCOPY
    M9B_MarkRect(0, 0, SCREEN_W, SCREEN_H);   // for checking: always copy everything
    M9B_can_defer = 0;
#endif
    view_drawn = 0;
    for (int b = 0; b < NBUF; b++) {
        if (b == cur) continue;
        for (uint32_t y = 0; y < SCREEN_H; y++) {
            if (marks[y].x0 < dirty[b][y].x0) dirty[b][y].x0 = marks[y].x0;
            if (marks[y].x1 > dirty[b][y].x1) dirty[b][y].x1 = marks[y].x1;
        }
    }
    span_clear(marks);

    // The next buffer: neither this one nor the one on screen. Bring it up
    // to date, except (after a normal frame) the view window's part.
    int next = 0;
    while (next == cur || next == on_screen) next++;
    uint8_t *nb = disp[next];
    uint32_t vx0 = (uint32_t)viewwindowx, vx1 = vx0 + (uint32_t)scaledviewwidth - 1u;
    uint32_t vy0 = (uint32_t)viewwindowy, vy1 = vy0 + (uint32_t)viewheight - 1u;
    int defer = M9B_can_defer;
    M9B_can_defer = 0;
    for (uint32_t y = 0; y < SCREEN_H; y++) {
        uint32_t x0 = dirty[next][y].x0, x1 = dirty[next][y].x1;
        if (x0 > x1) continue;
        if (defer && y >= vy0 && y <= vy1 && x1 >= vx0 && x0 <= vx1) {
            if (x0 < vx0) copy_span(nb, buf, y, x0, vx0 - 1u);   // left of the window
            if (x1 > vx1) copy_span(nb, buf, y, vx1 + 1u, x1);   // right of it
            deferred[y].x0 = (uint16_t)(x0 > vx0 ? x0 : vx0);
            deferred[y].x1 = (uint16_t)(x1 < vx1 ? x1 : vx1);
            deferred_src = buf;
        } else {
            copy_span(nb, buf, y, x0, x1);
        }
        dirty[next][y].x0 = 0xFFFFu;
        dirty[next][y].x1 = 0;
    }
    use_buffer(nb);
    cur = next;
    prof_copy += CLOCK_CNT - t1;

#ifdef M9B_TIMEDEMO
    // Timedemo checkpoints, every 250 game tics (the timedemo draws one frame
    // per tic, but a screen wipe takes a varying number of frames): the time
    // since the first frame, and a checksum of the frame, which has to match
    // between builds (M9B_FULLCOPY checks the buffer updates).
    {
        extern int gametic;   // d_loop.c
        static uint32_t last;
        static int next_tic = 250, started;
        static uint64_t cycles;
        uint32_t now = CLOCK_CNT;
        if (started) cycles += now - last;
        started = 1;
        last = now;
        if (gametic >= next_tic) {
            next_tic += 250;
            uint32_t a = 1, s = 0;
            for (uint32_t i = 0; i < SCREEN_W * SCREEN_H; i++) { a += buf[i]; s += a; }
            extern uint32_t m9b_read_bytes, m9b_read_cycles;
            printf("timedemo tic %d: %lu ms, frame %08lx; file reads so far %lu KB in %lu ms\n", gametic,
                   (unsigned long)(cycles / CLOCK_PER_MS), (unsigned long)(a ^ s << 16 ^ s >> 16),
                   (unsigned long)(m9b_read_bytes / 1024u), (unsigned long)(m9b_read_cycles / CLOCK_PER_MS));
            last = CLOCK_CNT;   // (not counting the checksum)
        }
    }
#endif
#ifdef M9B_PROFILE
    // Frame rate and where the time goes (ms per frame), every 5 s, and the
    // profile every 30 s. Only in the timedemo build: the UART output (and
    // the profile's sorting, about 0.3 s) stall the game.
    if (frames++ == 0) {
        frame_t0 = CLOCK_CNT;
        prof_tics = prof_display = prof_copy = prof_clean = 0;
    }
    uint32_t dt = CLOCK_CNT - frame_t0;
    if (dt >= 5u * CLOCK_HZ) {
        uint32_t n = frames - 1u;
        uint32_t fps10 = (uint32_t)((uint64_t)n * 10u * CLOCK_HZ / dt);
#define MS10(x) (unsigned long)((x) / n / (CLOCK_PER_MS / 10u))
        extern uint32_t m9b_read_bytes, m9b_read_cycles;   // syscalls.c (total)
        static uint32_t rb0, rc0;
        extern uint32_t sound_cycles, sound_mixes;   // sound_m9b.c (total)
        static uint32_t sc0, sm0;
        printf("%lu.%lu fps; per frame (0.1 ms): game %lu, draw %lu (of which copy %lu, clean %lu); "
               "file reads %lu KB in %lu ms; sound %lu.%lu%% (%lu halves mixed)\n",
               (unsigned long)(fps10 / 10u), (unsigned long)(fps10 % 10u),
               MS10(prof_tics), MS10(prof_display), MS10(prof_copy), MS10(prof_clean),
               (unsigned long)((m9b_read_bytes - rb0) / 1024u), (unsigned long)((m9b_read_cycles - rc0) / CLOCK_PER_MS),
               (unsigned long)((uint64_t)(sound_cycles - sc0) * 100u / dt),
               (unsigned long)((uint64_t)(sound_cycles - sc0) * 1000u / dt % 10u),
               (unsigned long)(sound_mixes - sm0));
        sc0 = sound_cycles;
        sm0 = sound_mixes;
        // The core's L1 bus counters (M9B_PERF): cycles with a data /
        // fetch line transfer open, data refills, write-backs, fetch refills.
        volatile uint32_t *perf = (volatile uint32_t *)M9B_PERF;
        static uint32_t pc0[5];
        uint32_t pc[5];
        for (int i = 0; i < 5; i++) pc[i] = perf[i];
        uint32_t dcyc = pc[0] - pc0[0], fcyc = pc[1] - pc0[1];
        uint32_t drf = pc[2] - pc0[2], dwb = pc[3] - pc0[3], frf = pc[4] - pc0[4];
        printf("  L1 waits: data %lu.%lu%% (%lu refills, %lu write-backs, %lu cycles each), "
               "fetch %lu.%lu%% (%lu refills, %lu cycles each)\n",
               (unsigned long)((uint64_t)dcyc * 100u / dt), (unsigned long)((uint64_t)dcyc * 1000u / dt % 10u),
               (unsigned long)drf, (unsigned long)dwb, (unsigned long)(drf + dwb ? dcyc / (drf + dwb) : 0),
               (unsigned long)((uint64_t)fcyc * 100u / dt), (unsigned long)((uint64_t)fcyc * 1000u / dt % 10u),
               (unsigned long)frf, (unsigned long)(frf ? fcyc / frf : 0));
        for (int i = 0; i < 5; i++) pc0[i] = pc[i];
#ifdef M9B_MISSPROF
        {
            extern unsigned int m9b_mp[8][4];
            static const char *const mp_name[7] = { "columns", "spans", "bsp+walls", "planes", "masked", "game tics", "display" };
            for (int k = 0; k < 7; k++) {
                unsigned int *m = m9b_mp[k];
                if (k >= 2) printf("  %s: %u.%u%% of the time, %u refills, %u write-backs, bus %u.%u%% of the time\n", mp_name[k],
                       (unsigned int)((uint64_t)m[3] * 100u / dt), (unsigned int)((uint64_t)m[3] * 1000u / dt % 10u), m[0], m[1],
                       (unsigned int)((uint64_t)m[2] * 100u / dt), (unsigned int)((uint64_t)m[2] * 1000u / dt % 10u));
                else printf("  %s: %u px, %u refills, %u write-backs (per 100 px: %u.%u, %u.%u), bus %u.%u%% of the time\n",
                       mp_name[k], m[3], m[0], m[1],
                       m[3] ? m[0] * 100u / m[3] : 0u, m[3] ? m[0] * 1000u / m[3] % 10u : 0u,
                       m[3] ? m[1] * 100u / m[3] : 0u, m[3] ? m[1] * 1000u / m[3] % 10u : 0u,
                       (unsigned int)((uint64_t)m[2] * 100u / dt), (unsigned int)((uint64_t)m[2] * 1000u / dt % 10u));
                m[0] = m[1] = m[2] = m[3] = 0;
            }
        }
#endif
        rb0 = m9b_read_bytes;
        rc0 = m9b_read_cycles;
        frames = 0;
        static uint32_t reports;
        if (++reports % 6u == 0) {   // every 30 s
            extern void prof_report(void);
            prof_report();
        }
    }
#endif
}

void DG_SleepMs(uint32_t ms) {
    uint32_t t0 = CLOCK_CNT;
    while (CLOCK_CNT - t0 < ms * CLOCK_PER_MS) {
    }
}

uint32_t DG_GetTicksMs(void) {
    // 32-bit counter at 36 MHz wraps every 119 s: extend it.
    static uint32_t last, ms, frac;
    uint32_t now = CLOCK_CNT;
    frac += now - last;
    last = now;
    ms += frac / CLOCK_PER_MS;
    frac %= CLOCK_PER_MS;
    return ms;
}

// -------------------------------------------------------------------------
// Keys from the UART.
// -------------------------------------------------------------------------
#define HOLD_MS 150u

static struct { unsigned char key; uint32_t until; } held[8];
static struct { unsigned char pressed, key; } queue[32];
static uint32_t q_head, q_tail;
static uint32_t esc_state, esc_time;   // 0, 1 after ESC, 2 after ESC [

static void push_event(int pressed, unsigned char key) {
    if (q_tail - q_head < 32u) {
        queue[q_tail % 32u].pressed = (unsigned char)pressed;
        queue[q_tail % 32u].key = key;
        q_tail++;
    }
}

static void key_hit(unsigned char key) {
    uint32_t now = DG_GetTicksMs();
    for (int i = 0; i < 8; i++)
        if (held[i].key == key) { held[i].until = now + HOLD_MS; return; }   // repeat
    for (int i = 0; i < 8; i++)
        if (!held[i].key) {
            held[i].key = key;
            held[i].until = now + HOLD_MS;
            push_event(1, key);
            return;
        }
}

static void poll_uart(void) {
    uint32_t now = DG_GetTicksMs();
    if (esc_state && now - esc_time > 30u) {   // a lone ESC
        esc_state = 0;
        key_hit(KEY_ESCAPE);
    }
    for (;;) {
        uint32_t d = UART_DATA;
        if (!(d & UART_RX_VALID)) break;
        unsigned char c = (unsigned char)d;
        if (esc_state == 1) {
            esc_state = c == '[' ? 2u : 0u;
            if (!esc_state) key_hit(KEY_ESCAPE);
            continue;
        }
        if (esc_state == 2) {
            esc_state = 0;
            if (c == 'A') key_hit(KEY_UPARROW);
            else if (c == 'B') key_hit(KEY_DOWNARROW);
            else if (c == 'C') key_hit(KEY_RIGHTARROW);
            else if (c == 'D') key_hit(KEY_LEFTARROW);
            continue;
        }
        if (c == 27) { esc_state = 1; esc_time = now; }
        else if (c == '\r' || c == '\n') key_hit(KEY_ENTER);
        else if (c == ' ') key_hit(KEY_USE);
        else if (c == 'f' || c == ',') key_hit(KEY_FIRE);
        else if (c >= 32 && c < 127) key_hit(c);
    }
    for (int i = 0; i < 8; i++)
        if (held[i].key && (int32_t)(now - held[i].until) >= 0) {
            push_event(0, held[i].key);
            held[i].key = 0;
        }
}

// -------------------------------------------------------------------------
// USB keyboards and mice (../core/rtl/usb_port.v), polled.
//
// A keyboard's report lists the keys held (HID usage codes, up to four here)
// and the modifiers; comparing it with the previous one gives the presses
// and releases. Ctrl fires, Alt strafes, Shift runs and space opens doors,
// as in vanilla Doom. A mouse's motion comes from MOUSE_POS, the hardware's
// running sums of every report, so none is lost between Doom's tics.
// -------------------------------------------------------------------------
#define USB_REG(p, o)    (*(volatile uint32_t*)(M9B_USB + 0x20u * (p) + (o)))
#define USB_STATUS(p)    USB_REG(p, 0x00u)   // bits 1:0 type, 15:8 report count
#define USB_KEYS(p)      USB_REG(p, 0x04u)   // {k3, k2, k1, modifiers}
#define USB_KEYS2(p)     USB_REG(p, 0x08u)   // k4
#define USB_MOUSE(p)     USB_REG(p, 0x0Cu)   // {dy, dx, buttons}
#define USB_MOUSE_POS(p) USB_REG(p, 0x14u)   // {y, x}, 16-bit running sums

// HID usage codes 0x04..0x63 (keyboard page) to Doom keys.
static const unsigned char hid_to_doom[0x64] = {
    [0x04] = 'a', 'b', 'c', 'd', 'e', 'f', 'g', 'h', 'i', 'j', 'k', 'l', 'm',
    'n', 'o', 'p', 'q', 'r', 's', 't', 'u', 'v', 'w', 'x', 'y', 'z',
    [0x1E] = '1', '2', '3', '4', '5', '6', '7', '8', '9', '0',
    [0x28] = KEY_ENTER, KEY_ESCAPE, KEY_BACKSPACE, KEY_TAB, KEY_USE,
    [0x2D] = '-', '=', '[', ']', '\\', '\\', ';', '\'', '`', ',', '.', '/',
    [0x39] = KEY_CAPSLOCK, KEY_F1, KEY_F2, KEY_F3, KEY_F4, KEY_F5, KEY_F6,
    KEY_F7, KEY_F8, KEY_F9, KEY_F10, KEY_F11, KEY_F12,
    [0x46] = KEY_PRTSCR, KEY_SCRLCK, KEY_PAUSE, KEY_INS, KEY_HOME, KEY_PGUP,
    KEY_DEL, KEY_END, KEY_PGDN, KEY_RIGHTARROW, KEY_LEFTARROW, KEY_DOWNARROW,
    KEY_UPARROW, KEY_NUMLOCK,
    [0x54] = KEYP_DIVIDE, KEYP_MULTIPLY, KEYP_MINUS, KEYP_PLUS, KEYP_ENTER,
    KEYP_1, KEYP_2, KEYP_3, KEYP_4, KEYP_5, KEYP_6, KEYP_7, KEYP_8, KEYP_9,
    KEY_INS, KEY_DEL,
};

// Modifier bits (left | right) and their keys.
static const struct { uint8_t mask; unsigned char key; } hid_mods[3] = {
    {0x11u, KEY_FIRE}, {0x22u, KEY_RSHIFT}, {0x44u, KEY_RALT}};

static struct {
    uint32_t type, count;
    uint8_t keys[4], mods;           // keyboard: the last report
    uint32_t pos;                    // mouse: MOUSE_POS at the last poll
    uint8_t buttons;
} usb[2];

static int key_in(unsigned char k, const uint8_t *keys) {
    for (int i = 0; i < 4; i++)
        if (keys[i] == k) return 1;
    return 0;
}

// Posts the differences between two keyboard states as key events.
static void usb_keys_changed(const uint8_t *old_keys, uint8_t old_mods, const uint8_t *keys, uint8_t mods) {
    for (int i = 0; i < 3; i++) {
        int was = (old_mods & hid_mods[i].mask) != 0, is = (mods & hid_mods[i].mask) != 0;
        if (was != is) push_event(is, hid_mods[i].key);
    }
    for (int i = 0; i < 4; i++) {
        unsigned char k = old_keys[i];
        if (k < 0x64u && hid_to_doom[k] && !key_in(k, keys)) push_event(0, hid_to_doom[k]);
    }
    for (int i = 0; i < 4; i++) {
        unsigned char k = keys[i];
        if (k < 0x64u && hid_to_doom[k] && !key_in(k, old_keys)) push_event(1, hid_to_doom[k]);
    }
}

static void usb_poll(void) {
    static const uint8_t none[4];
    for (uint32_t p = 0; p < 2u; p++) {
        uint32_t st = USB_STATUS(p), type = st & 3u, count = st >> 8 & 0xFFu;
        if (type != usb[p].type) {   // unplugged or replaced: release everything
            usb_keys_changed(usb[p].keys, usb[p].mods, none, 0);
            memset(usb[p].keys, 0, 4);
            usb[p].mods = 0;
            usb[p].buttons = 0;
            usb[p].pos = USB_MOUSE_POS(p);
            usb[p].type = type;
            usb[p].count = count;
            continue;
        }
        if (type != 1u || count == usb[p].count) continue;
        usb[p].count = count;
        uint32_t k = USB_KEYS(p);
        uint8_t keys[4] = {(uint8_t)(k >> 8), (uint8_t)(k >> 16), (uint8_t)(k >> 24), (uint8_t)USB_KEYS2(p)};
        if (keys[0] == 0x01u) continue;   // ErrorRollOver: too many keys, the report means nothing
        usb_keys_changed(usb[p].keys, usb[p].mods, keys, (uint8_t)k);
        memcpy(usb[p].keys, keys, 4);
        usb[p].mods = (uint8_t)k;
    }
}

// Chocolate Doom's default mouse acceleration: 2x beyond 10 counts per tic.
static int accelerate(int v) {
    if (v > 10) return (v - 10) * 2 + 10;
    if (v < -10) return (v + 10) * 2 - 10;
    return v;
}

// Called once per I_GetEvent (i_input.c): one ev_mouse with the motion
// since the last call, if anything changed.
void M9B_PollMouse(void) {
    int dx = 0, dy = 0, buttons = 0, any = 0;
    for (uint32_t p = 0; p < 2u; p++) {
        if (usb[p].type != 2u) continue;
        uint32_t pos = USB_MOUSE_POS(p);
        uint8_t b = (uint8_t)(USB_MOUSE(p) & 7u);
        dx += (int16_t)(pos - usb[p].pos);
        dy += (int16_t)((pos >> 16) - (usb[p].pos >> 16));
        any |= pos != usb[p].pos || b != usb[p].buttons;
        usb[p].pos = pos;
        usb[p].buttons = b;
        buttons |= b;
    }
    if (!any) return;
    event_t ev;
    ev.type = ev_mouse;
    ev.data1 = buttons;                // bit 0 left (fire), 1 right (strafe), 2 middle (forward)
    ev.data2 = accelerate(dx);
    ev.data3 = -accelerate(dy);        // HID: down is positive
    ev.data4 = 0;
    D_PostEvent(&ev);
}

int DG_GetKey(int *pressed, unsigned char *key) {
    poll_uart();
    usb_poll();
    if (q_head == q_tail) return 0;
    *pressed = queue[q_head % 32u].pressed;
    *key = queue[q_head % 32u].key;
    q_head++;
    return 1;
}

void DG_SetWindowTitle(const char *title) {
    (void)title;
}

int main(int argc, char **argv) {
    (void)argc;
    (void)argv;
    asm volatile("csrw mtvec, %0" :: "r"(&trap_entry));
#ifdef M9B_PROFILE
    extern void membench(void);
    membench();
#endif
#ifdef M9B_SDTEST
    extern void sdtest(void);
    sdtest();
#endif
    // Vertical-blank interrupts (trap.c): the page flip (and the profiler).
    *(volatile uint32_t*)(M9B_CTRL + 0x4u) = 0xFu;     // IRQ_PENDING: clear
    *(volatile uint32_t*)(M9B_CTRL + 0x8u) = 1u;       // IRQ_ENABLE: vblank
    asm volatile("csrs mie, %0" :: "r"(1u << 11));
    asm volatile("csrs mstatus, %0" :: "r"(1u << 3));
#ifdef M9B_TIMEDEMO
    static char *args[] = {"doom", "-iwad", "DOOMS/DOOM1.WAD", "-mb", "4", "-timedemo", "demo1", NULL};
    doomgeneric_Create(7, args);
#else
    static char *args[] = {"doom", "-iwad", "DOOMS/DOOM1.WAD", "-mb", "4", NULL};
    doomgeneric_Create(5, args);
#endif
    for (;;) doomgeneric_Tick();
}
