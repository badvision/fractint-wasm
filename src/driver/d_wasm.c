/*
 * d_wasm.c - SDL2/Emscripten WASM driver for xfractint
 *
 * Replaces unix/unixscr.c (X11 graphics) and unix/xfcurses.c (X11 text window).
 * Provides the flat-function interface that fractint's video.c and common code
 * call through function pointers (dotwrite/dotread/linewrite/lineread).
 *
 * Phase 1: Real pixel buffer + palette LUT, emscripten_set_main_loop integration.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <stdint.h>

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#else
#define EMSCRIPTEN_KEEPALIVE
#endif

/* SDL2 via Emscripten -s USE_SDL=2 */
#include <SDL2/SDL.h>

#include "port.h"
/* Include our WASM-safe xfcurses.h before prototyp.h to define WINDOW type */
#include "xfcurses.h"
#include "prototyp.h"

/* fpe_handler is defined in unix/general.c */
extern void fpe_handler(int signum);

/* ------------------------------------------------------------------ */
/* Globals expected by video.c and common code                        */
/* ------------------------------------------------------------------ */

/* --- Globals owned by unix/video.c — do NOT redefine here ---
 * fake_lut, istruecolor, daclearn, dacnorm, ShadowColors,
 * goodmode, dotwrite, dotread, linewrite, lineread, andcolor, diskflag,
 * videoflag, swapsetup, color_dark, color_bright, color_medium, reallyega,
 * gotrealdac, rowcount, video_type, svga_type, mode7text, textaddr,
 * textsafe, text_type, textrow, textcol, textrbase, textcbase, vesa_detect,
 * video_scroll, video_startx, video_starty, vesa_xres, vesa_yres,
 * chkd_vvs, video_vram, videotable[]
 * --- discardgraphics() is defined in common/realdos.c ---
 */

/* Globals from unixscr.c replacement */
int virtual_  = 0;

/* xfcurses.c globals (X11 window handles — null in WASM) */
int COLS  = 80;
int LINES = 25;
int ctrl_window = 0;
int slowdisplay = 0;
int resize_flag = 1;
int unixDisk    = 0;

/* videotable[] is defined (and initialized) in unix/video.c */

/* ------------------------------------------------------------------ */
/* Text mode buffer — 25 x 80 cells — declared here, used throughout */
/* ------------------------------------------------------------------ */
#define TEXT_ROWS 25
#define TEXT_COLS 80
/* Each cell: low byte = char, high byte = CGA attribute (fg|bg<<4). */
/* Default CGA attribute: white on black */
#define TEXT_ATTR_DEFAULT 0x07
static uint16_t text_buf[TEXT_ROWS * TEXT_COLS];
static int text_mode_active = 0;
static int text_buf_dirty   = 0;

/* Screen pixel buffer — 8-bit palette indices, dynamically allocated */
static BYTE *screen_pixels = NULL;
static int screen_w = 0;
static int screen_h = 0;
static int pixel_buf_size = 0;

/*
 * rgba_lut[i] = RGBA for palette index i.
 * Stored as little-endian Uint32 = 0xAABBGGRR so that a Uint32Array
 * view of ImageData.data paints the correct colour.
 * Built by writevideopalette() from dacbox[256][3] (6-bit VGA values).
 */
static Uint32 rgba_lut[256];

/*
 * Dirty flags for the JS render loop.
 *   frame_dirty   — set when any pixel in screen_pixels changes
 *   palette_dirty — set when rgba_lut is rebuilt (writevideopalette)
 *
 * wasm_consume_dirty() returns a bitmask and clears both flags:
 *   bit 0 (value 1) = pixels changed  → must re-expand palette + putImageData
 *   bit 1 (value 2) = palette changed → must re-expand palette + putImageData
 * A return value of 0 means nothing changed; JS can skip the render work.
 *
 * During color cycling spindac() calls writevideopalette() (palette_dirty=1)
 * but does NOT call writevideo/writevideoline (frame_dirty stays 0).
 * Both flags trigger the same JS action (palette expand + blit), so the
 * JS render loop checks dirty != 0 rather than inspecting individual bits.
 */
static int frame_dirty   = 0;
static int palette_dirty = 0;

/* dacbox is defined in unix/general.c */
extern unsigned char dacbox[256][3];

/* ------------------------------------------------------------------ */
/* Interrupt flag: set by zoom/resize/fractype to abort calcfract()   */
/* ------------------------------------------------------------------ */

/*
 * restart_requested — set to 1 before changing coordinates or fractype
 * while a calculation may be in progress.  xgetkey() returns ESC when
 * this flag is set, which propagates through keypressed() and causes
 * calcfract() to abort cleanly with calc_status=3 (interrupted,
 * non-resumable).  The flag is cleared in WS_INIT_VIDEO before the
 * new calculation starts.
 *
 * keybuffer is defined in unix/general.c; we need to clear it too so
 * the injected ESC doesn't linger and abort the next calculation.
 */
static volatile int restart_requested = 0;
extern int keybuffer;

/* ------------------------------------------------------------------ */
/* Pending coordinate restore (for URL hash loading)                  */
/* calcfracinit() resets coords to fractal defaults, so we apply the  */
/* URL-supplied coords *after* calcfracinit() in WS_INIT_VIDEO.       */
/* ------------------------------------------------------------------ */
static int    pending_coords = 0;
static double pending_xxmin, pending_xxmax, pending_yymin, pending_yymax;

/* ------------------------------------------------------------------ */
/* Key ring buffer (power-of-two size for fast masking)               */
/* ------------------------------------------------------------------ */

#define KEY_BUF_SIZE 64
static int key_ring[KEY_BUF_SIZE];
static int key_head = 0;
static int key_tail = 0;

static void key_push(int keycode)
{
    int next = (key_head + 1) & (KEY_BUF_SIZE - 1);
    if (next != key_tail) {  /* drop if full */
        key_ring[key_head] = keycode;
        key_head = next;
    }
}

