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

#ifdef WASM_BUILD
#include <pthread.h>
#include <wasm_simd128.h>
#include "wasm_key.h"
#endif

/* SDL2 via Emscripten -s USE_SDL=2 */
#include <SDL2/SDL.h>

#include "port.h"
/* Include our WASM-safe xfcurses.h before prototyp.h to define WINDOW type */
#include "xfcurses.h"
#include "prototyp.h"
#include "fractype.h"

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

#ifdef WASM_BUILD
/* Double-buffer: menu thread writes to text_buf_back; main thread publishes
 * to text_buf via text_buf_flush() under a generation counter so JS can
 * detect torn reads. */
static uint16_t text_buf_back[TEXT_ROWS * TEXT_COLS];
static _Atomic int text_buf_gen = 0;

static void text_buf_flush(void)
{
    atomic_fetch_add(&text_buf_gen, 1);   /* odd = write in progress */
    memcpy(text_buf, text_buf_back, sizeof(text_buf));
    atomic_fetch_add(&text_buf_gen, 1);   /* even = stable */
    text_buf_dirty = 1;
}
#endif /* WASM_BUILD */

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
/* Pending parameter restore                                          */
/* calcfracinit() resets coords *and* maxit to fractal defaults, so   */
/* we apply JS-supplied values *after* calcfracinit() in WS_INIT_VIDEO.*/
/* ------------------------------------------------------------------ */
static int    pending_coords = 0;
static double pending_xxmin, pending_xxmax, pending_yymin, pending_yymax;

static int  pending_maxit = 0;
static long pending_maxit_val = 0;

static int  pending_inside = 0;
static int  pending_inside_val = 1;

/* ------------------------------------------------------------------ */
/* Key ring buffer (power-of-two size for fast masking)               */
/* ------------------------------------------------------------------ */

#define KEY_BUF_SIZE 64
static int key_ring[KEY_BUF_SIZE];
static int key_head = 0;
static int key_tail = 0;

#ifdef WASM_BUILD
/* Menu key channel and active flag — defined here, declared in wasm_key.h */
KeyChannel menu_chan;
_Atomic int wasm_menu_active = 0;
#endif

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
#ifdef WASM_BUILD
    /* When the menu pthread is active, route through KeyChannel.
     * block=1 means the menu is waiting for input — block until a key arrives.
     * block=0 is a poll — return 0 if nothing is ready. */
    if (atomic_load(&wasm_menu_active)) {
        if (block)
            return key_channel_pop_blocking(&menu_chan);
        else
            return key_channel_pop_nowait(&menu_chan);
    }
#endif
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
#ifdef WASM_BUILD
    if (atomic_load(&wasm_menu_active)) {
        key_channel_push(&menu_chan, keycode);
        return;
    }
#endif
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
/* Parallel pixel queue                                                */
/* ------------------------------------------------------------------ */

#ifdef WASM_BUILD

#define PQUEUE_SIZE 4096   /* must be power of 2; fits >3 rows at 1280px */
#define PQUEUE_MASK (PQUEUE_SIZE - 1)
#define PQUEUE_THREADS 4

typedef struct {
    int    row, col;
    double cx, cy;
} PixelWork;

typedef struct {
    PixelWork  items[PQUEUE_SIZE];
    volatile int head;   /* producer writes here */
    volatile int tail;   /* consumers read here */
    pthread_mutex_t lock;
    pthread_cond_t  not_empty;
    pthread_cond_t  not_full;
    volatile int    done;            /* producer sets to 1 when finished pushing */
    volatile int    abort;           /* set to 1 to stop all workers */
    volatile int    workers_running; /* decremented by each worker on exit */
} PixelQueue;

/* Snapshot of rendering parameters captured at pworkers_start().
 * Workers read these instead of the global variables so a mid-flight
 * wasm_set_maxit() / wasm_set_inside() can't corrupt an in-progress frame. */
typedef struct {
    long  maxit;
    long  bailout;
    int   inside;
    int   colors;
    int   scr_w;
} PWorkerCtx;

static PWorkerCtx pworker_ctx;
static PixelQueue pqueue;
static pthread_t  pworkers[PQUEUE_THREADS];

static void pqueue_init(void)
{
    memset(&pqueue, 0, sizeof(pqueue));
    pthread_mutex_init(&pqueue.lock, NULL);
    pthread_cond_init(&pqueue.not_empty, NULL);
    pthread_cond_init(&pqueue.not_full, NULL);
}

/* Push one item. Blocks if full. Returns 0 if aborted. */
static int pqueue_push(PixelWork item)
{
    pthread_mutex_lock(&pqueue.lock);
    while (((pqueue.head - pqueue.tail) & PQUEUE_MASK) == PQUEUE_MASK - 1) {
        if (pqueue.abort) { pthread_mutex_unlock(&pqueue.lock); return 0; }
        pthread_cond_wait(&pqueue.not_full, &pqueue.lock);
    }
    if (pqueue.abort) { pthread_mutex_unlock(&pqueue.lock); return 0; }
    pqueue.items[pqueue.head & PQUEUE_MASK] = item;
    pqueue.head++;
    pthread_cond_signal(&pqueue.not_empty);
    pthread_mutex_unlock(&pqueue.lock);
    return 1;
}

