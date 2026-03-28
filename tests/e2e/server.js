const http = require('http');
const fs = require('fs');
const path = require('path');

const WEB_DIR = path.join(__dirname, '../../web');
const PORT = 8766;

const MIME = {
  '.html': 'text/html',
  '.js': 'application/javascript',
  '.wasm': 'application/wasm',
  '.css': 'text/css',
  '.png': 'image/png',
  '.data': 'application/octet-stream',
};

const server = http.createServer((req, res) => {
  let urlPath = req.url.split('?')[0];
  if (urlPath === '/') urlPath = '/index.html';

  const filePath = path.join(WEB_DIR, urlPath);
  const ext = path.extname(filePath);

  // COOP/COEP required for SharedArrayBuffer (pthreads)
  res.setHeader('Cross-Origin-Opener-Policy', 'same-origin');
  res.setHeader('Cross-Origin-Embedder-Policy', 'require-corp');
  res.setHeader('Content-Type', MIME[ext] || 'application/octet-stream');

  fs.readFile(filePath, (err, data) => {
    if (err) {
      res.writeHead(404);
      res.end('Not found');
      return;
    }
    res.writeHead(200);
    res.end(data);
  });
});

server.listen(PORT, () => console.log(`Test server on http://localhost:${PORT}`));