static int key_pop(void)
{
    if (key_tail == key_head) return 0;
    int k = key_ring[key_tail];
    key_tail = (key_tail + 1) & (KEY_BUF_SIZE - 1);
    return k;
}

/* ------------------------------------------------------------------ */
/* Internal pixel buffer helpers                                       */
/* ------------------------------------------------------------------ */

static void ensure_screen_buffer(void)
{
    int needed = screen_w * screen_h;
    if (needed <= 0) return;
    if (screen_pixels == NULL || needed != pixel_buf_size) {
        free(screen_pixels);
        screen_pixels = (BYTE *)calloc((size_t)needed, 1);
        pixel_buf_size = screen_pixels ? needed : 0;
    }
}

/* ------------------------------------------------------------------ */
/* unixscr.c replacement — init / shutdown                            */
/* ------------------------------------------------------------------ */

int unixarg(int argc, char **argv, int *i)
{
    return 0;   /* no X11-specific args in WASM */
}

void UnixInit(void)
{
    signal(SIGFPE, fpe_handler);
}

void initUnixWindow(void)
{
    screen_w = sxdots > 0 ? sxdots : 800;
    screen_h = sydots > 0 ? sydots : 600;
    sxdots = screen_w;
    sydots = screen_h;

    gotrealdac  = 1;
    fake_lut    = 0;
    istruecolor = 0;
    colors      = 256;

    /* Default VGA-style palette: 6-bit components (0-63) */
    {
        int i;
        for (i = 0; i < 256; i++) {
            dacbox[i][0] = (unsigned char)((i >> 5) * 8 + 7);
            dacbox[i][1] = (unsigned char)((((i + 16) & 28) >> 2) * 8 + 7);
            dacbox[i][2] = (unsigned char)(((i + 2) & 3) * 16 + 15);
        }
        dacbox[0][0] = dacbox[0][1] = dacbox[0][2] = 0;
        dacbox[1][0] = dacbox[1][1] = dacbox[1][2] = 63;
    }

    videotable[0].xdots   = sxdots;
    videotable[0].ydots   = sydots;
    videotable[0].colors  = colors;
    videotable[0].dotmode = 19;

    ensure_screen_buffer();
    writevideopalette();
}

void UnixDone(void)
{
    if (screen_pixels) {
        free(screen_pixels);
        screen_pixels = NULL;
    }
}

/* ------------------------------------------------------------------ */
/* unixscr.c replacement — video                                      */
/* ------------------------------------------------------------------ */

int startvideo(void)
{
    ensure_screen_buffer();
    if (screen_pixels) {
        memset(screen_pixels, 0, (size_t)(screen_w * screen_h));
    }
    /* Entering graphics mode: hide text overlay */
    text_mode_active = 0;
    text_buf_dirty   = 1;
    return 0;
}

int endvideo(void)
{
    return 0;
}

int resizeWindow(void)
{
    return 0;
}

void writevideo(int x, int y, int color)
{
    if (!screen_pixels) return;
    if (x < 0 || x >= screen_w || y < 0 || y >= screen_h) return;
    screen_pixels[y * screen_w + x] = (BYTE)(color & 0xFF);
    frame_dirty = 1;
}

int readvideo(int x, int y)
{
    if (!screen_pixels) return 0;
    if (x < 0 || x >= screen_w || y < 0 || y >= screen_h) return 0;
    return (int)screen_pixels[y * screen_w + x];
}

void writevideoline(int y, int x, int lastx, BYTE *pixels)
{
    int width, i;
    if (!screen_pixels) return;
    if (y < 0 || y >= screen_h) return;
    width = lastx - x + 1;
    for (i = 0; i < width; i++) {
        int px = x + i;
        if (px >= 0 && px < screen_w) {
            screen_pixels[y * screen_w + px] = pixels[i];
        }
    }
    frame_dirty = 1;
}

void readvideoline(int y, int x, int lastx, BYTE *pixels)
{
    int width, i;
    if (!screen_pixels) {
        memset(pixels, 0, (size_t)(lastx - x + 1));
        return;
    }
    width = lastx - x + 1;
    for (i = 0; i < width; i++) {
        int px = x + i;
        if (px >= 0 && px < screen_w && y >= 0 && y < screen_h) {
            pixels[i] = screen_pixels[y * screen_w + px];
        } else {
            pixels[i] = 0;
        }
    }
}

int readvideopalette(void)
{
    /* dacbox already holds the palette; nothing to read from hardware */
    return 0;
}

/*
 * writevideopalette — rebuild rgba_lut from dacbox.
 *
 * dacbox values are 6-bit (0-63); left-shift by 2 to get 8-bit (0-252).
 * Canvas ImageData is Uint8ClampedArray RGBA.  Viewed as a Uint32Array
 * (little-endian), each pixel is 0xAABBGGRR.
 */
int writevideopalette(void)
{
    int i;
    for (i = 0; i < 256; i++) {
        Uint32 r = (Uint32)(dacbox[i][0]) << 2;
        Uint32 g = (Uint32)(dacbox[i][1]) << 2;
        Uint32 b = (Uint32)(dacbox[i][2]) << 2;
        rgba_lut[i] = 0xFF000000u | (b << 16) | (g << 8) | r;
    }
    palette_dirty = 1;
    return 0;
}

void setlinemode(int mode)
{
    /* XOR mode not implemented */
}

void drawline(int x1, int y1, int x2, int y2)
{
    /* Bresenham's line algorithm using XOR on palette index */
    int dx = abs(x2 - x1);
    int dy = abs(y2 - y1);
    int sx = x1 < x2 ? 1 : -1;
    int sy = y1 < y2 ? 1 : -1;
    int err = dx - dy;
    while (1) {
        if (screen_pixels && x1 >= 0 && x1 < screen_w && y1 >= 0 && y1 < screen_h) {
            screen_pixels[y1 * screen_w + x1] ^= 0x0F;
        }
        if (x1 == x2 && y1 == y2) break;
        {
            int e2 = 2 * err;
            if (e2 > -dy) { err -= dy; x1 += sx; }
            if (e2 <  dx) { err += dx; y1 += sy; }
        }
    }
}

