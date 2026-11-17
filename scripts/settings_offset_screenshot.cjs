// Takes a picture of the statistics offset row in Settings, as the page shows it with an admin password set (#679).
// Usage: node settings_offset_screenshot.cjs <board> <admin password to set for the picture> <output folder>
// The password is removed at the end. Two files are written: the card with the row, and the whole Settings page.
const path = require('path');
const { chromium } = require('playwright');

function basicAuthorization(password) {
  return 'Basic ' + Buffer.from('admin:' + password).toString('base64');
}

async function postConfig(board, settings, password) {
  const headers = { 'Content-Type': 'application/json' };
  if (password) headers.Authorization = basicAuthorization(password);
  const answer = await fetch(`http://${board}/api/config`, { method: 'POST', headers, body: JSON.stringify(settings) });
  return answer.status;
}

async function main() {
  const [board, password, outputFolder] = process.argv.slice(2);
  const passwordSet = await postConfig(board, { admin: { pwd: password } });
  const browser = await chromium.launch({ headless: true });
  const context = await browser.newContext({
    httpCredentials: { username: 'admin', password },
    viewport: { width: 1100, height: 900 },
    locale: 'en-US',
    deviceScaleFactor: 2,
  });
  const page = await context.newPage();
  await page.goto(`http://${board}/#settings`);
  await page.waitForTimeout(4000);
  const row = page.locator('#sStatsTzRow');
  const rowVisible = await row.isVisible();
  const card = page.locator('.card', { has: row });
  await card.scrollIntoViewIfNeeded();
  await card.screenshot({ path: path.join(outputFolder, 'settings-offset-card.png') });
  await row.screenshot({ path: path.join(outputFolder, 'settings-offset-row.png') });
  await context.close();
  await browser.close();
  const passwordRemoved = await postConfig(board, { admin: { pwd: '' } }, password);
  console.log(JSON.stringify({ passwordSet, rowVisible, passwordRemoved }));
}

main().catch((error) => { console.error(error); process.exit(1); });
