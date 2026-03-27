# Fractint WebAssembly

A WebAssembly port of [Fractint](https://www.fractint.org/) — the legendary DOS fractal generator, running entirely in your browser.

**[Launch the App](https://badvision.github.io/fractint-wasm/)** &nbsp;|&nbsp; **[Documentation](https://badvision.github.io/fractint-wasm/docs/)**

---

## About Fractint

Fractint is one of the most comprehensive fractal generation programs ever written. Originally created for DOS by the **Stone Soup Group**, it supports hundreds of fractal types, deep zoom, color cycling, formula parsing, and much more. The program has been continuously developed since 1988.

- **Official Website:** [fractint.org](https://www.fractint.org/)
- **FTP Archive:** [fractint.net/ftp/current/](https://www.fractint.net/ftp/current/)
- **Source used for this port:** xfractint 20.04p16 — the Linux/Unix port of Fractint

## Authors and Credits

Fractint was created by **The Stone Soup Group**, including (among many others):

- **Bert Tyler** — original author and primary developer
- **Timothy Wegner** — major contributor, co-author of the *Fractal Creations* book
- **Mark Peterson** — formula parser, many fractal types
- **Pieter Branderhorst** — speed optimizations, many features
- **Robin Bussell** — 3D transforms
- **Michael Kaufman** — GIF encoder/decoder
- **Jonathan Osuch** — many fractal types
- **Wes Loewer** — boundary tracing, tesseral guessing
- ...and dozens of other contributors listed in the Fractint documentation

The xfractint Unix port was maintained by **Ken Shirriff**, **Tim Wegner**, **Sebastiano Vigna**, and others.

The original Fractint source code is available at:
- GitHub mirror: [LegalizeAdulthood/fractint-legacy](https://github.com/LegalizeAdulthood/fractint-legacy)
- Official archive: [fractint.net/ftp/current/](https://www.fractint.net/ftp/current/)

Fractint is released under the **Stone Soup License** — free to use, share, and modify with attribution.

## This Port

This WebAssembly port compiles the xfractint 20.04p16 C source using **Emscripten**, replacing the X11 display layer with a custom Canvas 2D rendering driver. The fractal calculation engine, color cycling, formula parser, and all fractal mathematics are the original Fractint C code running as WebAssembly at near-native speed.

### Technical Approach

- **Source:** xfractint-20.04p16 (Linux port with full C fallbacks)
- **Compiler:** Emscripten (C to WebAssembly)
- **Display:** Custom `src/driver/d_wasm.c` replacing X11 with Canvas 2D
- **Rendering:** Palette-indexed pixel buffer with RGBA LUT (256-color VGA emulation)
- **Color cycling:** Per-frame `spindac()` call with frame-skip speed control
- **Main loop:** `emscripten_set_main_loop` state machine (INIT → CALC → DONE → CYCLE)

### Available Fractal Types

| Type | Description |
|------|-------------|
| Mandelbrot (FP) | Classic Mandelbrot set, floating-point |
| Mandelbrot (int) | Mandelbrot set, integer arithmetic |
| Julia (FP) | Julia set, floating-point |
| Julia (int) | Julia set, integer arithmetic |
| Newton | Newton's method fractals |
| Newton Basin | Newton basins of attraction |
| Complex Newton | Complex Newton fractals |
| Sierpinski (FP) | Sierpinski triangle, floating-point |
| Sierpinski (int) | Sierpinski triangle, integer |
| Barnsley M1 (FP) | Barnsley Mandelbrot variant 1 |
| Barnsley M1 (int) | Barnsley Mandelbrot variant 1, integer |
| Lorenz (FP) | Lorenz strange attractor |
| Marks Mandelbrot (FP) | Marks Mandelbrot variant |

### Key Controls

| Key | Action |
|-----|--------|
| `+` or `C` | Start color cycling |
| `-` | Reverse color cycling direction |
| `Spacebar` | Stop color cycling |
| `PageUp` | Zoom in |
| `PageDown` | Zoom out |
| Arrow keys | Pan view |
| `Home` | Reset to default view |
| `Enter` | Confirm action |
| `Escape` | Stop current calculation |
| `F1` | Help (Fractint built-in help system) |
| `S` | Save image (GIF format via Fractint) |
| `T` | Change fractal type (via Fractint menu) |

Click the canvas once to give it keyboard focus, then use any of the above keys.

## Building from Source

Requirements:
- [Emscripten SDK](https://emscripten.org/docs/getting_started/downloads.html)
- `make`

```bash
git clone https://github.com/badvision/fractint-wasm.git
cd fractint-wasm
source ~/emsdk/emsdk_env.sh   # activate Emscripten
emmake make
# Output: web/fractint.js, web/fractint.wasm, web/fractint.data
emmake make deploy
# Copies static assets to web/ for testing
```

To serve locally:
```bash
cd web && python3 -m http.server 8080
# Open http://localhost:8080
```

## Project Structure

```
fractint-wasm/
├── src/
│   ├── driver/d_wasm.c     # WASM Canvas 2D display driver (replaces X11)
│   ├── include/            # WASM-safe header overrides
│   ├── js/                 # JavaScript frontend (keyboard, UI, WASM bridge)
│   ├── css/                # Stylesheet
│   └── index.html          # App HTML template
├── web/                    # Build output + deployable app
│   ├── fractint.js         # Emscripten glue code
│   ├── fractint.wasm       # Compiled WebAssembly binary
│   ├── fractint.data       # Preloaded fractal data files
│   ├── js/                 # Copied from src/js/
│   ├── css/                # Copied from src/css/
│   └── index.html          # App entry point
├── xfractint-20.04p16/     # Upstream xfractint source (unmodified)
├── data/                   # Fractal parameter files, color maps, formulas
├── docs/                   # Documentation website
└── Makefile                # Emscripten build
```

## License

The Fractint source code in `xfractint-20.04p16/` is covered by the **Stone Soup License** — see `fractint.doc` in the original distribution. The Stone Soup License is permissive: free to use, modify, and distribute with attribution.

The platform layer for this port (`src/driver/d_wasm.c`, `src/js/`, `src/css/`) is released under the **MIT License**.
