const { defineConfig } = require('@playwright/test');

module.exports = defineConfig({
  testDir: './tests/e2e',
  timeout: 60000,
  use: {
    baseURL: 'http://localhost:8766',  // separate port from dev server
    headless: true,
    launchOptions: {
      args: ['--enable-features=SharedArrayBuffer']  // ensure SAB in headless
    }
  },
  webServer: {
    command: 'node tests/e2e/server.js',
    url: 'http://localhost:8766',
    reuseExistingServer: false,
    timeout: 15000
  }
});
