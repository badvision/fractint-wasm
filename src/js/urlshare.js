/*
 * urlshare.js — URL hash encoding/decoding of fractal state.
 *
 * Encodes the current fractal parameters (type, coordinate corners,
 * max iterations) as base64(JSON) in the URL hash so the view can be
 * bookmarked and shared.
 *
 * Hash format: #<base64(JSON)>
 * JSON keys: { t, x0, x1, y0, y1, i }
 *   t  — fractal type index
 *   x0 — xxmin
 *   x1 — xxmax
 *   y0 — yymin
 *   y1 — yymax
 *   i  — maxit (max iterations)
 */

(function () {
  'use strict';

  /* Cached cwrap handles */
  var fnGetXxmin   = null;
  var fnGetXxmax   = null;
  var fnGetYymin   = null;
  var fnGetYymax   = null;
  var fnGetMaxit   = null;
  var fnGetFractype = null;
  var fnSetFractype = null;
  var fnResize      = null;

  /* ---------------------------------------------------------------- */
  /* Encode current state to URL hash                                 */
  /* ---------------------------------------------------------------- */

  function updateUrlHash() {
    if (!fnGetXxmin) return;
    try {
      var params = {
        t:  fnGetFractype(),
        x0: fnGetXxmin(),
        x1: fnGetXxmax(),
        y0: fnGetYymin(),
        y1: fnGetYymax(),
        i:  fnGetMaxit()
      };
      window.location.hash = btoa(JSON.stringify(params));
    } catch (e) {
      console.warn('[URLShare] encode error:', e);
    }
  }

  /* ---------------------------------------------------------------- */
  /* Decode URL hash and restore fractal state                        */
  /* ---------------------------------------------------------------- */

  function loadFromUrlHash() {
    var hash = window.location.hash.slice(1);
    if (!hash) return false;
    try {
      var params = JSON.parse(atob(hash));

      /* Validate required numeric fields */
      if (typeof params.x0 !== 'number' || typeof params.x1 !== 'number' ||
          typeof params.y0 !== 'number' || typeof params.y1 !== 'number') {
        return false;
      }

      if (typeof params.t === 'number' && fnSetFractype) {
        fnSetFractype(params.t);
      }

      /* Set coordinate corners — wasm_set_coords accepts 4 doubles */
      Module.ccall('wasm_set_coords', 'void',
                   ['number','number','number','number'],
                   [params.x0, params.x1, params.y0, params.y1]);

      return true;
    } catch (e) {
      /* Silently ignore invalid or missing hash */
      return false;
    }
  }

  /* ---------------------------------------------------------------- */
  /* Share button handler                                             */
  /* ---------------------------------------------------------------- */

  function wireShareButton() {
    var shareBtn = document.getElementById('btn-share');
    if (!shareBtn) return;

    shareBtn.addEventListener('click', function () {
      updateUrlHash();
      var url = window.location.href;
      if (navigator.clipboard && navigator.clipboard.writeText) {
        navigator.clipboard.writeText(url).then(function () {
          var orig = shareBtn.textContent;
          shareBtn.textContent = 'Copied!';
          setTimeout(function () { shareBtn.textContent = orig; }, 2000);
        }).catch(function () {
          /* Clipboard not available — URL is already in address bar */
        });
      }
    });
  }

  /* ---------------------------------------------------------------- */
  /* Init — called after WASM runtime is ready                        */
  /* ---------------------------------------------------------------- */

  function initUrlShare() {
    try {
      fnGetXxmin    = Module.cwrap('wasm_get_xxmin',   'number', []);
      fnGetXxmax    = Module.cwrap('wasm_get_xxmax',   'number', []);
      fnGetYymin    = Module.cwrap('wasm_get_yymin',   'number', []);
      fnGetYymax    = Module.cwrap('wasm_get_yymax',   'number', []);
      fnGetMaxit    = Module.cwrap('wasm_get_maxit',   'number', []);
      fnGetFractype = Module.cwrap('wasm_get_fractype','number', []);
      fnSetFractype = Module.cwrap('wasm_set_fractype','void',   ['number']);
    } catch (e) {
      console.warn('[URLShare] WASM exports not available:', e);
      return;
    }

    wireShareButton();

    /* Load state from hash on startup (before first render) */
    loadFromUrlHash();
  }

  /* ---------------------------------------------------------------- */
  /* Auto-update hash when fractal finishes calculating               */
  /* ---------------------------------------------------------------- */

  /* Poll for WS_DONE by watching pixel buffer stability.
   * We use a simple approach: update hash after each zoom/pan operation.
   * The updateUrlHash function is exposed globally so ui.js can call it. */

  window.FractintShare = {
    init:          initUrlShare,
    updateUrlHash: updateUrlHash,
    loadFromHash:  loadFromUrlHash
  };

}());
