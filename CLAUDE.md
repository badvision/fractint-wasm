# Fractint WASM — Developer Notes

## Build

```bash
make          # builds web/ (fractint.wasm, fractint.js, fractint.data)
make deploy   # copies src/index.html, src/css/, src/js/ into web/
```

Emscripten toolchain required. Must be compiled with:
- `-s USE_PTHREADS=1 -s PTHREAD_POOL_SIZE=4` — pthreads are load-bearing; do NOT remove
- `-s SIMD=1` — SIMD Mandelbrot path
- **Never add `-s ASYNCIFY=1`** — ASYNCIFY is incompatible with USE_PTHREADS=1; confirmed broken

GitHub Pages deploys from the `gh-pages` branch (`/` root). To publish:
```bash
# copy web/ contents into a gh-pages checkout, commit, push
```
There is no CI automation for this yet.

---

## Architecture

### Thread model

The browser main thread runs the Emscripten event loop (`emscripten_set_main_loop`). Four pre-spawned pthreads handle SIMD parallel Mandelbrot rendering. A fifth pthread is spawned on demand for curses menu interactions.

**wasm_state** (in `src/driver/d_wasm.c`) is a plain enum that encodes what the main thread is doing:

| State | Meaning |
|---|---|
| `WS_INIT` | startup |
| `WS_CALCFRAC` | single-threaded serial render |
| `WS_PAR_PRODUCE` | distributing rows to worker queue |
| `WS_PAR_WAIT` | waiting for parallel workers to finish |
| `WS_DONE` | idle, accepting input |
| `WS_COLOR_CYCLE` | palette cycling |
| `WS_MENU` | menu pthread active |

`wasm_state` is read only by the main thread; it is not atomic. This is safe today but should be commented clearly for future maintainers.

### Parallel render path (`d_wasm.c`)

- Producer-consumer `PixelQueue` distributes row ranges to 4 workers
- `PWorkerCtx` snapshot is taken at `pworkers_start()` — workers read immutable copies of `maxit`, `inside`, color tables, `scr_w`, and `bailout`
- `pworkers_abort()` is called before spawning a menu thread (if a parallel render is in progress) and on user-triggered zoom/pan interrupts
- SIMD path uses `wasm_f64x2_splat((double)ctx->bailout)` — **do not hardcode 4.0**; the bailout is user-configurable

### Path A: menu pthread (`src/driver/d_wasm.c`, `src/include/wasm_key.h`)

Fractint's curses menus call `waitkeypressed()` in a loop. In a browser, any blocking loop on the main thread freezes rendering. ASYNCIFY would solve this but breaks pthreads. Solution: run the menu on a dedicated pthread.

**Components:**

- `KeyChannel` (`wasm_key.h`) — mutex + condvar ring buffer. Menu pthread blocks in `key_channel_pop_blocking()`. Main thread pushes via `key_channel_push()`.
- `wasm_menu_active` (`_Atomic int`) — routing flag. `wasm_push_key()` routes to `KeyChannel` when 1, else to the main `key_ring[]`.
- `wasm_open_menu(int key)` — exported. Called by `keyboard.js` for menu keys (x/z/t/y/SPACE). Aborts any in-progress parallel render, spawns `menu_thread_func`.
- `menu_thread_func` — sets `text_mode_active = 1` directly (bypasses `stackscreen()`/`unstackscreen()` which are unsafe from a pthread). Calls `main_menu_switch()` loop. Clears text mode and sets `menu_done` on exit.
- `WS_MENU` state — render loop polls `menu_done` each frame. Includes a 300-frame (~5s) watchdog: if `menu_done` is not set, force-recovers to `WS_DONE` to prevent keyboard lockout from any future menu thread crash.
- `text_buf_back[]` + `_Atomic int text_buf_gen` — double-buffer. Menu pthread writes to `text_buf_back`; `text_buf_flush()` atomically copies to the front buffer on `wrefresh()`. JS uses `Atomics.load` on the gen counter to detect torn frames (`textoverlay.js`).

**Why not `stackscreen()`/`unstackscreen()`:** These call `newwin()`, `savecurses()`, etc. — curses window allocation is not pthread-safe in this build. The menu thread sets `text_mode_active` directly instead.

### Help system

`init_help()` is a no-op in WASM builds (`#ifdef WASM_BUILD return 0`) — there is no `.hlp` file. This leaves the static `label` pointer in `common/help.c` as NULL. Any menu path that reaches `read_help_topic()` will crash unless guarded.

