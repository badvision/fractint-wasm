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
  var wasmSetMaxit         = null;
  var wasmSetInside        = null;
  var wasmSetOutside       = null;
  var wasmSetJuliaParams   = null;
  var wasmSetPalettePreset = null;

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
      wasmToggleCycle    = Module.cwrap('wasm_toggle_cycle',     'void', ['number']);
      wasmSetCycleSpeed  = Module.cwrap('wasm_set_cycle_speed',  'void', ['number']);
      wasmSetFractype    = Module.cwrap('wasm_set_fractype',     'void', ['number']);
      wasmGetFractype    = Module.cwrap('wasm_get_fractype',     'number', []);
      wasmZoomToRect     = Module.cwrap('wasm_zoom_to_rect',     'void', ['number','number','number','number']);
      wasmZoomAtPoint    = Module.cwrap('wasm_zoom_at_point',    'void', ['number','number','number']);
      wasmSetMaxit         = Module.cwrap('wasm_set_maxit',          'void', ['number']);
      wasmSetInside        = Module.cwrap('wasm_set_inside',         'void', ['number']);
      wasmSetOutside       = Module.cwrap('wasm_set_outside',        'void', ['number']);
      wasmSetJuliaParams   = Module.cwrap('wasm_set_julia_params',   'void', ['number','number']);
      wasmSetPalettePreset = Module.cwrap('wasm_set_palette_preset', 'void', ['number']);
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

    var btnCycle       = document.getElementById('cycle-toggle');
    var btnDir         = document.getElementById('cycle-dir');
    var speedIn        = document.getElementById('cycle-speed');
    var btnReset       = document.getElementById('btn-reset');
    var btnZoomIn      = document.getElementById('btn-zoom-in');
    var btnZoomOut     = document.getElementById('btn-zoom-out');
    var btnSave        = document.getElementById('btn-save');
    var fracTypeSelect = document.getElementById('fractal-type');
    var palettePreset  = document.getElementById('palette-preset');
    var paramMaxit     = document.getElementById('param-maxit');
    var paramInside    = document.getElementById('param-inside');
    var paramOutside   = document.getElementById('param-outside');
    var juliaGroup     = document.getElementById('julia-params-group');
    var paramJuliaRe   = document.getElementById('param-julia-re');
    var paramJuliaIm   = document.getElementById('param-julia-im');
    var btnApplyJulia  = document.getElementById('btn-apply-julia');
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

    /* Julia fractal type indices — show seed panel for these */
    var JULIA_TYPES = { 1: true, 6: true, 108: true, 109: true };

    function updateJuliaPanel() {
      if (!juliaGroup || !fracTypeSelect) return;
      var type = parseInt(fracTypeSelect.value, 10);
      juliaGroup.style.display = JULIA_TYPES[type] ? '' : 'none';
    }

    if (fracTypeSelect) {
      fracTypeSelect.addEventListener('change', function () {
        var type = parseInt(this.value, 10);
        if (window.FractintShare) window.FractintShare.pushHistoryState();
        if (wasmSetFractype) {
          wasmSetFractype(type);
        } else if (typeof Module !== 'undefined' && Module.ccall) {
          Module.ccall('wasm_set_fractype', 'void', ['number'], [type]);
        }
        updateJuliaPanel();
        /* Auto-apply Julia seed defaults when switching to a Julia type */
        if (JULIA_TYPES[type] && wasmSetJuliaParams && paramJuliaRe && paramJuliaIm) {
          var re = parseFloat(paramJuliaRe.value);
          var im = parseFloat(paramJuliaIm.value);
          if (!isNaN(re) && !isNaN(im)) {
            wasmSetJuliaParams(re, im);
          }
        }
      });
      updateJuliaPanel();
    }

    if (palettePreset) {
      palettePreset.addEventListener('change', function () {
        var p = parseInt(this.value, 10);
        if (wasmSetPalettePreset) wasmSetPalettePreset(p);
      });
    }

    if (paramMaxit) {
      paramMaxit.addEventListener('change', function () {
        var n = parseInt(this.value, 10);
        if (isNaN(n)) return;
        if (wasmSetMaxit) wasmSetMaxit(n);
      });
    }

    if (paramInside) {
      paramInside.addEventListener('change', function () {
        var mode = parseInt(this.value, 10);
        if (wasmSetInside) wasmSetInside(mode);
      });
    }

    if (paramOutside) {
      paramOutside.addEventListener('change', function () {
        var mode = parseInt(this.value, 10);
        if (wasmSetOutside) wasmSetOutside(mode);
      });
    }

    if (btnApplyJulia) {
      btnApplyJulia.addEventListener('click', function () {
        var re = parseFloat(paramJuliaRe ? paramJuliaRe.value : '-0.7');
        var im = parseFloat(paramJuliaIm ? paramJuliaIm.value : '0.27015');
        if (isNaN(re) || isNaN(im)) return;
        if (window.FractintShare) window.FractintShare.pushHistoryState();
        if (wasmSetJuliaParams) wasmSetJuliaParams(re, im);
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

    /* Sync UI controls when fractal state is restored from URL hash */
    window.addEventListener('fractinstaterestored', function (e) {
      var p = e.detail;
      if (!p) return;
      /* Sync maxit input */
      if (paramMaxit && typeof p.i === 'number') {
        paramMaxit.value = p.i;
      }
      /* Sync inside select */
      if (paramInside && typeof p.in === 'number') {
        paramInside.value = String(p.in);
      }
      /* Sync outside select */
      if (paramOutside && typeof p.out === 'number') {
        paramOutside.value = String(p.out);
      }
      /* Sync fractal type select and Julia panel visibility */
      if (fracTypeSelect && typeof p.t === 'number') {
        fracTypeSelect.value = String(p.t);
        updateJuliaPanel();
      }
      /* Sync Julia seed inputs */
      if (paramJuliaRe && typeof p.p0 === 'number') {
        paramJuliaRe.value = p.p0;
      }
      if (paramJuliaIm && typeof p.p1 === 'number') {
        paramJuliaIm.value = p.p1;
      }
    });
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
        if (window.FractintShare) window.FractintShare.pushHistoryState();
        wasmZoomAtPoint(end.x, end.y, 0.5);
      } else {
        /* Drag: zoom to selected rectangle */
        var x1 = Math.min(dragStart.x, end.x);
        var y1 = Math.min(dragStart.y, end.y);
        var x2 = Math.max(dragStart.x, end.x);
        var y2 = Math.max(dragStart.y, end.y);
        if (window.FractintShare) window.FractintShare.pushHistoryState();
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
      if (window.FractintShare) window.FractintShare.pushHistoryState();
      wasmZoomAtPoint(pos.x, pos.y, 2.0);
    });

    /* Scroll wheel: zoom in/out at cursor */
    canvas.addEventListener('wheel', function (e) {
      e.preventDefault();
      var pos    = canvasCoords(e);
      var factor = e.deltaY > 0 ? 2.0 : 0.5;
      if (window.FractintShare) window.FractintShare.pushHistoryState();
      wasmZoomAtPoint(pos.x, pos.y, factor);
    }, { passive: false });

    initTouchZoom(canvas);
  }

  /* ---------------------------------------------------------------- */
  /* Touch zoom/pan — initialised from initMouseZoom()               */
  /* ---------------------------------------------------------------- */

  function initTouchZoom(canvas) {
    /* Fractint arrow-key codes (matches keyboard.js KEY constants) */
    var KEY_LEFT  = 1075;
    var KEY_RIGHT = 1077;
    var KEY_UP    = 1072;
    var KEY_DOWN  = 1080;

    /* State for single-finger tap/drag */
    var tapStart     = null;  /* { x, y, time } in canvas pixels */

    /* State for two-finger pinch */
    var pinchStart   = null;  /* { dist, cx, cy } */
    var pinchCurrent = null;  /* { dist, cx, cy } */

    /* Map a Touch object to canvas pixel coordinates */
    function touchCanvasCoords(touch) {
      var rect   = canvas.getBoundingClientRect();
      var scaleX = canvas.width  / rect.width;
      var scaleY = canvas.height / rect.height;
      return {
        x: Math.round((touch.clientX - rect.left) * scaleX),
        y: Math.round((touch.clientY - rect.top)  * scaleY)
      };
    }

    /* Euclidean distance between two touch points (in client pixels) */
    function pinchDist(t1, t2) {
      return Math.hypot(t2.clientX - t1.clientX, t2.clientY - t1.clientY);
    }

    /* Midpoint of two touches in canvas pixel coordinates */
    function pinchCenter(t1, t2) {
      var mid = {
        clientX: (t1.clientX + t2.clientX) / 2,
        clientY: (t1.clientY + t2.clientY) / 2
      };
      return touchCanvasCoords(mid);
    }

    canvas.addEventListener('touchstart', function (e) {
      e.preventDefault();

      if (e.touches.length === 1) {
        /* Single finger: record tap start */
        var pos  = touchCanvasCoords(e.touches[0]);
        tapStart = { x: pos.x, y: pos.y, time: Date.now() };
        /* Reset any stale pinch state */
        pinchStart   = null;
        pinchCurrent = null;

      } else if (e.touches.length === 2) {
        /* Two fingers: begin pinch */
        tapStart = null;
        var dist = pinchDist(e.touches[0], e.touches[1]);
        var ctr  = pinchCenter(e.touches[0], e.touches[1]);
        pinchStart   = { dist: dist, cx: ctr.x, cy: ctr.y };
        pinchCurrent = { dist: dist, cx: ctr.x, cy: ctr.y };
      }
    }, { passive: false });

    canvas.addEventListener('touchmove', function (e) {
      e.preventDefault();

      if (e.touches.length === 2 && pinchStart) {
        /* Accumulate latest pinch distance and center — applied on touchend */
        var dist = pinchDist(e.touches[0], e.touches[1]);
        var ctr  = pinchCenter(e.touches[0], e.touches[1]);
        pinchCurrent = { dist: dist, cx: ctr.x, cy: ctr.y };
      }
    }, { passive: false });

    canvas.addEventListener('touchend', function (e) {
      e.preventDefault();

      /* --- Pinch-to-zoom: apply accumulated gesture on finger lift --- */
      if (pinchStart && pinchCurrent && pinchStart.dist > 0) {
        var factor = pinchStart.dist / pinchCurrent.dist;
        /* Only zoom if there was meaningful movement (avoid noise) */
        if (Math.abs(factor - 1.0) > 0.05) {
          if (window.FractintShare) window.FractintShare.pushHistoryState();
          wasmZoomAtPoint(pinchStart.cx, pinchStart.cy, factor);
        }
        pinchStart   = null;
        pinchCurrent = null;
        tapStart     = null;
        return;
      }
      pinchStart   = null;
      pinchCurrent = null;

      /* --- Single-finger tap or drag --- */
      if (!tapStart) return;

      /* Need the released touch — use changedTouches */
      if (e.changedTouches.length === 0) {
        tapStart = null;
        return;
      }

      var endPos  = touchCanvasCoords(e.changedTouches[0]);
      var dx      = endPos.x - tapStart.x;
      var dy      = endPos.y - tapStart.y;
      var dist    = Math.hypot(dx, dy);
      var elapsed = Date.now() - tapStart.time;

      if (dist < 10 && elapsed < 300) {
        /* Short tap with minimal movement: zoom in 2x at tap point */
        if (window.FractintShare) window.FractintShare.pushHistoryState();
        wasmZoomAtPoint(tapStart.x, tapStart.y, 0.5);

      } else if (dist >= 10 && window.FractintKeyboard) {
        /* Drag: inject arrow-key presses proportional to distance */
        var absDx   = Math.abs(dx);
        var absDy   = Math.abs(dy);
        var presses = Math.min(Math.round(dist / 30), 8); /* cap at 8 */

        if (absDx >= absDy) {
          /* Dominant horizontal */
          var hKey = dx > 0 ? KEY_RIGHT : KEY_LEFT;
          for (var i = 0; i < presses; i++) {
            window.FractintKeyboard.pushKey(hKey);
          }
        } else {
          /* Dominant vertical */
          var vKey = dy > 0 ? KEY_DOWN : KEY_UP;
          for (var j = 0; j < presses; j++) {
            window.FractintKeyboard.pushKey(vKey);
          }
        }
      }

      tapStart = null;
    }, { passive: false });

    canvas.addEventListener('touchcancel', function (e) {
      e.preventDefault();
      tapStart     = null;
      pinchStart   = null;
      pinchCurrent = null;
    }, { passive: false });
  }

}());
