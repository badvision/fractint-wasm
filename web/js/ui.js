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
  var wasmZoomToRect    = null;
  var wasmZoomAtPoint   = null;

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
      wasmZoomToRect    = Module.cwrap('wasm_zoom_to_rect',    'void', ['number','number','number','number']);
      wasmZoomAtPoint   = Module.cwrap('wasm_zoom_at_point',   'void', ['number','number','number']);
    } catch (e) {
      console.warn('[FractintUI] WASM controls not available:', e);
    }

    initMouseZoom();

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
    /* btn-share is wired in urlshare.js after WASM init */

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

  /* ---------------------------------------------------------------- */
  /* Mouse zoom — initialised after WASM runtime is ready             */
  /* ---------------------------------------------------------------- */

  function initMouseZoom() {
    var canvas = document.getElementById('canvas');
    if (!canvas) return;
    if (!wasmZoomToRect || !wasmZoomAtPoint) return;

    var dragStart = null;
    var dragEnd   = null;

    /* Overlay canvas for rubber-band zoom box */
    var overlay = document.createElement('canvas');
    overlay.style.cssText = 'position:absolute;top:0;left:0;width:100%;height:100%;pointer-events:none;';
    overlay.id = 'zoom-overlay';
    canvas.parentElement.appendChild(overlay);
    var octx = overlay.getContext('2d');

    function syncOverlay() {
      overlay.width  = canvas.offsetWidth;
      overlay.height = canvas.offsetHeight;
    }
    syncOverlay();

    /* Map a MouseEvent to canvas pixel coordinates */
    function canvasCoords(e) {
      var rect   = canvas.getBoundingClientRect();
      var scaleX = canvas.width  / rect.width;
      var scaleY = canvas.height / rect.height;
      return {
        x: Math.round((e.clientX - rect.left) * scaleX),
        y: Math.round((e.clientY - rect.top)  * scaleY)
      };
    }

    function drawZoomBox(x1, y1, x2, y2) {
      syncOverlay();
      octx.clearRect(0, 0, overlay.width, overlay.height);
      var sx = overlay.width  / canvas.width;
      var sy = overlay.height / canvas.height;
      octx.strokeStyle = 'rgba(255,255,255,0.9)';
      octx.lineWidth   = 1.5;
      octx.setLineDash([4, 4]);
      octx.strokeRect(
        x1 * sx, y1 * sy,
        (x2 - x1) * sx, (y2 - y1) * sy
      );
    }

    canvas.addEventListener('mousedown', function (e) {
      if (e.button !== 0) return;
      e.preventDefault();
      dragStart = canvasCoords(e);
    });

    canvas.addEventListener('mousemove', function (e) {
      if (!dragStart) return;
      var cur = canvasCoords(e);

      var rawDx = cur.x - dragStart.x;
      var rawDy = cur.y - dragStart.y;

      var end;
      if (!e.shiftKey) {
        /* Constrain to canvas aspect ratio so zoomed view is not distorted */
        var aspectRatio = canvas.width / canvas.height;
        var absDx = Math.abs(rawDx);
        var absDy = Math.abs(rawDy);

        if (absDx / aspectRatio >= absDy) {
          /* Width-constrained: fix dx, derive dy */
          var constrainedDy = absDx / aspectRatio;
          end = {
            x: dragStart.x + rawDx,
            y: dragStart.y + (rawDy >= 0 ? constrainedDy : -constrainedDy)
          };
        } else {
          /* Height-constrained: fix dy, derive dx */
          var constrainedDx = absDy * aspectRatio;
          end = {
            x: dragStart.x + (rawDx >= 0 ? constrainedDx : -constrainedDx),
            y: dragStart.y + rawDy
          };
        }
      } else {
        /* Shift held: free aspect ratio */
        end = cur;
      }

      dragEnd = end;

      var x1 = Math.min(dragStart.x, end.x);
      var y1 = Math.min(dragStart.y, end.y);
      var x2 = Math.max(dragStart.x, end.x);
      var y2 = Math.max(dragStart.y, end.y);
      drawZoomBox(x1, y1, x2, y2);
    });

    canvas.addEventListener('mouseup', function (e) {
      if (!dragStart) return;
      /* Use last constrained position from mousemove; fall back to raw coords */
      var end = dragEnd || canvasCoords(e);
      var dx  = Math.abs(end.x - dragStart.x);
      var dy  = Math.abs(end.y - dragStart.y);

      /* Clear overlay */
      octx.clearRect(0, 0, overlay.width, overlay.height);

      if (dx < 5 && dy < 5) {
        /* Click: zoom in 2x centred on click point */
        wasmZoomAtPoint(end.x, end.y, 0.5);
      } else {
        /* Drag: zoom to selected rectangle */
        var x1 = Math.min(dragStart.x, end.x);
        var y1 = Math.min(dragStart.y, end.y);
        var x2 = Math.max(dragStart.x, end.x);
        var y2 = Math.max(dragStart.y, end.y);
        wasmZoomToRect(x1, y1, x2, y2);
      }
      dragStart = null;
      dragEnd   = null;
    });

    /* Cancel drag if mouse leaves the canvas */
    canvas.addEventListener('mouseleave', function () {
      if (dragStart) {
        octx.clearRect(0, 0, overlay.width, overlay.height);
        dragStart = null;
        dragEnd   = null;
      }
    });

    /* Right-click: zoom out 2x */
    canvas.addEventListener('contextmenu', function (e) {
      e.preventDefault();
      var pos = canvasCoords(e);
      wasmZoomAtPoint(pos.x, pos.y, 2.0);
    });

    /* Scroll wheel: zoom in/out at cursor */
    canvas.addEventListener('wheel', function (e) {
      e.preventDefault();
      var pos    = canvasCoords(e);
      var factor = e.deltaY > 0 ? 2.0 : 0.5;
      wasmZoomAtPoint(pos.x, pos.y, factor);
    }, { passive: false });
  }

}());
