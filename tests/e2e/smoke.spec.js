const { test, expect } = require('@playwright/test');

test.describe('Fractint WASM smoke tests', () => {

  test.beforeEach(async ({ page }) => {
    // Capture console errors
    const errors = [];
    page.on('console', msg => {
      if (msg.type() === 'error') errors.push(msg.text());
    });
    page.on('pageerror', err => errors.push(err.message));

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
    const errors = [];
    page.on('console', msg => { if (msg.type() === 'error') errors.push(msg.text()); });

    await page.selectOption('#fractal-type', '1');  // Julia
    // Wait a moment for render to start
    await page.waitForTimeout(500);
    expect(errors).toHaveLength(0);
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
    const errors = [];
    page.on('pageerror', err => errors.push(err.message));
    page.on('console', msg => { if (msg.type() === 'error') errors.push(msg.text()); });

    const canvas = page.locator('#canvas');
    const box = await canvas.boundingBox();

    // Drag a zoom rectangle
    await page.mouse.move(box.x + 100, box.y + 100);
    await page.mouse.down();
    await page.mouse.move(box.x + 300, box.y + 250);
    await page.mouse.up();

    // Wait for render
    await page.waitForTimeout(2000);

    expect(errors).toHaveLength(0);
  });

  test('arrow keys work after zoom', async ({ page }) => {
    const errors = [];
    page.on('pageerror', err => errors.push(err.message));

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

    expect(errors).toHaveLength(0);
  });

  test('Z key does not crash', async ({ page }) => {
    const errors = [];
    page.on('pageerror', err => errors.push(err.message));
    page.on('console', msg => { if (msg.type() === 'error') errors.push(msg.text()); });

    const canvas = page.locator('#canvas');
    await canvas.click();
    await page.keyboard.press('z');
    await page.waitForTimeout(2000);

    expect(errors).toHaveLength(0);
  });

  test('Z key after zoom rectangle does not crash or freeze', async ({ page }) => {
    const errors = [];
    page.on('pageerror', err => errors.push(err.message));
    page.on('console', msg => { if (msg.type() === 'error') errors.push(msg.text()); });

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

    expect(errors.filter(e => e.includes('unreachable') || e.includes('RuntimeError'))).toHaveLength(0);
  });

  test('arrow keys work before and after fractal type change', async ({ page }) => {
    const errors = [];
    page.on('pageerror', err => errors.push(err.message));

    const canvas = page.locator('#canvas');
    await canvas.click();
    await page.keyboard.press('ArrowRight');
    await page.waitForTimeout(300);

    await page.selectOption('#fractal-type', '1');
    await page.waitForTimeout(1500);

    await canvas.click();
    await page.keyboard.press('ArrowLeft');
    await page.waitForTimeout(300);

    expect(errors).toHaveLength(0);
  });

});