/* Pop up to 2 items. Returns count popped (0 = queue empty and done). */
static int pqueue_pop2(PixelWork *out)
{
    int n;
    pthread_mutex_lock(&pqueue.lock);
    while ((pqueue.head == pqueue.tail) && !pqueue.done && !pqueue.abort) {
        pthread_cond_wait(&pqueue.not_empty, &pqueue.lock);
    }
    n = 0;
    while (n < 2 && pqueue.head != pqueue.tail && !pqueue.abort) {
        out[n++] = pqueue.items[pqueue.tail & PQUEUE_MASK];
        pqueue.tail++;
        pthread_cond_signal(&pqueue.not_full);
    }
    pthread_mutex_unlock(&pqueue.lock);
    return n;
}

/*
 * iter_to_color — map escape iteration count to palette index.
 *
 * Mirrors Fractint's StandardFractal plot_pixel path for the simple case:
 *   outside >= 0 (default): uses escape iteration count
 *   inside >= 0  (default 1): uses inside color directly
 *
 * For the SIMD parallel path we only handle: outside == 0 (default ITER)
 * and inside == 1 (default blue).  The serial path handles everything else.
 *
 * Standard mapping (save_release > 1950, colors == 256):
 *   escaped:  color = ((iter - 1) % (colors - 1)) + 1   [skips color 0]
 *   inside:   coloriter = inside (1), color = 1          [direct]
 */
static int iter_to_color(long iter, const PWorkerCtx *ctx)
{
    if (iter >= ctx->maxit) {
        /* Inside point — use inside color (default 1 = blue) */
        int c = (ctx->inside >= 0) ? ctx->inside : 0;
        if (c == 0) return 0;
        return c % ctx->colors;
    }
    if (iter == 0) iter = 1; /* match Fractint: coloriter==0 becomes 1 */
    /* Escaped: skip color 0; cycle through colors 1..255 */
    return (int)(((iter - 1) % (ctx->colors - 1)) + 1);
}

static void *pworker_func(void *arg)
{
    const PWorkerCtx *ctx = (const PWorkerCtx *)arg;
    PixelWork items[2];

    while (!pqueue.abort) {
        int n = pqueue_pop2(items);
        if (n == 0) break;   /* queue done and drained */

        if (n == 2) {
            /* SIMD path: process 2 pixels with f64x2 */
            v128_t cr = wasm_f64x2_make(items[0].cx, items[1].cx);
            v128_t ci = wasm_f64x2_make(items[0].cy, items[1].cy);
            v128_t zr = wasm_f64x2_splat(0.0);
            v128_t zi = wasm_f64x2_splat(0.0);
            v128_t bail = wasm_f64x2_splat((double)ctx->bailout);
            /* iteration counts as doubles for lane accumulation */
            v128_t iters  = wasm_f64x2_splat(0.0);
            v128_t one    = wasm_f64x2_splat(1.0);
            v128_t active = wasm_f64x2_splat(1.0); /* 1.0 = still iterating */

            long limit = ctx->maxit;
            long i;
            for (i = 0; i < limit; i++) {
                v128_t zr2  = wasm_f64x2_mul(zr, zr);
                v128_t zi2  = wasm_f64x2_mul(zi, zi);
                v128_t mag2 = wasm_f64x2_add(zr2, zi2);

                /* lanes still within bailout radius */
                v128_t still_in = wasm_f64x2_le(mag2, bail);
                /* active = active AND still_in (keeps 1.0 only for unlaunched) */
                active = wasm_v128_and(active, still_in);
                /* count iterations only for active lanes */
                iters = wasm_f64x2_add(iters, wasm_v128_and(one,
                            wasm_f64x2_ne(active, wasm_f64x2_splat(0.0))));

                /* if both lanes escaped, done early */
                if (!wasm_v128_any_true(active)) break;

                /* z = z² + c */
                v128_t new_zr = wasm_f64x2_add(
                    wasm_f64x2_sub(zr2, zi2), cr);
                v128_t new_zi = wasm_f64x2_add(
                    wasm_f64x2_mul(wasm_f64x2_add(zr, zr), zi), ci);
                zr = new_zr;
                zi = new_zi;
            }

            /* Extract iteration counts from SIMD lanes */
            long iter0 = (long)wasm_f64x2_extract_lane(iters, 0);
            long iter1 = (long)wasm_f64x2_extract_lane(iters, 1);

            /* Write pixels using snapshotted scr_w (not the global) */
            int c0 = iter_to_color(iter0, ctx);
            int c1 = iter_to_color(iter1, ctx);
            screen_pixels[items[0].row * ctx->scr_w + items[0].col] = (unsigned char)c0;
            screen_pixels[items[1].row * ctx->scr_w + items[1].col] = (unsigned char)c1;

        } else {
            /* n == 1: scalar path for odd last pixel */
            double czr = items[0].cx, czi = items[0].cy;
            double pzr = 0.0, pzi = 0.0;
            long iter = 0;
            long limit = ctx->maxit;
            for (; iter < limit; iter++) {
                double r2 = pzr * pzr;
                double i2 = pzi * pzi;
                if (r2 + i2 > (double)ctx->bailout) break;
                double nr = r2 - i2 + czr;
                pzi = 2.0 * pzr * pzi + czi;
                pzr = nr;
            }
            screen_pixels[items[0].row * ctx->scr_w + items[0].col] =
                (unsigned char)iter_to_color(iter, ctx);
        }
    }
    pthread_mutex_lock(&pqueue.lock);
    pqueue.workers_running--;
    pthread_mutex_unlock(&pqueue.lock);
    return NULL;
}

