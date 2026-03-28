/*
 * qrwidget.js — Dynamic QR code for the current fractal URL.
 *
 * Renders into #qr-canvas whenever the fractal hash changes.
 * Depends on qrcode.min.js (QRCode constructor) being loaded first.
 */

(function () {
  'use strict';

  var qr = null;        /* QRCode instance */
  var lastUrl = '';     /* avoid re-render when URL hasn't changed */
  var container = null;

  function render() {
    var url = window.location.href;
    if (url === lastUrl) return;
    lastUrl = url;

    if (!container) {
      container = document.getElementById('qr-canvas');
      if (!container) return;
    }

    if (!window.QRCode) return;

    if (!qr) {
      qr = new QRCode(container, {
        width:         160,
        height:        160,
        colorDark:     '#e0e0e0',
        colorLight:    '#1a1a1a',
        correctLevel:  QRCode.CorrectLevel.M
      });
    }

    try {
      qr.makeCode(url);
    } catch (e) {
      /* URL too long for QR — show a note */
      container.innerHTML = '<span style="color:#555;font-size:11px;padding:8px;display:block;text-align:center">URL too long for QR</span>';
      qr = null;
    }
  }

  /* Update on every fractal hash change (fired by urlshare.js) */
  window.addEventListener('fractinthashchange', render);

  /* Also catch back/forward navigation */
  window.addEventListener('popstate', render);

  /* One-time hashchange listener: handles the case where a user pastes a
   * shared link into a new tab.  The hash is already in the URL before WASM
   * loads, so fractinthashchange never fires for that initial hash.  A plain
   * hashchange fires when the page hash is first established, which gives us
   * a chance to render the QR for the initial shared URL. */
  window.addEventListener('hashchange', render, { once: true });

  document.addEventListener('DOMContentLoaded', function () {
    container = document.getElementById('qr-canvas');
  });

}());