void xsync(void)
{
    /* no-op in WASM */
}

void schedulealarm(int soon)
{
    /* no-op: WASM uses requestAnimationFrame */
}

void redrawscreen(void)
{
    /* JS reads pixel_buf + rgba_lut directly via wasm_get_* exports */
}

/* ------------------------------------------------------------------ */
/* Font — return embedded 8x8 bitmap table                            */
/* ------------------------------------------------------------------ */

static unsigned char wasm_font[128 * 8] = { 0 };
static int font_initialized = 0;

unsigned char *xgetfont(void)
{
    if (!font_initialized) {
        font_initialized = 1;
    }
    return wasm_font;
}

/* ------------------------------------------------------------------ */
/* Keyboard                                                            */
/* ------------------------------------------------------------------ */

/*
 * xgetkey — called by fractint's getkeyint().
 * Always non-blocking in WASM (block parameter ignored).
 * Returns the next queued keycode, or 0.
 *
 * When restart_requested is set, returns ESC (27) to interrupt any
 * in-progress calcfract() call cleanly.  The flag is only checked here,
 * not consumed — WS_INIT_VIDEO clears it (along with keybuffer) before
 * starting the new calculation.
 */
int xgetkey(int block)
{
    (void)block;
    if (restart_requested) {
        return 27; /* ESC — causes calcfract() to abort cleanly */
    }
    return key_pop();
}

/*
 * wasm_push_key — JS calls this to inject key events.
 * keycode follows Fractint/DOS conventions: ASCII for printable chars,
 * 1000-series for function/arrow keys (see src/js/keyboard.js).
 */
EMSCRIPTEN_KEEPALIVE
void wasm_push_key(int keycode)
{
    key_push(keycode);
}

/* ------------------------------------------------------------------ */
/* Exported pixel/palette accessors for JS rendering                  */
/* ------------------------------------------------------------------ */

/*
 * wasm_get_pixel_buf — pointer to screen_w * screen_h palette-index bytes.
 */
EMSCRIPTEN_KEEPALIVE
BYTE *wasm_get_pixel_buf(void)
{
    return screen_pixels;
}

/*
 * wasm_get_rgba_lut — pointer to rgba_lut[256] (little-endian Uint32 0xAABBGGRR).
 */
EMSCRIPTEN_KEEPALIVE
Uint32 *wasm_get_rgba_lut(void)
{
    return rgba_lut;
}

/*
 * wasm_get_screen_dims — width in low 16 bits, height in high 16 bits.
 * JS: var w = dims & 0xFFFF, h = (dims >>> 16) & 0xFFFF;
 */
EMSCRIPTEN_KEEPALIVE
int wasm_get_screen_dims(void)
{
    return (screen_h << 16) | screen_w;
}

/*
 * wasm_consume_dirty — return dirty bitmask and atomically clear both flags.
 *   bit 0 (1) = pixel buffer changed (writevideo / writevideoline called)
 *   bit 1 (2) = palette LUT changed  (writevideopalette called)
 * Returns 0 if nothing has changed since the last call — JS render loop
 * can skip the palette-expand + putImageData work entirely.
 */
EMSCRIPTEN_KEEPALIVE
int wasm_consume_dirty(void)
{
    int d = (frame_dirty ? 1 : 0) | (palette_dirty ? 2 : 0);
    frame_dirty   = 0;
    palette_dirty = 0;
    return d;
}

/* ------------------------------------------------------------------ */
/* WASM main loop                                                      */
/* ------------------------------------------------------------------ */

/*
 * wasm_state drives the render pipeline without blocking the browser
 * event loop.
 *
 * WS_INIT_VIDEO   — first callback: set video mode, init fractal params
 * WS_CALCFRAC     — subsequent callbacks: run calcfrac() chunk-by-chunk
 * WS_DONE         — fractal finished; idle, processes navigation keys
 * WS_COLOR_CYCLE  — one spindac() step per frame; stops on any keypress
 */
typedef enum {
    WS_INIT_VIDEO = 0,
    WS_CALCFRAC,
    WS_DONE,
    WS_COLOR_CYCLE,
    WS_PAN_CALC   /* calculating only the newly exposed strip after a pan */
} WasmState;

static WasmState wasm_state = WS_INIT_VIDEO;

/* Watchdog: force restart if WS_CALCFRAC is stuck for too many frames */
static int calcfrac_frame_count = 0;
static const int calcfrac_watchdog = 3000; /* ~50 s at 60 fps */

/* Pan strip state: direction used only to determine which strip was exposed */
static int pan_strip_dir  = 0; /* 1=right 2=left 3=down 4=up, 0=none */

/* Direction for color cycling: +1 = forward, -1 = reverse */
static int cycle_dir = 1;

/* Frame-skip counters for cycling speed control.
 * delay() is a no-op in WASM, so we skip frames instead.
 * cycle_skip_max=1 means every frame (fastest); higher = slower. */
static int cycle_skip_counter = 0;
static int cycle_skip_max     = 1;

/* Fractint internals used by the loop */
extern void calcfracinit(void);
extern int  calcfract(void);
extern void setvideomode(int, int, int, int);
extern void spindac(int dir, int inc);

extern struct videoinfo videoentry;
extern struct videoinfo videotable[];
extern int adapter;
extern int dotmode, xdots, ydots, sxdots, sydots;
extern int sxoffs, syoffs, colors, textsafe2;
extern int diskvideo, diskisactive;
extern int rotate_hi, rotate_lo;
extern int calc_status;
extern int showfile;
extern double dxsize, dysize;
extern int fractype;