static void pworkers_start(void)
{
    int i;
    /* Snapshot globals that workers must not read live */
    pworker_ctx.maxit    = maxit;
    pworker_ctx.bailout  = bailout;
    pworker_ctx.inside   = inside;
    pworker_ctx.colors   = colors;
    pworker_ctx.scr_w    = screen_w;
    pqueue_init();
    pqueue.done            = 0;
    pqueue.abort           = 0;
    pqueue.head            = 0;
    pqueue.tail            = 0;
    pqueue.workers_running = PQUEUE_THREADS;
    for (i = 0; i < PQUEUE_THREADS; i++) {
        pthread_create(&pworkers[i], NULL, pworker_func, &pworker_ctx);
    }
}

/*
 * pworkers_signal_done — signal workers that no more items will be pushed,
 * without joining.  Workers will drain the queue and exit on their own.
 * WS_PAR_WAIT polls workers_running until it reaches 0, then joins
 * (instantly, since the threads have already exited).
 */
static void pworkers_signal_done(void)
{
    pthread_mutex_lock(&pqueue.lock);
    pqueue.done = 1;
    pthread_cond_broadcast(&pqueue.not_empty);
    pthread_mutex_unlock(&pqueue.lock);
}

/*
 * pworkers_join — join all worker threads after they have exited.
 * Called only from WS_PAR_WAIT after workers_running reaches 0.
 */
static void pworkers_join(void)
{
    int i;
    for (i = 0; i < PQUEUE_THREADS; i++) {
        pthread_join(pworkers[i], NULL);
    }
}

static void pworkers_abort(void)
{
    int i;
    pthread_mutex_lock(&pqueue.lock);
    pqueue.abort = 1;
    pthread_cond_broadcast(&pqueue.not_empty);
    pthread_cond_broadcast(&pqueue.not_full);
    pthread_mutex_unlock(&pqueue.lock);
    for (i = 0; i < PQUEUE_THREADS; i++) {
        pthread_join(pworkers[i], NULL);
    }
    pqueue.workers_running = 0; /* mark as fully cleaned up */
}

/*
 * fractype_is_parallel_safe — returns 1 if the current fractal type
 * can be computed by our SIMD Mandelbrot path, and the coloring mode
 * is simple enough (outside==ITER, inside>=0).
 *
 * We handle MANDELFP (4) and MANDEL integer (0) since both iterate
 * z = z² + c from z=0.  Julia types require a fixed c and variable z0
 * which differs from the Mandelbrot formula — fall through to serial.
 *
 * outside == 0 means ITER (default: colour by iteration count).
 * inside  >= 0 means a fixed palette index (default 1 = blue).
 */
static int fractype_is_parallel_safe(void)
{
    /* Only handle Mandelbrot (fp and integer), not Julia variants */
    if (fractype != MANDELFP && fractype != MANDEL)
        return 0;
    /* Only simple outside/inside coloring — no smooth, no distance est. */
    if (outside != 0 || inside < 0)
        return 0;
    return 1;
}

/* Current row being pushed by the producer (WS_PAR_PRODUCE) */
static int par_current_row = 0;

/* ------------------------------------------------------------------ */
/* Menu pthread (WS_MENU)                                             */
/* ------------------------------------------------------------------ */

/* IMAGESTART=2, RESTORESTART=3, RESTART=1, CONTINUE=4 from fractint.h */
#include "fractint.h"

typedef struct { int trigger_key; } MenuThreadArg;
static MenuThreadArg menu_thread_arg;
static pthread_t     menu_thread_id;
static _Atomic int   menu_done = 0;
static int           menu_result  = 0;
static int           menu_kbdchar = 0;
static int           menu_watchdog_frames = 0;

/* Fractint menu functions (declared in prototyp.h, already included above) */

