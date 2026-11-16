// Checks on a board what the web page does with the statistics' UTC offset (#679), in a headless Chromium:
//  - without an admin password, a look at the home page sends the browser's offset when the board has another;
//  - with one, it sends nothing (it asked for the password), and Settings show the offset and save it.
// Usage: node timezone_page_check.cjs <board> <admin password to set for the test>
// The board's statistics offset is set to a wrong value first, and the password is removed at the end.
const { chromium } = require('playwright');

const WRONG_OFFSET = 345;
const SECONDS_ON_THE_HOME_PAGE = 12;

async function boardConfig(board, password) {
  const headers = password ? { Authorization: 'Basic ' + Buffer.from('admin:' + password).toString('base64') } : {};
  const answer = await fetch(`http://${board}/api/config`, { headers });
  return answer.json();
}

async function postConfig(board, settings, password) {
  const headers = { 'Content-Type': 'application/json' };
  if (password) headers.Authorization = 'Basic ' + Buffer.from('admin:' + password).toString('base64');
  const answer = await fetch(`http://${board}/api/config`, { method: 'POST', headers, body: JSON.stringify(settings) });
  return answer.status;
}

async function lookAtTheHomePage(browser, board, password) {
  const context = await browser.newContext(password ? { httpCredentials: { username: 'admin', password } } : {});
  const page = await context.newPage();
  const configWrites = [];
  const passwordAsked = [];
  page.on('request', (request) => {
    if (request.method() === 'POST' && request.url().endsWith('/api/config')) configWrites.push(request.postData());
  });
  page.on('response', (response) => {
    if (response.status() === 401) passwordAsked.push(response.url());
  });
  await page.goto(`http://${board}/`);
  await page.waitForTimeout(SECONDS_ON_THE_HOME_PAGE * 1000);
  const browserOffset = await page.evaluate(() => -new Date().getTimezoneOffset());
  return { page, context, configWrites, passwordAsked, browserOffset };
}

async function settingsField(page) {
  await page.evaluate(() => { location.hash = '#settings'; });
  await page.waitForTimeout(2000);
  return page.evaluate(() => ({
    rowHidden: document.querySelector('#sStatsTzRow').hidden,
    value: document.querySelector('#sStatsTz').value,
  }));
}

async function main() {
  const [board, password] = process.argv.slice(2);
  const browser = await chromium.launch({ headless: true });
  const report = {};

  await postConfig(board, { stats: { tz: WRONG_OFFSET } });
  const without = await lookAtTheHomePage(browser, board, null);
  report.without_password = {
    config_writes: without.configWrites,
    password_asked: without.passwordAsked.length,
    offset_on_the_board_after: (await boardConfig(board)).stats.tz,
    browser_offset: without.browserOffset,
    settings: await settingsField(without.page),
  };
  await without.context.close();

  await postConfig(board, { stats: { tz: WRONG_OFFSET }, admin: { pwd: password } });
  const withPassword = await lookAtTheHomePage(browser, board, password);
  report.with_password = {
    config_writes: withPassword.configWrites,
    offset_on_the_board_after: (await boardConfig(board, password)).stats.tz,
    settings: await settingsField(withPassword.page),
  };
  await withPassword.page.click('#sStatsTzBrowser');
  report.with_password.after_the_button = await withPassword.page.evaluate(() => document.querySelector('#sStatsTz').value);
  await withPassword.page.click('#sSave');
  await withPassword.page.waitForTimeout(4000);
  report.with_password.offset_after_save = (await boardConfig(board, password)).stats.tz;
  await withPassword.context.close();

  report.password_removed = await postConfig(board, { admin: { pwd: '' } }, password);
  await browser.close();
  console.log(JSON.stringify(report, null, 1));
}

main().catch((error) => { console.error(error); process.exit(1); });