/* Coordinate corners (defined in common/calcfrac.c / framain2.c) */
extern double xxmin, xxmax, yymin, yymax, xx3rd, yy3rd;
extern double sxmin, sxmax, symin, symax;


/*
 * wasm_zoom_by_factor — scale the view around its center.
 * factor < 1.0 zooms in (e.g. 0.5 = 2x); factor > 1.0 zooms out.
 * After adjusting coordinates, resets sxmin/sxmax/symin/symax so
 * zoom.c reference points stay consistent, then triggers recalculation.
 */
static void wasm_zoom_by_factor(double factor)
{
    double cx = (xxmin + xxmax) * 0.5;
    double cy = (yymin + yymax) * 0.5;
    double hw = (xxmax - xxmin) * 0.5 * factor;
    double hh = (yymax - yymin) * 0.5 * factor;
    restart_requested = 1; /* interrupt any in-progress calcfract() */
    xxmin = cx - hw;
    xxmax = cx + hw;
    xx3rd = xxmin;
    yymin = cy - hh;
    yymax = cy + hh;
    yy3rd = yymin;
    /* Keep save-corners in sync so zoom.c reference points are correct */
    sxmin = xxmin;
    sxmax = xxmax;
    symin = yymin;
    symax = yymax;
    calc_status = 0;
    wasm_state  = WS_INIT_VIDEO;
}

/*
 * shift_pixel_buffer — memmove the 8-bit palette-index pixel buffer by
 * (dx, dy) pixels.  Positive dx shifts content right (new strip on left);
 * negative dx shifts content left (new strip on right).  The newly
 * exposed strip is zeroed so it renders as palette index 0 (black) until
 * the fractal calculation fills it in.
 */
static void shift_pixel_buffer(int dx, int dy)
{
    if (!screen_pixels || screen_w <= 0 || screen_h <= 0) return;

    if (dx != 0) {
        int y;
        int adx = dx < 0 ? -dx : dx;
        if (adx >= screen_w) {
            memset(screen_pixels, 0, (size_t)(screen_w * screen_h));
            return;
        }
        for (y = 0; y < screen_h; y++) {
            BYTE *row = screen_pixels + y * screen_w;
            if (dx > 0) {
                /* shift content right: new strip on left */
                memmove(row + dx, row, (size_t)(screen_w - dx));
                memset(row, 0, (size_t)dx);
            } else {
                /* shift content left: new strip on right */
                memmove(row, row + adx, (size_t)(screen_w - adx));
                memset(row + screen_w - adx, 0, (size_t)adx);
            }
        }
    }

    if (dy != 0) {
        int ady = dy < 0 ? -dy : dy;
        if (ady >= screen_h) {
            memset(screen_pixels, 0, (size_t)(screen_w * screen_h));
            return;
        }
        if (dy > 0) {
            /* shift content down: new strip at top */
            memmove(screen_pixels + (size_t)dy * screen_w,
                    screen_pixels,
                    (size_t)(screen_h - dy) * screen_w);
            memset(screen_pixels, 0, (size_t)dy * screen_w);
        } else {
            /* shift content up: new strip at bottom */
            memmove(screen_pixels,
                    screen_pixels + (size_t)ady * screen_w,
                    (size_t)(screen_h - ady) * screen_w);
            memset(screen_pixels + (size_t)(screen_h - ady) * screen_w,
                   0, (size_t)ady * screen_w);
        }
    }
}

/*
 * do_pan — incremental pan via arrow key.
 * Shifts the existing pixel buffer so current pixels stay visible while
 * the new strip is computed, then enters WS_PAN_CALC to fill the gap.
 *
 * Key codes (Fractint/DOS convention):
 *   LEFT=1075  RIGHT=1077  UP=1072  DOWN=1080
 */
static void do_pan(int key)
{
    /* Pan by 20% of the current view */
    double frac = 0.20;
    double dx_coord = (xxmax - xxmin) * frac;
    double dy_coord = (yymax - yymin) * frac;
    int    px = (int)(screen_w * frac); /* pixel columns to shift */
    int    py = (int)(screen_h * frac); /* pixel rows to shift */

    switch (key) {
    case 1077: /* RIGHT — view moves right (fractal coords increase) */
        xxmin += dx_coord;  xxmax += dx_coord;  xx3rd += dx_coord;
        /* content shifts left in buffer; new strip appears on right */
        shift_pixel_buffer(-px, 0);
        pan_strip_dir = 1; /* right */
        break;
    case 1075: /* LEFT */
        xxmin -= dx_coord;  xxmax -= dx_coord;  xx3rd -= dx_coord;
        shift_pixel_buffer(px, 0);
        pan_strip_dir = 2; /* left */
        break;
    case 1080: /* DOWN — view moves down (fractal y decreases in canvas coords) */
        yymin -= dy_coord;  yymax -= dy_coord;  yy3rd -= dy_coord;
        shift_pixel_buffer(0, -py);
        pan_strip_dir = 3; /* down */
        break;
    case 1072: /* UP */
        yymin += dy_coord;  yymax += dy_coord;  yy3rd += dy_coord;
        shift_pixel_buffer(0, py);
        pan_strip_dir = 4; /* up */
        break;
    default:
        return;
    }

    sxmin = xxmin;  sxmax = xxmax;
    symin = yymin;  symax = yymax;
    calc_status = 0;
    wasm_state  = WS_PAN_CALC;
}

