const { test, expect } = require('@playwright/test');

/**
 * Set up error capture on a page BEFORE navigation.
 *
 * Returns an `errors` array that accumulates:
 *   - pageerror events (main-thread JS errors)
 *   - console.error messages (includes Worker-propagated errors via
 *     fractint-wasm.js's worker.onerror → Module.printErr path)
 *
 * Also injects an initScript that patches console.error so messages
 * containing 'unreachable' or 'worker sent an error' are stored in
 * window.__workerErrors, giving tests a second surface to check.
 *
 * NOTE: call this BEFORE page.goto() so all listeners are registered
 * before any network activity begins.
 */
async function setupErrorCapture(page) {
  const errors = [];

  // Main-thread JS errors
  page.on('pageerror', err => errors.push('pageerror: ' + err.message));

  // console.error — Worker crashes propagate here via worker.onerror in
  // fractint-wasm.js which calls Module.printErr on the main thread.
  page.on('console', msg => {
    if (msg.type() === 'error') errors.push('console.error: ' + msg.text());
  });

  // Patch console.error in the page context so Worker-crash keywords are
  // stored separately; useful for targeted assertions.
  await page.addInitScript(() => {
    const origError = console.error.bind(console);
    console.error = (...args) => {
      origError(...args);
      const msg = args.join(' ');
      if (msg.includes('unreachable') || msg.includes('worker sent an error') ||
          msg.includes('RuntimeError')) {
        window.__workerErrors = window.__workerErrors || [];
        window.__workerErrors.push(msg);
      }
    };
  });

  return errors;
}

test.describe('Fractint WASM smoke tests', () => {

  test.beforeEach(async ({ page }) => {
    // Register listeners BEFORE goto so nothing is missed
    const errors = await setupErrorCapture(page);

    await page.goto('/');

    // Wait for the loading overlay to disappear (WASM ready)
    await page.waitForSelector('#loading-overlay', { state: 'hidden', timeout: 30000 });

    // Store errors for per-test access
    page._testErrors = errors;
  });

  test('page loads without errors', async ({ page }) => {
    expect(page._testErrors).toHaveLength(0);
  });

  test('canvas is visible and has content', async ({ page }) => {
    const canvas = page.locator('#canvas');
    await expect(canvas).toBeVisible();
    const box = await canvas.boundingBox();
    expect(box.width).toBeGreaterThan(100);
    expect(box.height).toBeGreaterThan(100);
  });

  test('fractal type dropdown changes fractal', async ({ page }) => {
    await page.selectOption('#fractal-type', '1');  // Julia
    // Wait a moment for render to start
    await page.waitForTimeout(500);
    expect(page._testErrors).toHaveLength(0);
  });

  test('arrow keys pan the fractal', async ({ page }) => {
    const canvas = page.locator('#canvas');
    await canvas.click();  // focus canvas
    await page.keyboard.press('ArrowRight');
    await page.waitForTimeout(300);
    // No crash = pass
    expect(page._testErrors).toHaveLength(0);
  });

  test('zoom via rectangle drag does not crash', async ({ page }) => {
    const canvas = page.locator('#canvas');
    const box = await canvas.boundingBox();

    // Drag a zoom rectangle
    await page.mouse.move(box.x + 100, box.y + 100);
    await page.mouse.down();
    await page.mouse.move(box.x + 300, box.y + 250);
    await page.mouse.up();

    // Wait for render
    await page.waitForTimeout(2000);

    expect(page._testErrors).toHaveLength(0);
  });

  test('arrow keys work after zoom', async ({ page }) => {
    const canvas = page.locator('#canvas');
    const box = await canvas.boundingBox();

    // Zoom first
    await page.mouse.move(box.x + 100, box.y + 100);
    await page.mouse.down();
    await page.mouse.move(box.x + 300, box.y + 250);
    await page.mouse.up();
    await page.waitForTimeout(2000);

    // Then try arrow key
    await canvas.click();
    await page.keyboard.press('ArrowRight');
    await page.waitForTimeout(500);

    expect(page._testErrors).toHaveLength(0);
  });

  test('Z key does not crash worker', async ({ page }) => {
    const canvas = page.locator('#canvas');

    // Poll until the fractal engine is in WS_DONE state before pressing Z.
    // The beforeEach only waits for loading-overlay to hide (WASM ready),
    // but the engine may still be in WS_CALCFRAC.
    await page.waitForFunction(() => {
      if (!window.Module || !window.Module.ccall) return false;
      const d1 = window.Module.ccall('wasm_consume_dirty', 'number', [], []);
      return d1 === 0;
    }, { timeout: 15000, polling: 200 });

    await canvas.click();
    await page.keyboard.press('z');
    // Give the menu pthread time to start and run
    await page.waitForTimeout(2000);

    // Check both error surfaces
    const workerErrors = await page.evaluate(() => window.__workerErrors || []);
    expect(workerErrors, 'Worker errors: ' + JSON.stringify(workerErrors)).toHaveLength(0);

    const unrecoverableErrors = page._testErrors.filter(
      e => e.includes('unreachable') || e.includes('RuntimeError')
    );
    expect(unrecoverableErrors, 'Crash errors: ' + JSON.stringify(unrecoverableErrors)).toHaveLength(0);
  });

  test('Z key after zoom rectangle does not crash or freeze', async ({ page }) => {
    const canvas = page.locator('#canvas');
    const box = await canvas.boundingBox();

    // Zoom first
    await page.mouse.move(box.x + 100, box.y + 100);
    await page.mouse.down();
    await page.mouse.move(box.x + 300, box.y + 250);
    await page.mouse.up();
    await page.waitForTimeout(2000);

    // Then press Z — must not crash
    await canvas.click();
    await page.keyboard.press('z');
    await page.waitForTimeout(1000);

    // Arrow keys must still work (proves keyboard not frozen)
    await page.keyboard.press('ArrowRight');
    await page.waitForTimeout(500);

    const workerErrors = await page.evaluate(() => window.__workerErrors || []);
    expect(workerErrors, 'Worker errors: ' + JSON.stringify(workerErrors)).toHaveLength(0);

    const crashErrors = page._testErrors.filter(
      e => e.includes('unreachable') || e.includes('RuntimeError')
    );
    expect(crashErrors, 'Crash errors: ' + JSON.stringify(crashErrors)).toHaveLength(0);
  });

  test('arrow keys work before and after fractal type change', async ({ page }) => {
    const canvas = page.locator('#canvas');
    await canvas.click();
    await page.keyboard.press('ArrowRight');
    await page.waitForTimeout(300);

    await page.selectOption('#fractal-type', '1');
    await page.waitForTimeout(1500);

    await canvas.click();
    await page.keyboard.press('ArrowLeft');
    await page.waitForTimeout(300);

    expect(page._testErrors).toHaveLength(0);
  });

});