static void *menu_thread_func(void *arg)
{
    MenuThreadArg *marg = (MenuThreadArg *)arg;
    int kbdmore    = 1;
    int frommandel = 0;
    char stacked[4] = {0};
    int kbdchar    = marg->trigger_key;
    int mms_value  = CONTINUE;

    /* Enter text mode directly — avoids stackscreen()'s curses window
     * allocation machinery (farmemalloc/newwin) which is unsafe from a
     * pthread context. LINES/COLS may be 0 on this thread, and newwin()
     * with zero dimensions triggers an unreachable trap in the WASM build. */
    text_mode_active = 1;
    memset(text_buf_back, ' ', sizeof(text_buf_back));
    text_buf_flush();

    do {
        mms_value = main_menu_switch(&kbdchar, &frommandel,
                                     &kbdmore, stacked, 0);
        if (mms_value == CONTINUE && kbdmore) {
            kbdchar = getakey();
        }
    } while (mms_value == CONTINUE && kbdmore);

    /* Exit text mode directly */
    text_mode_active = 0;
    text_buf_dirty   = 1;

    menu_result  = mms_value;
    menu_kbdchar = kbdchar;

    atomic_store(&wasm_menu_active, 0);
    atomic_store(&menu_done, 1);
    return NULL;
}

#endif /* WASM_BUILD */

/* ------------------------------------------------------------------ */
/* WASM main loop                                                      */
/* ------------------------------------------------------------------ */

/*
 * wasm_state drives the render pipeline without blocking the browser
 * event loop.
 *
 * WS_INIT_VIDEO    — first callback: set video mode, init fractal params
 * WS_CALCFRAC      — subsequent callbacks: run calcfrac() chunk-by-chunk
 * WS_DONE          — fractal finished; idle, processes navigation keys
 * WS_COLOR_CYCLE   — one spindac() step per frame; stops on any keypress
 * WS_PAR_PRODUCE   — multi-frame producer: push one row per frame into pqueue
 * WS_PAR_WAIT      — wait for workers to drain and finish, then transition
 */