static void wasm_main_loop_callback(void)
{
    switch (wasm_state) {

    case WS_INIT_VIDEO:
        /* Clear interrupt/watchdog state before starting a new calculation */
        restart_requested  = 0;
        keybuffer          = 0; /* flush any lingering ESC injected by restart */
        calcfrac_frame_count = 0;

        /* Flush any stale keys from the user-visible ring too */
        key_head = key_tail = 0;

        /* Set up video mode — mirrors big_while_loop() first iteration */
        far_memcpy((char far *)&videoentry,
                   (char far *)&videotable[adapter],
                   sizeof(videoentry));

        dotmode   = videoentry.dotmode % 1000;
        textsafe2 = dotmode / 100;
        dotmode  %= 100;
        xdots     = videoentry.xdots;
        ydots     = videoentry.ydots;
        colors    = videoentry.colors;
        sxdots    = xdots;
        sydots    = ydots;
        sxoffs    = syoffs = 0;
        if (rotate_hi >= colors) rotate_hi = colors - 1;

        diskvideo    = 0;
        diskisactive = 1;

        setvideomode(videoentry.videomodeax,
                     videoentry.videomodebx,
                     videoentry.videomodecx,
                     videoentry.videomodedx);

        diskisactive = 0;
        dxsize = xdots - 1;
        dysize = ydots - 1;

        /* Prepare fractal calculation */
        calc_status = 0;
        showfile    = 1;
        calcfracinit();

        /* Restore URL-supplied coordinates after calcfracinit() resets them */
        if (pending_coords) {
            xxmin = pending_xxmin;  xxmax = pending_xxmax;
            yymin = pending_yymin;  yymax = pending_yymax;
            xx3rd = xxmin;          yy3rd = yymin;
            sxmin = xxmin;          sxmax = xxmax;
            symin = yymin;          symax = yymax;
            pending_coords = 0;
        }

        wasm_state = WS_CALCFRAC;
        break;

    case WS_CALCFRAC:
        {
            int result;

            /* Watchdog: if stuck too long, force a safe restart */
            if (++calcfrac_frame_count > calcfrac_watchdog) {
                calcfrac_frame_count = 0;
                wasm_state = WS_INIT_VIDEO;
                break;
            }

            result = calcfract();
            if (result == 0 || calc_status == 4) {
                writevideopalette();
                calcfrac_frame_count = 0;
                wasm_state = WS_DONE;
            } else if (result < 0 && restart_requested) {
                /* Interrupted by our own restart flag — go to WS_INIT_VIDEO
                 * which will clear the flag and start fresh. */
                wasm_state = WS_INIT_VIDEO;
            }
            /* Other interrupts (result < 0, restart not requested) mean
             * calcfract() returned early but wants to resume next frame;
             * stay in WS_CALCFRAC. */
        }
        break;

    case WS_DONE:
        {
            /* Process navigation/cycling keys once per frame.
             * Key codes follow Fractint/DOS conventions:
             *   PAGE_UP=1073, PAGE_DOWN=1081, HOME=1071
             *   LEFT=1075, RIGHT=1077, UP=1072, DOWN=1080
             */
            int key = key_pop(); /* bypass restart_requested check in xgetkey */
            if (key != 0) {
                switch (key) {
                case 'c':
                    cycle_dir = 1;
                    wasm_state = WS_COLOR_CYCLE;
                    break;
                case '+':
                    cycle_dir = 1;
                    wasm_state = WS_COLOR_CYCLE;
                    break;
                case '-':
                    cycle_dir = -1;
                    wasm_state = WS_COLOR_CYCLE;
                    break;

                /* Zoom in: PageUp = shrink view to 50% of current size */
                case 1073: /* PAGE_UP */
                    wasm_zoom_by_factor(0.5);
                    break;

                /* Zoom out: PageDown = expand view to 200% of current size */
                case 1081: /* PAGE_DOWN */
                    wasm_zoom_by_factor(2.0);
                    break;

                /* Pan: arrow keys — incremental buffer shift + strip recalc */
                case 1075: /* LEFT_ARROW */
                case 1077: /* RIGHT_ARROW */
                case 1072: /* UP_ARROW */
                case 1080: /* DOWN_ARROW */
                    do_pan(key);
                    break;

                /* Home: reset view to default for current fractal type */
                case 1071: /* HOME */
                    restart_requested = 1;
                    calc_status = 0;
                    wasm_state  = WS_INIT_VIDEO;
                    break;

                default:
                    break;
                }
            }
        }
        break;

    case WS_COLOR_CYCLE:
        /* Advance palette once every cycle_skip_max frames.
         * This provides real speed control since delay() is a no-op in WASM. */
        if (++cycle_skip_counter >= cycle_skip_max) {
            cycle_skip_counter = 0;
            spindac(cycle_dir, 1);
        }
        /* Any keypress stops cycling */
        if (key_pop() != 0) {
            wasm_state = WS_DONE;
        }
        break;

    case WS_PAN_CALC:
        /*
         * The pixel buffer was already shifted in do_pan() so the user sees
         * the new view position immediately.  Now do a full recalculation to
         * fill in the correct pixel values across the whole screen.
         *
         * We drop straight into WS_INIT_VIDEO rather than trying to restrict
         * calcfract() to just the new strip: calcfract() reads its bounds from
         * the worklist (built by calcfracinit), so setting ixstart/ixstop
         * directly has no effect on the standard escape-time engine.
         *
         * The slight visual artefact (shifted old pixels briefly visible) is
         * far better UX than a black screen during the recalc.
         */
        pan_strip_dir = 0;
        wasm_state = WS_INIT_VIDEO;
        break;
    }
}

/*
 * wasm_start_main_loop — called from fractint.c (inside #ifdef WASM_BUILD)
 * instead of big_while_loop().
 * adapter=0 selects videotable[0] = 800x600x256, dotmode=19.
 */
EMSCRIPTEN_KEEPALIVE
void wasm_start_main_loop(void)
{
    adapter    = 0;
    wasm_state = WS_INIT_VIDEO;
    /* simulate_infinite_loop=0: return to JS immediately */
    emscripten_set_main_loop(wasm_main_loop_callback, 0, 0);
}

