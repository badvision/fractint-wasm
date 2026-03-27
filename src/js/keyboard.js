/*
 * keyboard.js — DOM keyboard event to Fractint key code translation
 *
 * Fractint uses DOS scan code conventions:
 *   ASCII for printable characters
 *   1000-series for function/arrow keys (F1=1059, UP=1072, etc.)
 *
 * Key events are delivered to WASM via Module.ccall('wasm_push_key').
 * Before the Module is ready, they are buffered in keyQueue and flushed
 * after onRuntimeInitialized.
 */

(function () {
  'use strict';

  /* Fractint key constants (matches port.h / fractint.h definitions) */
  var KEY = {
    F1: 1059, F2: 1060, F3: 1061, F4: 1062, F5: 1063,
    F6: 1064, F7: 1065, F8: 1066, F9: 1067, F10: 1068,
    UP:    1072, DOWN:  1080, LEFT:  1075, RIGHT: 1077,
    PGUP:  1073, PGDN:  1081, HOME:  1071, END:   1079,
    INS:   1082, DEL:   1083,
    ESC:   27,   ENTER: 13,   TAB:   9
  };

  /* Pre-runtime key buffer — flushed when Module is ready */
  var keyQueue = [];
  var moduleReady = false;

  function pushKey(code) {
    if (code === 0) return;
    if (moduleReady && window.Module && window.Module.ccall) {
      window.Module.ccall('wasm_push_key', null, ['number'], [code]);
    } else {
      keyQueue.push(code);
    }
  }

  function flushQueue() {
    moduleReady = true;
    while (keyQueue.length > 0) {
      var code = keyQueue.shift();
      if (window.Module && window.Module.ccall) {
        window.Module.ccall('wasm_push_key', null, ['number'], [code]);
      }
    }
  }

  function translateKey(e) {
    switch (e.key) {
      case 'F1':         return KEY.F1;
      case 'F2':         return KEY.F2;
      case 'F3':         return KEY.F3;
      case 'F4':         return KEY.F4;
      case 'F5':         return KEY.F5;
      case 'F6':         return KEY.F6;
      case 'F7':         return KEY.F7;
      case 'F8':         return KEY.F8;
      case 'F9':         return KEY.F9;
      case 'F10':        return KEY.F10;
      case 'ArrowUp':    return KEY.UP;
      case 'ArrowDown':  return KEY.DOWN;
      case 'ArrowLeft':  return KEY.LEFT;
      case 'ArrowRight': return KEY.RIGHT;
      case 'PageUp':     return KEY.PGUP;
      case 'PageDown':   return KEY.PGDN;
      case 'Home':       return KEY.HOME;
      case 'End':        return KEY.END;
      case 'Insert':     return KEY.INS;
      case 'Delete':     return KEY.DEL;
      case 'Escape':     return KEY.ESC;
      case 'Enter':      return KEY.ENTER;
      case 'Tab':        return KEY.TAB;
      default:
        if (e.key.length === 1) return e.key.charCodeAt(0);
        return 0;
    }
  }

  function onKeyDown(e) {
    var code = translateKey(e);
    if (code !== 0) {
      pushKey(code);
      e.preventDefault();
    }
  }

  function init() {
    var c = document.getElementById('canvas');
    if (c) {
      c.addEventListener('keydown', onKeyDown);
      c.addEventListener('click', function () { c.focus(); });
    }
    /* Also listen on document for keys when canvas is not focused */
    document.addEventListener('keydown', function (e) {
      /* Only if canvas has focus, handled above; but also allow global keys */
      if (document.activeElement !== c) {
        var code = translateKey(e);
        if (code !== 0) pushKey(code);
      }
    });
  }

  window.FractintKeyboard = {
    init:        init,
    pushKey:     pushKey,
    flushQueue:  flushQueue,
    keyQueue:    keyQueue   /* writable array — ui.js can also push into this */
  };

  document.addEventListener('DOMContentLoaded', init);

  /* Hook into Module.onRuntimeInitialized to flush buffered keys */
  var origOnInit = (window.Module && window.Module.onRuntimeInitialized) || null;
  window.Module = window.Module || {};
  var savedOnInit = window.Module.onRuntimeInitialized;
  window.Module.onRuntimeInitialized = function () {
    flushQueue();
    if (savedOnInit) savedOnInit();
  };
}());