typedef enum {
    WS_INIT_VIDEO = 0,
    WS_CALCFRAC,
    WS_DONE,
    WS_COLOR_CYCLE,
    WS_PAN_CALC,      /* calculating only the newly exposed strip after a pan */
    WS_PAR_PRODUCE,   /* parallel SIMD path: push pixel rows into queue */
    WS_PAR_WAIT,      /* parallel SIMD path: wait for workers to finish */
    WS_MENU           /* dedicated pthread running curses menu */
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

/* Forward declarations for functions defined later in this file */
void wasm_toggle_cycle(int dir);
#ifdef WASM_BUILD
void wasm_open_menu(int key);
#endif

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
extern int outside;
extern int inside;
extern long maxit;

/* Coordinate corners (defined in common/calcfrac.c / framain2.c) */
extern double xxmin, xxmax, yymin, yymax, xx3rd, yy3rd;
extern double sxmin, sxmax, symin, symax;

/* Fractal parameters: param[0]=Julia real, param[1]=Julia imag, etc. */
extern double param[];
/* Escape radius (bailout is a long in this version of Fractint) */
extern long   bailout;


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

        /* Restore JS-supplied values after calcfracinit() resets them */
        if (pending_coords) {
            xxmin = pending_xxmin;  xxmax = pending_xxmax;
            yymin = pending_yymin;  yymax = pending_yymax;
            xx3rd = xxmin;          yy3rd = yymin;
            sxmin = xxmin;          sxmax = xxmax;
            symin = yymin;          symax = yymax;
            pending_coords = 0;
        }
        if (pending_maxit) {
            maxit = pending_maxit_val;
            pending_maxit = 0;
        }
        if (pending_inside) {
            inside = pending_inside_val;
            pending_inside = 0;
        }

        wasm_state = WS_CALCFRAC;
        break;

    case WS_CALCFRAC:
        {
            int result;

#ifdef WASM_BUILD
            /* Parallel SIMD path for Mandelbrot with simple coloring */
            if (fractype_is_parallel_safe()) {
                pworkers_start();
                par_current_row = 0;
                calcfrac_frame_count = 0;
                wasm_state = WS_PAR_PRODUCE;
                break;
            }
#endif

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
                    /* Keys that open blocking menus cannot run in WASM — handle
                     * useful ones directly; silently ignore the rest. */
                    switch (key) {
                    case '\\':   /* backslash — toggle color cycling */
                        wasm_toggle_cycle(cycle_dir ? cycle_dir : 1);
                        break;
                    case 'e':    /* edit palette — not supportable, ignore */
                    case 'r':    /* restore — ignore */
                    case 's':    /* save — ignore (PNG save is in JS) */
                    case 'd':    /* shell — ignore */
                        break;
                    case 'z':    /* zoom params */
                    case 'x':    /* extended params */
                    case 'y':    /* more params */
#ifdef WASM_BUILD
                        wasm_open_menu(key);
#endif
                        break;
                    case '@':    /* batch file — ignore */
                        break;
                    case ' ':    /* SPACE — Mandel/Julia toggle via menu */
#ifdef WASM_BUILD
                        wasm_open_menu(' ');
#endif
                        break;
                    case 't':
                    case 'T':
#ifdef WASM_BUILD
                        wasm_open_menu('t');
#endif
                        break;
                    default:
                        /* Truly unknown — do nothing, stay in WS_DONE */
                        break;
                    }
                    break;  /* stay in WS_DONE */
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

#ifdef WASM_BUILD
    case WS_PAR_PRODUCE:
        /*
         * Multi-frame producer: push one full row of pixels per callback
         * invocation into the ring buffer.  Workers drain and compute
         * concurrently.  Each row push is non-blocking (the queue holds
         * PQUEUE_SIZE=4096 items, which comfortably fits one row at any
         * supported resolution).
         *
         * y-coordinate formula mirrors dypixel_calc() in fractals.c:
         *   cy = yymax - row * delyy - col * delyy2
         * For standard (non-skewed) Mandelbrot, yy3rd == yymin so:
         *   delyy  = (yymax - yy3rd) / (ydots-1) = (yymax - yymin) / (ydots-1)
         *   delyy2 = (yy3rd - yymin) / (xdots-1) = 0
         * Therefore: cy = yymax - row * (yymax - yymin) / (ydots - 1)
         *
         * x-coordinate mirrors dxpixel_calc():
         *   cx = xxmin + col * delxx + row * delxx2
         * For standard Mandelbrot: xx3rd == xxmin so delxx2 = 0, giving:
         *   cx = xxmin + col * (xxmax - xxmin) / (xdots - 1)
         */
        {
            double dx = (xdots > 1) ? (xxmax - xxmin) / (xdots - 1) : 0.0;
            double dy = (ydots > 1) ? (yymax - yymin) / (ydots - 1) : 0.0;
            int r = par_current_row;
            double cy = yymax - r * dy;
            int c;

            if (restart_requested) {
                pworkers_abort();
                wasm_state = WS_INIT_VIDEO;
                break;
            }

            /* Push one full row */
            for (c = 0; c < xdots; c++) {
                PixelWork pw;
                pw.row = r;
                pw.col = c;
                pw.cx  = xxmin + c * dx;
                pw.cy  = cy;
                if (!pqueue_push(pw)) {
                    /* aborted (restart_requested set during push) */
                    pworkers_abort();
                    wasm_state = WS_INIT_VIDEO;
                    goto par_produce_done;
                }
            }

            par_current_row++;
            if (par_current_row >= ydots) {
                /* All rows pushed — signal workers that production is done.
                 * Do NOT join here; that would block the browser main thread
                 * until all workers finish.  Transition to WS_PAR_WAIT which
                 * polls workers_running each frame and joins only when all
                 * threads have already exited (instant join). */
                pworkers_signal_done();
                wasm_state = WS_PAR_WAIT;
            }
            par_produce_done:;
        }
        break;

    case WS_PAR_WAIT:
        /*
         * Non-blocking poll: check whether all worker threads have exited.
         * workers_running is decremented inside pworker_func() (under lock)
         * just before each thread returns.  When it reaches 0, all threads
         * have already exited so pthread_join() returns immediately.
         *
         * This keeps the browser main thread free every frame — on slow
         * hardware or high maxit the user never sees a frozen tab during
         * the last batch of computation.
         */
        if (pqueue.workers_running == 0) {
            int i;
            for (i = 0; i < PQUEUE_THREADS; i++) {
                pthread_join(pworkers[i], NULL); /* instant: already exited */
            }
            frame_dirty = 1;
            writevideopalette();
            calc_status = 4;
            wasm_state  = WS_DONE;
        }
        /* else: stay in WS_PAR_WAIT, check again next frame */
        break;

    case WS_MENU:
        /*
         * Non-blocking poll: check whether the menu pthread has finished.
         * menu_done is set atomically by menu_thread_func() before it returns.
         * pthread_join() is instant once the thread has already exited.
         *
         * Watchdog: if menu_done is never set (e.g. the thread crashed via
         * exit(-1) / WASM unreachable trap), recover after ~5 s (300 frames).
         */
        if (atomic_load(&menu_done)) {
            menu_watchdog_frames = 0;
            pthread_join(menu_thread_id, NULL);
            /* Clear key state so no stale keys linger into next calc */
            keybuffer  = 0;
            key_head = key_tail = 0;
            switch (menu_result) {
            case IMAGESTART:
            case RESTORESTART:
            case RESTART:
                restart_requested = 0;
                calc_status = 0;
                wasm_state  = WS_INIT_VIDEO;
                break;
            default:
                wasm_state = WS_DONE;
                break;
            }
        } else if (++menu_watchdog_frames > 300) {
            /* Menu thread appears to have crashed — force recovery */
            menu_watchdog_frames = 0;
            atomic_store(&wasm_menu_active, 0);
            atomic_store(&menu_done, 0);
            /* Flush key channels */
            key_head = key_tail = 0;
            pthread_mutex_lock(&menu_chan.lock);
            menu_chan.head = menu_chan.tail = 0;
            pthread_mutex_unlock(&menu_chan.lock);
            wasm_state = WS_DONE;
        }
        /* else: stay in WS_MENU, check again next frame */
        break;

#endif /* WASM_BUILD */
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
#ifdef WASM_BUILD
    key_channel_init(&menu_chan);
#endif
    /* simulate_infinite_loop=0: return to JS immediately */
    emscripten_set_main_loop(wasm_main_loop_callback, 0, 0);
}

#ifdef WASM_BUILD
/*
 * wasm_open_menu — JS calls this to spawn the menu pthread.
 * Only valid from WS_DONE; ignored if menu is already active.
 */
EMSCRIPTEN_KEEPALIVE
void wasm_open_menu(int key)
{
    if (atomic_load(&wasm_menu_active)) return;

    if (wasm_state == WS_PAR_PRODUCE || wasm_state == WS_PAR_WAIT) {
        /* Abort in-flight parallel render, then fall through to open menu */
        pworkers_abort();
    } else if (wasm_state != WS_DONE) {
        /* Any other non-idle state (WS_CALCFRAC, WS_INIT_VIDEO, etc.) — drop */
        return;
    }

    /* Flush main key ring */
    key_head = key_tail = 0;

    /* Flush menu channel */
    pthread_mutex_lock(&menu_chan.lock);
    menu_chan.head = menu_chan.tail = 0;
    pthread_mutex_unlock(&menu_chan.lock);

    atomic_store(&menu_done, 0);
    menu_watchdog_frames = 0;
    menu_thread_arg.trigger_key = key;

    wasm_state = WS_MENU;
    atomic_store(&wasm_menu_active, 1);

    pthread_create(&menu_thread_id, NULL, menu_thread_func, &menu_thread_arg);
}
#endif /* WASM_BUILD */

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

EMSCRIPTEN_KEEPALIVE
int wasm_is_cycling(void)
{
    return wasm_state == WS_COLOR_CYCLE ? 1 : 0;
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
#ifdef WASM_BUILD
        /* Write to back-buffer; menu thread calls text_buf_flush() to publish */
        text_buf_back[row * TEXT_COLS + col] =
            (uint16_t)((ch & 0xFF) | ((attr & 0xFF) << 8));
#else
        text_buf[row * TEXT_COLS + col] =
            (uint16_t)((ch & 0xFF) | ((attr & 0xFF) << 8));
        text_buf_dirty = 1;
#endif
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
#ifdef WASM_BUILD
    for (i = 0; i < TEXT_ROWS * TEXT_COLS; i++)
        text_buf_back[i] = (uint16_t)(' ' | (TEXT_ATTR_DEFAULT << 8));
    text_buf_flush();
#else
    for (i = 0; i < TEXT_ROWS * TEXT_COLS; i++)
        text_buf[i] = (uint16_t)(' ' | (TEXT_ATTR_DEFAULT << 8));
    text_buf_dirty   = 1;
#endif
    if (curwin) { curwin->_cur_y = 0; curwin->_cur_x = 0; }
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
#ifdef WASM_BUILD
    text_buf_flush();
#else
    text_buf_dirty = 1;
#endif
}

void wdeleteln(WINDOW *win)
{
    /* Scroll up from win->_cur_y to TEXT_ROWS-1, blank last line */
    int row, col, attr;
    if (!win) return;
    row = win->_cur_y;
    if (row < 0 || row >= TEXT_ROWS - 1) return;
    attr = win->_cur_attr ? win->_cur_attr : TEXT_ATTR_DEFAULT;
#ifdef WASM_BUILD
    memmove(&text_buf_back[row * TEXT_COLS],
            &text_buf_back[(row + 1) * TEXT_COLS],
            (size_t)(TEXT_ROWS - 1 - row) * TEXT_COLS * sizeof(uint16_t));
#else
    memmove(&text_buf[row * TEXT_COLS],
            &text_buf[(row + 1) * TEXT_COLS],
            (size_t)(TEXT_ROWS - 1 - row) * TEXT_COLS * sizeof(uint16_t));
#endif
    for (col = 0; col < TEXT_COLS; col++)
        text_putchar(TEXT_ROWS - 1, col, ' ', attr & 0xFF);
#ifdef WASM_BUILD
    text_buf_flush();
#else
    text_buf_dirty = 1;
#endif
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
#ifdef WASM_BUILD
        memmove(&text_buf_back[(row + 1) * TEXT_COLS],
                &text_buf_back[row * TEXT_COLS],
                (size_t)(TEXT_ROWS - 1 - row) * TEXT_COLS * sizeof(uint16_t));
#else
        memmove(&text_buf[(row + 1) * TEXT_COLS],
                &text_buf[row * TEXT_COLS],
                (size_t)(TEXT_ROWS - 1 - row) * TEXT_COLS * sizeof(uint16_t));
#endif
    for (col = 0; col < TEXT_COLS; col++)
        text_putchar(row, col, ' ', attr & 0xFF);
#ifdef WASM_BUILD
    text_buf_flush();
#else
    text_buf_dirty = 1;
#endif
}

void wrefresh(WINDOW *win)
{
    (void)win;
#ifdef WASM_BUILD
    text_buf_flush();
#else
    text_buf_dirty = 1;
#endif
}
void xrefresh(WINDOW *win, int l1, int l2)
{
    (void)win; (void)l1; (void)l2;
#ifdef WASM_BUILD
    text_buf_flush();
#else
    text_buf_dirty = 1;
#endif
}
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

void refresh(int line1, int line2)
{
    (void)line1; (void)line2;
#ifdef WASM_BUILD
    text_buf_flush();
#else
    text_buf_dirty = 1;
#endif
}
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

#ifdef WASM_BUILD
/*
 * wasm_get_text_gen — pointer to text_buf_gen atomic counter.
 * JS reads this via HEAP32 with Atomics.load to detect torn reads:
 *   odd  value = write in progress (skip frame)
 *   even value = stable snapshot
 */
EMSCRIPTEN_KEEPALIVE
int *wasm_get_text_gen(void)
{
    /* _Atomic int and int share the same layout; safe cast for JS HEAP32 */
    return (int *)&text_buf_gen;
}
#endif /* WASM_BUILD */

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
    return (int)maxit;
}