/*
 * wasm_toggle_cycle — JS calls this to start/stop color cycling.
 * dir: +1 = forward, -1 = reverse, 0 = use current direction.
 * If already cycling, stops; if in DONE state, starts cycling.
 */
EMSCRIPTEN_KEEPALIVE
void wasm_toggle_cycle(int dir)
{
    if (wasm_state == WS_COLOR_CYCLE) {
        wasm_state = WS_DONE;
    } else if (wasm_state == WS_DONE) {
        if (dir != 0) cycle_dir = dir;
        wasm_state = WS_COLOR_CYCLE;
    }
}

/*
 * wasm_set_cycle_speed — JS calls this to control cycling speed.
 * Maps speed 1-10 to cycle_skip_max 10..1:
 *   speed=10 (fastest) → skip_max=1  (advance every frame)
 *   speed=1  (slowest) → skip_max=10 (advance every 10th frame)
 * delay() is a no-op in WASM, so frame-skipping is the only effective mechanism.
 */
EMSCRIPTEN_KEEPALIVE
void wasm_set_cycle_speed(int speed)
{
    if (speed < 1)  speed = 1;
    if (speed > 10) speed = 10;
    cycle_skip_max     = 11 - speed;
    cycle_skip_counter = 0; /* reset counter so change takes effect immediately */
}

/*
 * wasm_set_fractype — change the current fractal type and trigger a full
 * re-initialisation (WS_INIT_VIDEO resets coordinates from fractalspecific[]).
 * Use fractype values from fractype.h, e.g. MANDELFP=4, JULIAFP=6.
 */
EMSCRIPTEN_KEEPALIVE
void wasm_set_fractype(int type)
{
    restart_requested = 1; /* interrupt any in-progress calcfract() */
    fractype   = type;
    calc_status = 0;
    wasm_state  = WS_INIT_VIDEO;
}

/*
 * wasm_get_fractype — return the currently active fractal type index.
 */
EMSCRIPTEN_KEEPALIVE
int wasm_get_fractype(void)
{
    return fractype;
}

/*
 * wasm_resize — JS calls this when the canvas container changes size.
 * Reallocates the pixel buffer, updates Fractint globals sxdots/sydots,
 * and triggers a full recalculation from WS_INIT_VIDEO.
 */
EMSCRIPTEN_KEEPALIVE
void wasm_resize(int width, int height)
{
    /* Clamp to reasonable bounds */
    if (width  < 320)  width  = 320;
    if (height < 240)  height = 240;
    if (width  > 3840) width  = 3840;
    if (height > 2160) height = 2160;

    screen_w = width;
    screen_h = height;

    /* Reallocate pixel buffer to match new dimensions */
    ensure_screen_buffer();
    if (!screen_pixels) return;

    /* Update Fractint globals */
    sxdots = width;
    sydots  = height;
    xdots   = sxdots;
    ydots   = sydots;

    videotable[0].xdots = sxdots;
    videotable[0].ydots = sydots;

    /* Trigger full reinitialisation */
    restart_requested = 1; /* interrupt any in-progress calcfract() */
    calc_status = 0;
    wasm_state  = WS_INIT_VIDEO;
}

/*
 * wasm_zoom_to_rect — zoom into the pixel rectangle (px1,py1)-(px2,py2).
 * Coordinates are in canvas pixel space; the function normalises them so
 * px1 < px2 and py1 < py2 regardless of drag direction.
 */
EMSCRIPTEN_KEEPALIVE
void wasm_zoom_to_rect(int px1, int py1, int px2, int py2)
{
    double dx, dy, new_xmin, new_xmax, new_ymin, new_ymax;
    int tmp;

    if (screen_w <= 0 || screen_h <= 0) return;

    /* Normalise so px1 <= px2, py1 <= py2 */
    if (px1 > px2) { tmp = px1; px1 = px2; px2 = tmp; }
    if (py1 > py2) { tmp = py1; py1 = py2; py2 = tmp; }

    /* Guard against zero-size box */
    if (px2 <= px1 || py2 <= py1) return;

    dx = xxmax - xxmin;
    dy = yymax - yymin;

    new_xmin = xxmin + (dx * px1) / screen_w;
    new_xmax = xxmin + (dx * px2) / screen_w;
    /* Canvas y=0 is top; fractal y increases upward */
    new_ymax = yymax - (dy * py1) / screen_h;
    new_ymin = yymax - (dy * py2) / screen_h;

    restart_requested = 1; /* interrupt any in-progress calcfract() */
    xxmin = new_xmin;
    xxmax = new_xmax;
    yymin = new_ymin;
    yymax = new_ymax;
    xx3rd = xxmin;
    yy3rd = yymin;

    sxmin = xxmin; sxmax = xxmax;
    symin = yymin; symax = yymax;

    calc_status = 0;
    wasm_state  = WS_INIT_VIDEO;
}

/*
 * wasm_zoom_at_point — zoom centred on pixel (px, py) by the given factor.
 * factor < 1.0 zooms in (e.g. 0.5 = 2x); factor > 1.0 zooms out.
 */
EMSCRIPTEN_KEEPALIVE
void wasm_zoom_at_point(int px, int py, double factor)
{
    double cx, cy, hw, hh;

    if (screen_w <= 0 || screen_h <= 0) return;

    cx = xxmin + (xxmax - xxmin) * px / screen_w;
    cy = yymax - (yymax - yymin) * py / screen_h;

    hw = (xxmax - xxmin) * factor * 0.5;
    hh = (yymax - yymin) * factor * 0.5;

    restart_requested = 1; /* interrupt any in-progress calcfract() */
    xxmin = cx - hw;
    xxmax = cx + hw;
    yymin = cy - hh;
    yymax = cy + hh;
    xx3rd = xxmin;
    yy3rd = yymin;

    sxmin = xxmin; sxmax = xxmax;
    symin = yymin; symax = yymax;

    calc_status = 0;
    wasm_state  = WS_INIT_VIDEO;
}

