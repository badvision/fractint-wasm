/* calcmand.c
 * This file contains routines to replace calcmand.asm.
 *
 * This file Copyright 1991 Ken Shirriff.  It may be used according to the
 * fractint license conditions, blah blah blah.
 *
 * calcmandasm() — C implementation of the fast integer Mandelbrot/Julia
 * inner loop.  Replaces the DOS x86 assembly version for XFRACT/WASM builds.
 *
 * The function is called after calcmand() has loaded:
 *   linitx, linity  — pixel coords (c for Mandelbrot, initial z for Julia)
 *   lparm.x/.y      — initial z perturbation (0,0 for normal Mandelbrot)
 *   llimit          — integer bailout threshold (rqlim * fudge)
 *   maxit           — maximum iterations
 *   inside/outside  — special colour-mode flags
 * It must set coloriter, realcoloriter and return coloriter (>=0) on normal
 * exit, or -1 when the keyboard interrupt flag is raised.
 */

#include "port.h"
#include "prototyp.h"
#include "fractype.h"

extern long firstsavedand;
extern int  nextsavedincr;

unsigned long savedmask;
long linitx, linity;

#define ABS(x) ((x) < 0 ? -(x) : (x))

long
calcmandasm(void)
{
    long x, y, x2, y2, xy;     /* fixed-point working variables         */
    long Cx, Cy;                /* fixed-point c parameter               */
    long cx;                    /* iteration counter (counts down)       */
    long savedx, savedy;        /* periodicity saved values              */
    long savedand;
    int  savedincr;
    long tmpfsd;
    int  period_color;

    /* Set up inside colour for periodicity hits */
    if (inside < 0)
        period_color = maxit;
    else
        period_color = inside;

    /* Initialise periodicity state (mirrors calmanfp.c) */
    if (periodicitycheck == 0) {
        oldcoloriter = 0;
    } else if (reset_periodicity) {
        oldcoloriter = maxit - 255;
    }

    tmpfsd = maxit - firstsavedand;
    if (oldcoloriter > tmpfsd)
        oldcoloriter = tmpfsd;

    savedx    = 0;
    savedy    = 0;
    orbit_ptr = 0;
    savedand  = firstsavedand;
    savedincr = 1;

    /* Keyboard check throttle */
    kbdcount--;
    if (kbdcount < 0) {
        int key;
        kbdcount = 1000;
        key = keypressed();
        if (key) {
            if (key == 'o' || key == 'O') {
                getakey();
                show_orbit = 1 - show_orbit;
            } else {
                coloriter = -1;
                return -1;
            }
        }
    }

    cx = maxit;

    /*
     * For Mandelbrot types the pixel coordinate is c and the orbit
     * starts at z = lparm + c.  For Julia types Cx/Cy are the Julia
     * parameter and z starts at the pixel coordinate.
     */
    if (fractype != JULIA && fractype != JULIAFP) {
        /* Mandelbrot */
        Cx = linitx;
        Cy = linity;
        x  = lparm.x + Cx;
        y  = lparm.y + Cy;
    } else {
        /* Julia */
        Cx = lparm.x;
        Cy = lparm.y;
        x  = linitx;
        y  = linity;
        /* perform first squaring step as calmanfp.c does for Julia */
        x2 = multiply(x, x, bitshift);
        y2 = multiply(y, y, bitshift);
        xy = multiply(x, y, bitshiftless1);
        x  = x2 - y2 + Cx;
        y  = xy + Cy;
    }

    overflow = 0;
    x2 = multiply(x, x, bitshift);
    y2 = multiply(y, y, bitshift);
    xy = multiply(x, y, bitshiftless1);

    while (--cx > 0) {
        x  = x2 - y2 + Cx;
        y  = xy + Cy;
        overflow = 0;
        x2 = multiply(x, x, bitshift);
        y2 = multiply(y, y, bitshift);
        xy = multiply(x, y, bitshiftless1);

        /* Bail out on integer overflow or if magnitude exceeds limit.
         * x2+y2 can wrap negative on overflow, so check both conditions. */
        if (overflow || (x2 + y2) < 0 || (x2 + y2) >= llimit)
            goto over_bailout;

        /* Periodicity check */
        if (cx < oldcoloriter) {
            if (((maxit - cx) & savedand) == 0) {
                savedx = x;
                savedy = y;
                savedincr--;
                if (savedincr == 0) {
                    savedand  = (savedand << 1) + 1;
                    savedincr = nextsavedincr;
                }
            } else {
                if (ABS(savedx - x) < lclosenuff &&
                    ABS(savedy - y) < lclosenuff) {
                    oldcoloriter  = maxit;
                    realcoloriter = maxit;
                    kbdcount      = kbdcount - (maxit - cx);
                    coloriter     = period_color;
                    goto pop_stack;
                }
            }
        }

        if (show_orbit)
            iplot_orbit(x, y, -1);
    }

    /* Reached maxit — point is inside */
    oldcoloriter  = maxit;
    kbdcount     -= maxit;
    realcoloriter = maxit;
    coloriter     = period_color;

pop_stack:
    if (orbit_ptr)
        scrub_orbit();

    /* Store final lnew so calcmand() can use it if needed */
    lnew.x = x;
    lnew.y = y;

    return coloriter;

over_bailout:
    if (cx - 10 > 0)
        oldcoloriter = cx - 10;
    else
        oldcoloriter = 0;

    coloriter = realcoloriter = maxit - cx;
    if (coloriter == 0) coloriter = 1;
    kbdcount -= realcoloriter;

    lnew.x = x;
    lnew.y = y;

    goto pop_stack;
}

#if 0    /* not used */
code16bit() {}
checkperiod() {}
code32bit() {}
#endif
