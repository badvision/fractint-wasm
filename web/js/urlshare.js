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

  function currentParams() {
    return {
      t:  fnGetFractype(),
      x0: fnGetXxmin(),
      x1: fnGetXxmax(),
      y0: fnGetYymin(),
      y1: fnGetYymax(),
      i:  fnGetMaxit()
    };
  }

  /* Replace the current history entry with the present fractal state.
   * Called automatically when the render loop goes idle (active→idle edge).
   * Also called by the Share button to ensure the URL is current. */
  function updateUrlHash() {
    if (!fnGetXxmin) return;
    try {
      var params = currentParams();
      var hash = btoa(JSON.stringify(params));
      history.replaceState(params, '', '#' + hash);
    } catch (e) {
      console.warn('[URLShare] encode error:', e);
    }
  }

  /* Push a new history entry for the current fractal state.
   * Call this BEFORE triggering a zoom/pan/fractype change so the back
   * button returns to the previous view. */
  function pushHistoryState() {
    if (!fnGetXxmin) return;
    try {
      var params = currentParams();
      var hash = btoa(JSON.stringify(params));
      history.pushState(params, '', '#' + hash);
    } catch (e) {
      console.warn('[URLShare] pushState error:', e);
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
  /* popstate handler — back/forward button restores fractal state   */
  /* ---------------------------------------------------------------- */

  function onPopState(e) {
    var params = e.state;
    if (params && typeof params.x0 === 'number' && typeof params.x1 === 'number' &&
        typeof params.y0 === 'number' && typeof params.y1 === 'number') {
      /* State object stored by pushState/replaceState — use directly */
      try {
        if (typeof params.t === 'number' && fnSetFractype) {
          fnSetFractype(params.t);
        }
        Module.ccall('wasm_set_coords', 'void',
                     ['number','number','number','number'],
                     [params.x0, params.x1, params.y0, params.y1]);
      } catch (err) {
        console.warn('[URLShare] popstate restore error:', err);
      }
    } else {
      /* No state (e.g. initial page-load entry) — fall back to hash */
      loadFromUrlHash();
    }
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

    /* Back/forward navigation */
    window.addEventListener('popstate', onPopState);

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
    init:             initUrlShare,
    updateUrlHash:    updateUrlHash,
    pushHistoryState: pushHistoryState,
    loadFromHash:     loadFromUrlHash
  };

}());
