/*
 * textoverlay.js — Renders Fractint's 25x80 text buffer as a canvas overlay.
 *
 * When Fractint enters text mode (menus, help, dialogs) it writes characters
 * and CGA colour attributes to a 25x80 uint16_t buffer in WASM memory.
 * This module reads that buffer every animation frame, re-renders it onto a
 * transparent canvas positioned above the fractal canvas, and shows/hides it
 * based on the text_mode_active flag from WASM.
 *
 * CGA attribute byte layout:
 *   bits [3:0] = foreground colour index (0-15)
 *   bits [6:4] = background colour index (0-7)
 *   bit    [7] = blink (ignored here)
 */

(function () {
  'use strict';

  /* CGA 16-colour palette */
  var CGA_COLORS = [
    '#000000', /* 0  black         */
    '#0000AA', /* 1  dark blue     */
    '#00AA00', /* 2  dark green    */
    '#00AAAA', /* 3  dark cyan     */
    '#AA0000', /* 4  dark red      */
    '#AA00AA', /* 5  dark magenta  */
    '#AA5500', /* 6  brown         */
    '#AAAAAA', /* 7  light grey    */
    '#555555', /* 8  dark grey     */
    '#5555FF', /* 9  bright blue   */
    '#55FF55', /* 10 bright green  */
    '#55FFFF', /* 11 bright cyan   */
    '#FF5555', /* 12 bright red    */
    '#FF55FF', /* 13 bright magenta*/
    '#FFFF55', /* 14 bright yellow */
    '#FFFFFF'  /* 15 white         */
  ];

  var TEXT_ROWS = 25;
  var TEXT_COLS = 80;

  /* Character cell dimensions in canvas pixels.
   * 9x16 gives a classic 720x400 CGA/VGA text look. */
  var CHAR_W = 9;
  var CHAR_H = 16;

  var textCanvas = null;
  var textCtx    = null;

  /* Cached cwrap handles — set after WASM runtime is ready */
  var fnGetTextBuf        = null;
  var fnIsTextMode        = null;
  var fnConsumeTextDirty  = null;

  function initTextOverlay() {
    textCanvas = document.createElement('canvas');
    textCanvas.id    = 'text-overlay';
    textCanvas.width  = TEXT_COLS * CHAR_W;   /* 720 */
    textCanvas.height = TEXT_ROWS * CHAR_H;   /* 400 */

    /* Overlay sits above the fractal canvas, scaled to container size. */
    textCanvas.style.cssText = [
      'position:absolute',
      'top:0',
      'left:0',
      'width:100%',
      'height:100%',
      'display:none',
      'pointer-events:none',  /* pass mouse clicks through when visible */
      'image-rendering:pixelated',
      'z-index:5'
    ].join(';');

    var container = document.getElementById('canvas-container');
    if (container) {
      container.appendChild(textCanvas);
    } else {
      document.body.appendChild(textCanvas);
    }

    textCtx = textCanvas.getContext('2d');
    textCtx.font         = CHAR_W + 'px monospace';
    textCtx.textBaseline = 'top';

    /* Wire up WASM exports */
    try {
      fnGetTextBuf       = Module.cwrap('wasm_get_text_buf',       'number', []);
      fnIsTextMode       = Module.cwrap('wasm_is_text_mode',       'number', []);
      fnConsumeTextDirty = Module.cwrap('wasm_consume_text_dirty', 'number', []);
    } catch (e) {
      console.warn('[TextOverlay] WASM exports not available:', e);
    }
  }

  function renderTextBuf() {
    if (!textCtx || !fnGetTextBuf || !Module.HEAPU16) return;

    var ptr = fnGetTextBuf();
    if (!ptr) return;

    /* HEAPU16 index = byte-offset / 2 */
    var base = ptr >>> 1;
    var buf  = Module.HEAPU16.subarray(base, base + TEXT_ROWS * TEXT_COLS);

    for (var row = 0; row < TEXT_ROWS; row++) {
      for (var col = 0; col < TEXT_COLS; col++) {
        var cell = buf[row * TEXT_COLS + col];
        var ch   = cell & 0xFF;
        var attr = (cell >>> 8) & 0xFF;
        var fg   = attr & 0x0F;
        var bg   = (attr >>> 4) & 0x07;  /* only 3 bits for bg */

        var x = col * CHAR_W;
        var y = row * CHAR_H;

        /* Background fill */
        textCtx.fillStyle = CGA_COLORS[bg];
        textCtx.fillRect(x, y, CHAR_W, CHAR_H);

        /* Character glyph — only printable ASCII */
        if (ch >= 32 && ch < 127) {
          textCtx.fillStyle = CGA_COLORS[fg];
          textCtx.fillText(String.fromCharCode(ch), x, y);
        }
      }
    }
  }

  function textOverlayTick() {
    if (!fnIsTextMode || !fnConsumeTextDirty) return;

    var inText = fnIsTextMode();

    if (textCanvas) {
      var shouldShow = inText ? 'block' : 'none';
      if (textCanvas.style.display !== shouldShow) {
        textCanvas.style.display = shouldShow;
      }
    }

    if (inText && fnConsumeTextDirty()) {
      renderTextBuf();
    }
  }

  /* Public API — consumed by fractint-wasm.js after runtime init */
  window.FractintText = {
    init: initTextOverlay,
    tick: textOverlayTick
  };

}());