/*
 * wasm_get_julia_re / wasm_get_julia_im — return the current Julia seed
 * (param[0] real and param[1] imaginary).  Used by urlshare.js to include
 * the seed in the URL hash so shared Julia fractal URLs are fully reproducible.
 */
EMSCRIPTEN_KEEPALIVE
double wasm_get_julia_re(void) { return param[0]; }

EMSCRIPTEN_KEEPALIVE
double wasm_get_julia_im(void) { return param[1]; }

/*
 * wasm_set_palette_preset — apply a built-in 256-colour palette.
 *
 * Presets (all values are 6-bit VGA, 0-63):
 *   0 = Default (colourful cycling rainbow — the initUnixWindow default)
 *   1 = Fire    (black → deep red → orange → yellow → white)
 *   2 = Ice     (black → dark blue → cyan → white)
 *   3 = Greens  (black → dark green → bright green → pale yellow)
 *   4 = Sunset  (black → purple → red → orange → yellow)
 *   5 = Classic (Fractint traditional blue-cyan-white gradient)
 */
EMSCRIPTEN_KEEPALIVE
void wasm_set_palette_preset(int preset)
{
    int i;
    switch (preset) {
    case 0: /* Default — same formula as initUnixWindow() */
        for (i = 0; i < 256; i++) {
            dacbox[i][0] = (unsigned char)((i >> 5) * 8 + 7);
            dacbox[i][1] = (unsigned char)((((i + 16) & 28) >> 2) * 8 + 7);
            dacbox[i][2] = (unsigned char)(((i + 2) & 3) * 16 + 15);
        }
        dacbox[0][0] = dacbox[0][1] = dacbox[0][2] = 0;
        dacbox[1][0] = dacbox[1][1] = dacbox[1][2] = 63;
        break;

    case 1: /* Fire */
        dacbox[0][0] = dacbox[0][1] = dacbox[0][2] = 0;
        for (i = 1; i < 256; i++) {
            double t = (double)i / 255.0;
            /* R: 0→63 early, G: 0→63 mid, B: stays near 0 */
            dacbox[i][0] = (unsigned char)(t < 0.5 ? t * 2.0 * 63 : 63);
            dacbox[i][1] = (unsigned char)(t < 0.5 ? 0 : (t - 0.5) * 2.0 * 63);
            dacbox[i][2] = (unsigned char)(t > 0.85 ? (t - 0.85) / 0.15 * 40 : 0);
        }
        break;

    case 2: /* Ice */
        dacbox[0][0] = dacbox[0][1] = dacbox[0][2] = 0;
        for (i = 1; i < 256; i++) {
            double t = (double)i / 255.0;
            dacbox[i][0] = (unsigned char)(t > 0.7 ? (t - 0.7) / 0.3 * 63 : 0);
            dacbox[i][1] = (unsigned char)(t < 0.5 ? t * 2.0 * 40 : 40 + (t - 0.5) * 2.0 * 23);
            dacbox[i][2] = (unsigned char)(t < 0.5 ? t * 2.0 * 63 : 63);
        }
        break;

    case 3: /* Greens */
        dacbox[0][0] = dacbox[0][1] = dacbox[0][2] = 0;
        for (i = 1; i < 256; i++) {
            double t = (double)i / 255.0;
            dacbox[i][0] = (unsigned char)(t > 0.8 ? (t - 0.8) / 0.2 * 40 : 0);
            dacbox[i][1] = (unsigned char)(t * 63);
            dacbox[i][2] = (unsigned char)(t > 0.8 ? (t - 0.8) / 0.2 * 30 : 0);
        }
        break;

    case 4: /* Sunset */
        dacbox[0][0] = dacbox[0][1] = dacbox[0][2] = 0;
        for (i = 1; i < 256; i++) {
            double t = (double)i / 255.0;
            /* purple → red → orange → yellow */
            dacbox[i][0] = (unsigned char)(t < 0.3 ? t / 0.3 * 40 : 40 + (t - 0.3) / 0.7 * 23);
            dacbox[i][1] = (unsigned char)(t < 0.5 ? 0 : (t - 0.5) / 0.5 * 55);
            dacbox[i][2] = (unsigned char)(t < 0.3 ? t / 0.3 * 30 : t < 0.5 ? (0.5 - t) / 0.2 * 30 : 0);
        }
        break;

    default: /* Classic Fractint blue-cyan-white */
    case 5:
        dacbox[0][0] = dacbox[0][1] = dacbox[0][2] = 0;
        for (i = 1; i < 256; i++) {
            double t = (double)i / 255.0;
            dacbox[i][0] = (unsigned char)(t > 0.66 ? (t - 0.66) / 0.34 * 63 : 0);
            dacbox[i][1] = (unsigned char)(t > 0.33 ? (t - 0.33) / 0.67 * 63 : 0);
            dacbox[i][2] = (unsigned char)(t * 63);
        }
        break;
    }
    writevideopalette();
    /* palette_dirty is set by writevideopalette(); frame will re-blit */
}