/* ------------------------------------------------------------------ */
/* xfcurses.c replacement — text layer backed by text_buf            */
/* (text_buf, text_mode_active, etc. declared at top of file)        */
/* ------------------------------------------------------------------ */

static void text_putchar(int row, int col, int ch, int attr)
{
    if (row >= 0 && row < TEXT_ROWS && col >= 0 && col < TEXT_COLS) {
        text_buf[row * TEXT_COLS + col] =
            (uint16_t)((ch & 0xFF) | ((attr & 0xFF) << 8));
        text_buf_dirty = 1;
    }
}

static WINDOW wasm_win = {0, 0, 0, 0, 25, 80, TEXT_ATTR_DEFAULT, NULL, NULL};
WINDOW *curwin = &wasm_win;

WINDOW *initscr(void)
{
    curwin = &wasm_win;
    curwin->_num_y = LINES;
    curwin->_num_x = COLS;
    return curwin;
}

void endwin(void)  { }
void cbreak(void)  { }
void nocbreak(void){ }
void echo(void)    { }
void noecho(void)  { }

void clear(void)
{
    int i;
    for (i = 0; i < TEXT_ROWS * TEXT_COLS; i++)
        text_buf[i] = (uint16_t)(' ' | (TEXT_ATTR_DEFAULT << 8));
    if (curwin) { curwin->_cur_y = 0; curwin->_cur_x = 0; }
    text_buf_dirty   = 1;
    /* Called only from setvideomode() case 0 — mark text mode active */
    text_mode_active = 1;
}

int  standout(void){ return 0; }
int  standend(void){ return 0; }

void wmove(WINDOW *win, int y, int x)
{
    if (!win) return;
    win->_cur_y = y;
    win->_cur_x = x;
}

void waddch(WINDOW *win, const chtype ch)
{
    int attr, r, c;
    if (!win) return;
    /* Newline: advance row, reset col */
    if ((ch & 0xFF) == (unsigned)'\n') {
        win->_cur_y++;
        win->_cur_x = 0;
        return;
    }
    attr = win->_cur_attr ? win->_cur_attr : TEXT_ATTR_DEFAULT;
    r = win->_cur_y;
    c = win->_cur_x;
    text_putchar(r, c, (int)(ch & 0xFF), attr & 0xFF);
    win->_cur_x++;
    if (win->_cur_x >= TEXT_COLS) {
        win->_cur_x = 0;
        win->_cur_y++;
    }
}

void waddstr(WINDOW *win, char *str)
{
    if (!win || !str) return;
    while (*str) {
        waddch(win, (chtype)(unsigned char)*str++);
    }
}

void wclear(WINDOW *win)
{
    int r, c, attr;
    if (!win) return;
    attr = win->_cur_attr ? win->_cur_attr : TEXT_ATTR_DEFAULT;
    for (r = 0; r < TEXT_ROWS; r++)
        for (c = 0; c < TEXT_COLS; c++)
            text_putchar(r, c, ' ', attr & 0xFF);
    win->_cur_y = 0;
    win->_cur_x = 0;
    text_buf_dirty = 1;
}

void wdeleteln(WINDOW *win)
{
    /* Scroll up from win->_cur_y to TEXT_ROWS-1, blank last line */
    int row, col, attr;
    if (!win) return;
    row = win->_cur_y;
    if (row < 0 || row >= TEXT_ROWS - 1) return;
    attr = win->_cur_attr ? win->_cur_attr : TEXT_ATTR_DEFAULT;
    memmove(&text_buf[row * TEXT_COLS],
            &text_buf[(row + 1) * TEXT_COLS],
            (size_t)(TEXT_ROWS - 1 - row) * TEXT_COLS * sizeof(uint16_t));
    for (col = 0; col < TEXT_COLS; col++)
        text_putchar(TEXT_ROWS - 1, col, ' ', attr & 0xFF);
    text_buf_dirty = 1;
}

void winsertln(WINDOW *win)
{
    /* Scroll down from win->_cur_y, blank current line */
    int row, col, attr;
    if (!win) return;
    row = win->_cur_y;
    if (row < 0 || row >= TEXT_ROWS) return;
    attr = win->_cur_attr ? win->_cur_attr : TEXT_ATTR_DEFAULT;
    if (row < TEXT_ROWS - 1)
        memmove(&text_buf[(row + 1) * TEXT_COLS],
                &text_buf[row * TEXT_COLS],
                (size_t)(TEXT_ROWS - 1 - row) * TEXT_COLS * sizeof(uint16_t));
    for (col = 0; col < TEXT_COLS; col++)
        text_putchar(row, col, ' ', attr & 0xFF);
    text_buf_dirty = 1;
}

void wrefresh(WINDOW *win)                 { (void)win; text_buf_dirty = 1; }
void xrefresh(WINDOW *win, int l1, int l2) { (void)win; (void)l1; (void)l2; text_buf_dirty = 1; }
void touchwin(WINDOW *win)                 { (void)win; }
void wtouchln(WINDOW *win, int y, int n, int changed) { (void)win; (void)y; (void)n; (void)changed; }

void wstandout(WINDOW *win)
{
    /* Switch to inverse-video: swap fg/bg nibbles of current attribute */
    int attr;
    if (!win) return;
    attr = win->_cur_attr ? win->_cur_attr : TEXT_ATTR_DEFAULT;
    win->_cur_attr = ((attr & 0x0F) << 4) | ((attr >> 4) & 0x0F);
}

void wstandend(WINDOW *win)
{
    /* Restore to default attribute */
    if (!win) return;
    win->_cur_attr = TEXT_ATTR_DEFAULT;
}

void delwin(WINDOW *win)
{
    if (win && win != &wasm_win) free(win);
}

void mvcur(int or, int oc, int nr, int nc)
{
    (void)or; (void)oc; (void)nr; (void)nc;
}

