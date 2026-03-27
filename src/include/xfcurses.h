/*
 * xfcurses.h - WASM override of the X11-dependent xfcurses.h
 *
 * The upstream headers/xfcurses.h hard-includes X11/Xlib.h which is
 * unavailable under Emscripten.  We place this file earlier in the
 * include path so it shadows the upstream version.
 *
 * It provides the same types and function declarations that video.c
 * and unixscr.c depend on, but without any X11 types.
 */

#ifndef WASM_XFCURSES_H
#define WASM_XFCURSES_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* Fake WINDOW type — matches xfcurses.c's _win_st definition         */
/* ------------------------------------------------------------------ */

struct _win_st {
    int  _cur_y, _cur_x;
    int  _car_y, _car_x;
    int  _num_y, _num_x;
    int  _cur_attr;
    char *_text;
    short *_attr;
};

#define WINDOW struct _win_st
#define stdscr NULL

typedef unsigned chtype;

/* ------------------------------------------------------------------ */
/* X11 opaque stubs — declared as void* so code compiles              */
/* ------------------------------------------------------------------ */

/* These are referenced in xfcurses.h externs but we just use void* */
typedef void *Display;
typedef unsigned long Window;
typedef void *GC;
typedef void *Visual;
typedef void *Screen;
typedef unsigned long Colormap;
typedef unsigned long Atom;
typedef void *XImage;

/* extern globals that d_wasm.c defines */
extern Display *Xdp;
extern Window   Xw;
extern Window   Xwc;
extern Window   Xwp;
extern Window   Xroot;
extern GC       Xgc;
extern Visual  *Xvi;
extern Screen  *Xsc;
extern Colormap Xcmap;
extern int      Xdscreen;
extern int      Xdepth;
extern char    *Xmessage;
extern char    *Xdisplay;
extern char    *Xgeometry;
extern char    *Xfontname;
extern char    *Xfontnamebold;
extern Atom     wm_delete_window;
extern Atom     wm_protocols;

extern int COLS;
extern int LINES;

extern void Open_XDisplay(void);

/* ------------------------------------------------------------------ */
/* Curses-like function declarations                                   */
/* ------------------------------------------------------------------ */

extern void cbreak(void);
extern void nocbreak(void);
extern void echo(void);
extern void noecho(void);
extern void clear(void);
extern int  standout(void);
extern int  standend(void);
extern void endwin(void);
extern void refresh(int line1, int line2);
extern void xpopup(char *str);
extern void mvcur(int oldrow, int oldcol, int newrow, int newcol);

extern void    delwin(WINDOW *win);
extern void    waddch(WINDOW *win, const chtype ch);
extern void    waddstr(WINDOW *win, char *str);
extern void    wclear(WINDOW *win);
extern void    wdeleteln(WINDOW *win);
extern void    winsertln(WINDOW *win);
extern void    wmove(WINDOW *win, int y, int x);
extern void    wrefresh(WINDOW *win);
extern void    xrefresh(WINDOW *win, int line1, int line2);
extern void    touchwin(WINDOW *win);
extern void    wtouchln(WINDOW *win, int y, int n, int changed);
extern void    wstandout(WINDOW *win);
extern void    wstandend(WINDOW *win);
extern WINDOW *newwin(int nlines, int ncols, int begin_y, int begin_x);
extern WINDOW *initscr(void);
extern void    set_margins(int width, int height);

#define getyx(win,y,x) (y = (win)->_cur_y, x = (win)->_cur_x)

/* pixel array referenced in video.c when !NCURSES */
extern unsigned long pixel[48];

/* XFlush — declared as int to match upstream X11 signature used in video.c */
extern int XFlush(void *display);

#endif /* WASM_XFCURSES_H */
