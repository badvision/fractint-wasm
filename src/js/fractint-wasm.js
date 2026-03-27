/*
 * fractint-wasm.js — Emscripten Module configuration and render loop
 *
 * This file MUST be loaded before the Emscripten-generated fractint.js so
 * that the Module object is defined before the glue script reads it.
 *
 * Render pipeline:
 *   1. wasm_get_screen_dims() → packed (h<<16)|w
 *   2. wasm_get_pixel_buf()   → pointer to 8-bit palette-index array (w*h)
 *   3. wasm_get_rgba_lut()    → pointer to 256 uint32 RGBA entries
 *   4. JS unpacks and writes to canvas via ImageData
 */

/* ui.js may pre-seed window.Module with setStatus / onRuntimeInitialized;
 * merge rather than overwrite. */
window.Module = window.Module || {};

(function (M) {
  'use strict';

  /* ------------------------------------------------------------------ */
  /* Canvas reference                                                    */
  /* ------------------------------------------------------------------ */

  M.canvas = (function () {
    var c = document.getElementById('canvas');
    if (!c) {
      /* Fallback: create and attach if missing (should not happen). */
      c = document.createElement('canvas');
      c.id = 'canvas';
      c.setAttribute('tabindex', '0');
      var container = document.getElementById('canvas-container');
      if (container) {
        container.insertBefore(c, container.firstChild);
      } else {
        document.body.appendChild(c);
      }
    }
    return c;
  }());

  /* ------------------------------------------------------------------ */
  /* Emscripten status / progress hooks                                 */
  /* ------------------------------------------------------------------ */

  /* Preserve any setStatus already installed by ui.js */
  var prevSetStatus = M.setStatus || null;

  M.setStatus = function (text) {
    if (prevSetStatus) {
      prevSetStatus(text);
    } else if (text) {
      var el = document.getElementById('loading-text');
      if (el) el.textContent = text;
    }
  };

  /* ------------------------------------------------------------------ */
  /* Logging                                                             */
  /* ------------------------------------------------------------------ */

  M.print = function (text) {
    console.log('[Fractint]', text);
  };

  M.printErr = function (text) {
    console.error('[Fractint]', text);
  };

  /* ------------------------------------------------------------------ */
  /* Runtime initialized → start render loop                           */
  /* ------------------------------------------------------------------ */

  /* Preserve any onRuntimeInitialized already installed by ui.js */
  var prevOnInit = M.onRuntimeInitialized || null;

  M.onRuntimeInitialized = function () {
    console.log('[Fractint] WASM runtime initialized');

    /* Call ui.js handler first (hides loading overlay, etc.) */
    if (prevOnInit) {
      prevOnInit();
    }

    /* Get exported functions via cwrap */
    var getPixelBuf   = M.cwrap('wasm_get_pixel_buf',   'number', []);
    var getRgbaLut    = M.cwrap('wasm_get_rgba_lut',    'number', []);
    var getScreenDims = M.cwrap('wasm_get_screen_dims', 'number', []);
    var wasmResize    = M.cwrap('wasm_resize',           'void',   ['number', 'number']);

    var canvas = document.getElementById('canvas');
    var ctx = canvas.getContext('2d');

    /* Cache the last known dimensions to avoid re-allocating ImageData
     * every frame when the screen size hasn't changed. */
    var cachedW = 0;
    var cachedH = 0;
    var cachedImageData = null;
    var cachedDst = null;

    function renderFrame() {
      /* Guard: HEAPU8 / HEAPU32 are available after init, but check anyway. */
      if (!M.HEAPU8 || !M.HEAPU32) {
        requestAnimationFrame(renderFrame);
        return;
      }

      var dims = getScreenDims();
      var w = dims & 0xFFFF;
      var h = (dims >>> 16) & 0xFFFF;

      if (w <= 0 || h <= 0) {
        requestAnimationFrame(renderFrame);
        return;
      }

      var pixBufPtr = getPixelBuf();
      var lutPtr    = getRgbaLut();

      if (!pixBufPtr || !lutPtr) {
        requestAnimationFrame(renderFrame);
        return;
      }

      /* Resize canvas element only when dimensions actually change */
      if (w !== cachedW || h !== cachedH) {
        canvas.width  = w;
        canvas.height = h;
        cachedW = w;
        cachedH = h;
        cachedImageData = ctx.createImageData(w, h);
        cachedDst = new Uint32Array(cachedImageData.data.buffer);
      }

      /* Map WASM heap views onto pixel buffer and RGBA LUT */
      var pixelBuf = M.HEAPU8.subarray(pixBufPtr, pixBufPtr + w * h);
      /* lutPtr is a byte offset; HEAPU32 index = lutPtr >> 2 */
      var lutBase  = lutPtr >>> 2;
      var rgbaLut  = M.HEAPU32.subarray(lutBase, lutBase + 256);

      /* Palette expand: index → RGBA uint32 */
      var n = w * h;
      for (var i = 0; i < n; i++) {
        cachedDst[i] = rgbaLut[pixelBuf[i]];
      }

      ctx.putImageData(cachedImageData, 0, 0);

      requestAnimationFrame(renderFrame);
    }

    requestAnimationFrame(renderFrame);

    /* ---------------------------------------------------------------- */
    /* Dynamic canvas resize via ResizeObserver                         */
    /* ---------------------------------------------------------------- */

    var resizeCanvas = function () {
      var container = document.getElementById('canvas-container');
      if (!container) return;
      var w = container.clientWidth;
      var h = container.clientHeight;
      /* Snap to even numbers */
      w = Math.floor(w / 2) * 2;
      h = Math.floor(h / 2) * 2;
      if (w < 320 || h < 240) return;
      /* canvas.width/height are already synced by the render loop
       * when WASM reports new dims, but we must tell WASM the new size. */
      wasmResize(w, h);
    };

    /* Initial resize to fill the container */
    resizeCanvas();

    if (window.ResizeObserver) {
      var ro = new ResizeObserver(function () {
        clearTimeout(window._fractintResizeTimer);
        window._fractintResizeTimer = setTimeout(resizeCanvas, 150);
      });
      var container = document.getElementById('canvas-container');
      if (container) ro.observe(container);
    } else {
      window.addEventListener('resize', function () {
        clearTimeout(window._fractintResizeTimer);
        window._fractintResizeTimer = setTimeout(resizeCanvas, 150);
      });
    }
  };

}(window.Module));