void refresh(int line1, int line2)         { (void)line1; (void)line2; text_buf_dirty = 1; }
void xpopup(char *str)                     { (void)str; }
void set_margins(int width, int height)    { (void)width; (void)height; }

WINDOW *newwin(int nlines, int ncols, int begin_y, int begin_x)
{
    WINDOW *w = (WINDOW *)malloc(sizeof(WINDOW));
    if (!w) return NULL;
    memset(w, 0, sizeof(WINDOW));
    w->_num_y    = nlines;
    w->_num_x    = ncols;
    w->_cur_y    = begin_y;
    w->_cur_x    = begin_x;
    w->_cur_attr = TEXT_ATTR_DEFAULT;
    return w;
}

/* ------------------------------------------------------------------ */
/* Text/graphics mode switching                                        */
/* ------------------------------------------------------------------ */

/*
 * setvideomode() in unix/video.c calls clear()+wrefresh() for dotmode==0
 * (text mode) and startvideo() for dotmode==19 (graphics).
 * We hook text_mode_active via startvideo() for graphics-off and
 * the clear() function above for text-on.
 *
 * startvideo() is our own function defined earlier in this file; we
 * patch it to also clear the text_mode_active flag.
 */

/* ------------------------------------------------------------------ */
/* Exported text-buffer accessors for JS rendering                    */
/* ------------------------------------------------------------------ */

EMSCRIPTEN_KEEPALIVE
uint16_t *wasm_get_text_buf(void)
{
    return text_buf;
}

EMSCRIPTEN_KEEPALIVE
int wasm_is_text_mode(void)
{
    return text_mode_active;
}

EMSCRIPTEN_KEEPALIVE
int wasm_consume_text_dirty(void)
{
    int d = text_buf_dirty;
    text_buf_dirty = 0;
    return d;
}

/*
 * wasm_enter_text_mode — JS test helper: activates text mode and writes
 * a sample string to the text buffer so the overlay can be verified.
 * Not called by production code paths.
 */
EMSCRIPTEN_KEEPALIVE
void wasm_enter_text_mode(void)
{
    int col;
    const char *msg = "Fractint Text Mode";
    clear(); /* sets text_mode_active = 1 */
    /* Write sample text on row 0 */
    if (curwin) {
        wmove(curwin, 0, 1);
        curwin->_cur_attr = 0x1F; /* bright white on blue */
        while (*msg) {
            waddch(curwin, (chtype)(unsigned char)*msg++);
        }
    }
    text_buf_dirty = 1;
}

/*
 * wasm_exit_text_mode — JS test helper: deactivates text mode.
 */
EMSCRIPTEN_KEEPALIVE
void wasm_exit_text_mode(void)
{
    text_mode_active = 0;
    text_buf_dirty   = 1;
}

/* ------------------------------------------------------------------ */
/* Coordinate and parameter exports for URL sharing                   */
/* ------------------------------------------------------------------ */

EMSCRIPTEN_KEEPALIVE
double wasm_get_xxmin(void) { return xxmin; }

EMSCRIPTEN_KEEPALIVE
double wasm_get_xxmax(void) { return xxmax; }

EMSCRIPTEN_KEEPALIVE
double wasm_get_yymin(void) { return yymin; }

EMSCRIPTEN_KEEPALIVE
double wasm_get_yymax(void) { return yymax; }

EMSCRIPTEN_KEEPALIVE
int wasm_get_maxit(void)
{
    extern long maxit;
    return (int)maxit;
}

/*
 * wasm_set_coords — restore fractal coordinate corners from saved state
 * (e.g. URL hash).  Triggers a full recalculation.
 */
EMSCRIPTEN_KEEPALIVE
void wasm_set_coords(double xmin, double xmax, double ymin, double ymax)
{
    /* Store as pending so they survive the calcfracinit() coord reset
     * that happens at the start of every WS_INIT_VIDEO cycle. */
    pending_xxmin  = xmin;  pending_xxmax  = xmax;
    pending_yymin  = ymin;  pending_yymax  = ymax;
    pending_coords = 1;

    restart_requested = 1;
    xxmin = xmin;  xxmax = xmax;
    yymin = ymin;  yymax = ymax;
    xx3rd = xxmin; yy3rd = yymin;
    sxmin = xxmin; sxmax = xxmax;
    symin = yymin; symax = yymax;
    calc_status = 0;
    wasm_state  = WS_INIT_VIDEO;
}

/* pixel[] array — referenced in video.c's #ifndef NCURSES block */
unsigned long pixel[48] = {0};

/* XImage* referenced in xfcurses.c; declared as void* for WASM */
void *Ximage = NULL;

/*
 * X11 globals that xfcurses.h declares extern.
 * Our xfcurses.h typedefs these as void* / unsigned long so they
 * match the extern declarations exactly.
 */
Display  *Xdp   = NULL;
Window    Xw    = 0;
Window    Xwc   = 0;
Window    Xwp   = 0;
Window    Xroot = 0;
GC        Xgc   = NULL;
Visual   *Xvi   = NULL;
Screen   *Xsc   = NULL;
Colormap  Xcmap = 0;
int       Xdscreen = 0;
int       Xdepth   = 8;
char     *Xmessage  = NULL;
char     *Xdisplay  = "";
char     *Xgeometry = NULL;
char     *Xfontname = "fixed";
char     *Xfontnamebold = "fixed";

Atom wm_delete_window = 0;
Atom wm_protocols     = 0;

void Open_XDisplay(void) { }

/* ------------------------------------------------------------------ */
/* Misc globals and stubs needed by video.c / common code             */
/* ------------------------------------------------------------------ */

/* PSviewer: defined in unixscr.c upstream; we own it here */
char *PSviewer = "gv";

/* XFlush: X11 output-buffer flush — no-op in WASM (int return to match X11 sig) */
int XFlush(void *display) { (void)display; return 0; }

int XZoomWaiting = 0;
