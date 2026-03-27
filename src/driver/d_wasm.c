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

/* Screen pixel buffer — 8-bit palette indices */
static BYTE *screen_pixels = NULL;
static int screen_w = 0;
static int screen_h = 0;

/*
 * rgba_lut[i] = RGBA for palette index i.
 * Stored as little-endian Uint32 = 0xAABBGGRR so that a Uint32Array
 * view of ImageData.data paints the correct colour.
 * Built by writevideopalette() from dacbox[256][3] (6-bit VGA values).
 */
static Uint32 rgba_lut[256];

/* dacbox is defined in unix/general.c */
extern unsigned char dacbox[256][3];

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
    if (screen_pixels == NULL && screen_w > 0 && screen_h > 0) {
        screen_pixels = (BYTE *)calloc((size_t)(screen_w * screen_h), 1);
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
 */
int xgetkey(int block)
{
    (void)block;
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
    WS_COLOR_CYCLE
} WasmState;

static WasmState wasm_state = WS_INIT_VIDEO;

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
 * wasm_pan — shift the view by (dx_frac, dy_frac) of the current width/height.
 * Positive dx_frac pans right; positive dy_frac pans down.
 */
static void wasm_pan(double dx_frac, double dy_frac)
{
    double dx = (xxmax - xxmin) * dx_frac;
    double dy = (yymax - yymin) * dy_frac;
    xxmin += dx;  xxmax += dx;  xx3rd += dx;
    yymin += dy;  yymax += dy;  yy3rd += dy;
    sxmin = xxmin;  sxmax = xxmax;
    symin = yymin;  symax = yymax;
    calc_status = 0;
    wasm_state  = WS_INIT_VIDEO;
}

static void wasm_main_loop_callback(void)
{
    switch (wasm_state) {

    case WS_INIT_VIDEO:
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

        wasm_state = WS_CALCFRAC;
        break;

    case WS_CALCFRAC:
        {
            int result = calcfract();
            if (result == 0 || calc_status == 4) {
                writevideopalette();
                wasm_state = WS_DONE;
            }
            /* result != 0 means interrupted; keep calling next frame */
        }
        break;

    case WS_DONE:
        {
            /* Process navigation/cycling keys once per frame.
             * Key codes follow Fractint/DOS conventions:
             *   PAGE_UP=1073, PAGE_DOWN=1081, HOME=1071
             *   LEFT=1075, RIGHT=1077, UP=1072, DOWN=1080
             */
            int key = xgetkey(0);
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

                /* Pan: arrow keys shift view by 20% of current extent */
                case 1075: /* LEFT_ARROW */
                    wasm_pan(-0.2, 0.0);
                    break;
                case 1077: /* RIGHT_ARROW */
                    wasm_pan(0.2, 0.0);
                    break;
                case 1072: /* UP_ARROW */
                    wasm_pan(0.0, -0.2);
                    break;
                case 1080: /* DOWN_ARROW */
                    wasm_pan(0.0, 0.2);
                    break;

                /* Home: reset view to default for current fractal type */
                case 1071: /* HOME */
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
        if (xgetkey(0) != 0) {
            wasm_state = WS_DONE;
        }
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

/* ------------------------------------------------------------------ */
/* xfcurses.c replacement — fake curses text layer                    */
/* ------------------------------------------------------------------ */

static WINDOW wasm_win = {0, 0, 0, 0, 25, 80, 0, NULL, NULL};
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
void clear(void)   { }
int  standout(void){ return 0; }
int  standend(void){ return 0; }

void wmove(WINDOW *win, int y, int x)
{
    if (!win) return;
    win->_cur_y = y;
    win->_cur_x = x;
}

void waddch(WINDOW *win, const chtype ch)  { }
void waddstr(WINDOW *win, char *str)       { }
void wclear(WINDOW *win)                   { }
void wdeleteln(WINDOW *win)                { }
void winsertln(WINDOW *win)                { }
void wrefresh(WINDOW *win)                 { }
void xrefresh(WINDOW *win, int l1, int l2) { }
void touchwin(WINDOW *win)                 { }
void wtouchln(WINDOW *win, int y, int n, int changed) { }
void wstandout(WINDOW *win)                { }
void wstandend(WINDOW *win)                { }
void delwin(WINDOW *win)                   { }
void mvcur(int or, int oc, int nr, int nc) { }
void refresh(int line1, int line2)         { }
void xpopup(char *str)                     { }
void set_margins(int width, int height)    { }

WINDOW *newwin(int nlines, int ncols, int begin_y, int begin_x)
{
    WINDOW *w = (WINDOW *)malloc(sizeof(WINDOW));
    if (!w) return NULL;
    memset(w, 0, sizeof(WINDOW));
    w->_num_y = nlines;
    w->_num_x = ncols;
    w->_cur_y = begin_y;
    w->_cur_x = begin_x;
    return w;
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