**Fix in place:** `read_help_topic()` has `if (label == NULL || help_file == -1) return -len;` at the top. This makes help lookups silently return empty rather than crashing. **Do not remove this guard.**

---

## WASM exports (`d_wasm.c`)

All exported functions are listed in `EXPORTED_FUNCTIONS` in the Makefile.

| Export | Description |
|---|---|
| `wasm_set_maxit(int)` | pending_maxit pattern — calcfracinit picks it up |
| `wasm_get_maxit()` | current maxit |
| `wasm_set_fractype(int)` | sets fractype + restart |
| `wasm_get_fractype()` | current fractype index |
| `wasm_set_inside(int)` / `wasm_get_inside()` | inside coloring mode |
| `wasm_set_outside(int)` / `wasm_get_outside()` | outside coloring mode |
| `wasm_set_bailout(int)` / `wasm_get_bailout()` | escape radius squared; default 4 (radius 2) |
| `wasm_set_julia_params(double, double)` | sets param[0]/param[1] |
| `wasm_get_julia_re()` / `wasm_get_julia_im()` | param[0]/param[1] getters |
| `wasm_set_coords(x0,x1,y0,y1)` | pending_coords pattern |
| `wasm_get_xxmin/xxmax/yymin/yymax()` | current coordinate corners |
| `wasm_set_palette_preset(int)` | 6 built-in palettes (0=default … 5=classic) |
| `wasm_open_menu(int key)` | spawns menu pthread for the given key |
| `wasm_push_key(int)` | routes key to KeyChannel or main key_ring |
| `wasm_get_text_gen()` | atomic generation counter for text overlay |
| `wasm_is_text_mode()` | 1 when curses text overlay is active |
| `wasm_is_cycling()` | 1 when in WS_COLOR_CYCLE state |
| `wasm_consume_dirty()` | returns 1 and clears dirty flag if a new frame is ready |

---

## URL hash / state sharing (`src/js/urlshare.js`)

Format: `#base64(JSON)` where JSON keys are:

```
{ t, x0, x1, y0, y1, i, in, out, b [, p0, p1] }
```

- `p0`/`p1` (Julia seed) only present for Julia-family fractal types `{1, 6, 108, 109}`
- `b` = bailout value
- Dispatches `fractinthashchange` after every `replaceState`/`pushState`
- Dispatches `fractinstaterestored` after hash restore — `ui.js` syncs all controls from it

Back/forward browser navigation is supported via `popstate`.

---

## JS modules (`src/js/`)

| File | Purpose |
|---|---|
| `ui.js` | Control panel wiring, fractype selector, Julia seed panel, cycle button |
| `keyboard.js` | DOM keydown → Fractint scan codes; MENU_KEYS table routes x/z/t/y/SPACE to `wasm_open_menu` |
| `textoverlay.js` | Renders `text_buf` onto a 2D canvas overlay; generation-counter guard with `Atomics.load` |
| `urlshare.js` | URL hash encode/decode, share button, popstate handler |
| `qrwidget.js` | QR code panel, updates on `fractinthashchange` |

---

## Known tech debt / unimplemented ideas

- **Palette preset not in URL hash** — `dacbox` is modified by `spindac` during palette cycling, making a preset index stale immediately. Requires either snapshotting the full palette (expensive) or a separate "last preset applied" index that gets invalidated on any manual palette edit.
- **`key_ring[]` has no mutex** — not a real race today because `wasm_push_key` and `key_pop` both execute on the browser main thread when the menu is not active. Worth a comment to prevent future regressions.
- **`wasm_state` is non-atomic** — safe today (only main thread reads/writes it), but undocumented. A comment near the declaration would help future maintainers.
- **'e' (palette editor), 'r' (restore), 's' (save), F1 (help) keys** — not tested. These menu paths may have additional dependencies (file I/O, X11 pixmap ops) that need WASM stubs or guards similar to the help.c null guard.
- **WS_MENU watchdog is one-shot** — if the menu thread crashes, the watchdog recovers keyboard input after ~5s, but the pthread is leaked (never joined). A crashed menu pthread should be detached or tracked.
- **No CI for gh-pages deployment** — publishing to GitHub Pages is a manual copy-and-push step. A GitHub Actions workflow triggered on push to `main` would prevent the deployed version from drifting behind.
- **Playwright test coverage gaps** — x/t/y/SPACE menu keys are not yet tested; only Z is covered. The cycle button desync regression also lacks a dedicated test.