/*
 * wasm_set_maxit — change the maximum iteration count.
 * calcfracinit() resets maxit to the fractal's default, so we use the
 * pending pattern to apply the value after calcfracinit() runs.
 */
EMSCRIPTEN_KEEPALIVE
void wasm_set_maxit(int n)
{
    if (n < 2)     n = 2;
    if (n > 32767) n = 32767;
    pending_maxit_val = (long)n;
    pending_maxit     = 1;
    restart_requested = 1;
    calc_status = 0;
    wasm_state  = WS_INIT_VIDEO;
}

/*
 * wasm_set_inside — change the inside coloring mode.
 * Common values: 0=black, 1=blue (default), -1=ZMAG, -2=BOFS.
 * calcfracinit() may reset inside, so use the pending pattern.
 */
EMSCRIPTEN_KEEPALIVE
void wasm_set_inside(int mode)
{
    pending_inside_val = mode;
    pending_inside     = 1;
    restart_requested  = 1;
    calc_status = 0;
    wasm_state  = WS_INIT_VIDEO;
}

EMSCRIPTEN_KEEPALIVE
int wasm_get_inside(void) { return inside; }

/*
 * wasm_set_outside — change the outside (escaped) coloring mode.
 * Common values: 0=ITER (default), 2=REAL, 3=IMAG, 4=MULT, 5=SUM, 6=ATAN.
 */
