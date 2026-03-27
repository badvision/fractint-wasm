/*
 * palette.js — Palette rendering helpers for Fractint WASM
 *
 * Fractint stores its palette in dacbox[256][3] where each component is
 * in the range 0-63 (VGA DAC convention, not 0-255).  These helpers
 * convert and apply the palette to the SDL canvas.
 */

(function () {
  'use strict';

  /**
   * Convert a Fractint dacbox entry (0-63 per channel) to CSS hex color.
   * @param {number} r  Red   0-63
   * @param {number} g  Green 0-63
   * @param {number} b  Blue  0-63
   * @returns {string}  "#rrggbb"
   */
  function dacToHex(r, g, b) {
    function scale(v) {
      var n = Math.round((v * 255) / 63);
      return n.toString(16).padStart(2, '0');
    }
    return '#' + scale(r) + scale(g) + scale(b);
  }

  /**
   * Build a 256-entry Uint32Array RGBA palette from a flat Uint8Array that
   * mirrors dacbox[256][3].
   * @param {Uint8Array} dacbox  768 bytes: [r0,g0,b0, r1,g1,b1, ...]
   * @returns {Uint32Array}
   */
  function buildRGBA(dacbox) {
    var rgba = new Uint32Array(256);
    for (var i = 0; i < 256; i++) {
      var r = Math.round((dacbox[i * 3 + 0] * 255) / 63);
      var g = Math.round((dacbox[i * 3 + 1] * 255) / 63);
      var b = Math.round((dacbox[i * 3 + 2] * 255) / 63);
      /* ImageData pixels are RGBA in little-endian Uint32 = 0xAABBGGRR */
      rgba[i] = (0xFF << 24) | (b << 16) | (g << 8) | r;
    }
    return rgba;
  }

  /**
   * Render an 8-bit palette-indexed pixel buffer to a canvas ImageData.
   * @param {Uint8Array}  pixels   width * height bytes, palette indices
   * @param {Uint32Array} palette  256 RGBA entries (from buildRGBA)
   * @param {ImageData}   imgData  canvas ImageData to write into
   */
  function renderFrame(pixels, palette, imgData) {
    var data = new Uint32Array(imgData.data.buffer);
    for (var i = 0, n = pixels.length; i < n; i++) {
      data[i] = palette[pixels[i]];
    }
  }

  window.FractintPalette = {
    dacToHex:    dacToHex,
    buildRGBA:   buildRGBA,
    renderFrame: renderFrame
  };
}());
