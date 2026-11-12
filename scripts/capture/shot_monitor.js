// shot_monitor.js OUT.png [URL]: screenshot of the DAOS monitor page (default http://localhost:8766/) in the
// light theme on a white background, 1400 px wide, cropped below its last panel. Waits 6 s for data.
const { chromium } = require('./pw');
const out = process.argv[2], url = process.argv[3] || 'http://localhost:8766/';
if (!out) { console.error('usage: shot_monitor.js OUT.png [URL]'); process.exit(1); }
(async () => {
  const b = await chromium.launch();
  const p = await b.newPage({ viewport: { width: 1400, height: 1000 }, colorScheme: 'light' });
  await p.goto(url, { waitUntil: 'networkidle' });
  await p.addStyleTag({ content: ':root{--bg:#fff !important;--surface:#fff !important}' });
  await p.waitForTimeout(6000);
  const h = await p.evaluate(() => Math.max(...[...document.querySelectorAll('.panel,.tile')].map(e => e.getBoundingClientRect().bottom)) + 20);
  await p.screenshot({ path: out, clip: { x: 0, y: 0, width: 1400, height: Math.min(1000, Math.ceil(h)) } });
  await b.close();
})();