EMSCRIPTEN_KEEPALIVE
void wasm_set_outside(int mode)
{
    outside = mode;
    restart_requested = 1;
    calc_status = 0;
    wasm_state  = WS_INIT_VIDEO;
}

EMSCRIPTEN_KEEPALIVE
int wasm_get_outside(void) { return outside; }

/*
 * wasm_set_bailout — change the escape-radius squared (bailout value).
 * calcfracinit() reads bailout directly (fracsubr.c line 291: rqlim = bailout
 * when bailout != 0), so no pending pattern is needed — set it and restart.
 * Minimum meaningful value is 2 (radius sqrt(2) ≈ 1.41 > 1).
 */
EMSCRIPTEN_KEEPALIVE
void wasm_set_bailout(int val)
{
    if (val < 2) val = 2;      /* minimum meaningful bailout */
    bailout = (long)val;
    restart_requested = 1;     /* trigger recalculation */
    calc_status = 0;
    wasm_state  = WS_INIT_VIDEO;
}

EMSCRIPTEN_KEEPALIVE
int wasm_get_bailout(void)
{
    return (int)bailout;
}

/*
 * wasm_set_julia_params — set Julia set seed (C parameter).
 * param[0] = real part, param[1] = imaginary part.
 * Works for JULIAFP (6), JULIA (1), and other Julia-type fractals.
 */
EMSCRIPTEN_KEEPALIVE
void wasm_set_julia_params(double cre, double cim)
{
    param[0] = cre;
    param[1] = cim;
    restart_requested = 1;
    calc_status = 0;
    wasm_state  = WS_INIT_VIDEO;
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
