/*
 * ui.js — Loading overlay, controls panel wiring for Fractint WASM.
 *
 * Canvas rendering is handled by fractint-wasm.js which must load after this
 * file but before fractint.js.
 */

(function () {
  'use strict';

  var cycleActive = false;
  var cycleDir    = 1;

  /* Direct WASM function handles — set up after runtime is ready */
  var wasmToggleCycle   = null;
  var wasmSetCycleSpeed = null;
  var wasmSetFractype   = null;
  var wasmGetFractype   = null;

  /* ---------------------------------------------------------------- */
  /* Loading progress feedback                                        */
  /* ---------------------------------------------------------------- */

  window.FractintUI = {
    setProgress: function (pct) {
      var fill = document.getElementById('progress-fill');
      if (fill) fill.style.width = Math.min(100, pct) + '%';
    },
    setLoadingText: function (msg) {
      var el = document.getElementById('loading-text');
      if (el) el.textContent = msg;
    },
    hideOverlay: function () {
      var overlay = document.getElementById('loading-overlay');
      if (overlay) overlay.style.display = 'none';
      var canvas = document.getElementById('canvas');
      if (canvas) canvas.focus();
    }
  };

  /* ---------------------------------------------------------------- */
  /* Emscripten Module configuration hooks                            */
  /* ---------------------------------------------------------------- */

  window.Module = window.Module || {};

  window.Module.setStatus = function (text) {
    if (!text) return;
    var match = text.match(/(\d+)\/(\d+)/);
    if (match) {
      var pct = (parseInt(match[1]) / parseInt(match[2])) * 90;
      window.FractintUI.setProgress(pct);
    }
    window.FractintUI.setLoadingText(text || 'Loading...');
  };

  /* Preserve any onRuntimeInitialized already installed by keyboard.js */
  var prevOnInit = window.Module.onRuntimeInitialized || null;

  window.Module.onRuntimeInitialized = function () {
    window.FractintUI.setProgress(100);
    window.FractintUI.setLoadingText('Ready');
    setTimeout(function () {
      window.FractintUI.hideOverlay();
    }, 400);

    /* Wire up direct WASM controls */
    try {
      wasmToggleCycle   = Module.cwrap('wasm_toggle_cycle',    'void', ['number']);
      wasmSetCycleSpeed = Module.cwrap('wasm_set_cycle_speed', 'void', ['number']);
      wasmSetFractype   = Module.cwrap('wasm_set_fractype',    'void', ['number']);
      wasmGetFractype   = Module.cwrap('wasm_get_fractype',    'number', []);
    } catch (e) {
      console.warn('[FractintUI] WASM controls not available:', e);
    }

    if (prevOnInit) prevOnInit();
  };

  /* ---------------------------------------------------------------- */
  /* DOM ready: wire controls                                         */
  /* ---------------------------------------------------------------- */

  document.addEventListener('DOMContentLoaded', function () {

    var btnCycle     = document.getElementById('cycle-toggle');
    var btnDir       = document.getElementById('cycle-dir');
    var speedIn      = document.getElementById('cycle-speed');
    var btnReset     = document.getElementById('btn-reset');
    var btnZoomIn    = document.getElementById('btn-zoom-in');
    var btnZoomOut   = document.getElementById('btn-zoom-out');
    var btnSave      = document.getElementById('btn-save');
    var fracTypeSelect = document.getElementById('fractal-type');

    if (speedIn) {
      speedIn.addEventListener('input', function () {
        var speed = parseInt(this.value, 10);
        if (wasmSetCycleSpeed) {
          wasmSetCycleSpeed(speed);
        }
      });
    }

    if (btnCycle) {
      btnCycle.addEventListener('click', function () {
        if (wasmToggleCycle) {
          /* Pass direction when starting, 0 when stopping */
          wasmToggleCycle(cycleActive ? 0 : cycleDir);
        } else {
          /* Fallback: key injection if WASM not yet ready */
          if (window.FractintKeyboard) {
            window.FractintKeyboard.pushKey('c'.charCodeAt(0));
          }
        }
        cycleActive = !cycleActive;
        this.textContent = cycleActive ? '\u25A0 Stop' : '\u25BA Cycle';
        this.classList.toggle('active', cycleActive);
        if (btnDir) {
          btnDir.textContent = cycleActive
            ? (cycleDir > 0 ? '\u2192 Cycling FWD' : '\u2190 Cycling REV')
            : (cycleDir > 0 ? '\u2192 FWD' : '\u2190 REV');
        }
      });
    }

    if (btnDir) {
      btnDir.addEventListener('click', function () {
        cycleDir = -cycleDir;
        this.textContent = cycleActive
          ? (cycleDir > 0 ? '\u2192 Cycling FWD' : '\u2190 Cycling REV')
          : (cycleDir > 0 ? '\u2192 FWD' : '\u2190 REV');
        if (cycleActive && wasmToggleCycle) {
          /* Toggle off then back on with new direction */
          wasmToggleCycle(0);        /* stop */
          wasmToggleCycle(cycleDir); /* restart with new direction */
        }
      });
    }

    if (btnReset) {
      btnReset.addEventListener('click', function () {
        /* HOME key (1071) resets the Fractint view */
        if (window.FractintKeyboard) window.FractintKeyboard.pushKey(1071);
      });
    }

    if (btnZoomIn) {
      btnZoomIn.addEventListener('click', function () {
        /* PAGE_UP (1073) zooms in */
        if (window.FractintKeyboard) window.FractintKeyboard.pushKey(1073);
      });
    }

    if (btnZoomOut) {
      btnZoomOut.addEventListener('click', function () {
        /* PAGE_DOWN (1081) zooms out */
        if (window.FractintKeyboard) window.FractintKeyboard.pushKey(1081);
      });
    }

    if (fracTypeSelect) {
      fracTypeSelect.addEventListener('change', function () {
        var type = parseInt(this.value, 10);
        if (wasmSetFractype) {
          wasmSetFractype(type);
        } else if (typeof Module !== 'undefined' && Module.ccall) {
          Module.ccall('wasm_set_fractype', 'void', ['number'], [type]);
        }
      });
    }

    if (btnSave) {
      btnSave.addEventListener('click', function () {
        var c = document.getElementById('canvas');
        if (c) {
          var link = document.createElement('a');
          link.download = 'fractint.png';
          link.href = c.toDataURL('image/png');
          link.click();
        }
      });
    }
  });
}());
