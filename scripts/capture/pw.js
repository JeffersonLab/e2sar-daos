// pw.js: load Playwright for the capture scripts. PLAYWRIGHT = path to its package (for example the one
// `npx playwright` cached: ~/.npm/_npx/<hash>/node_modules/playwright); otherwise a normal require().
module.exports = require(process.env.PLAYWRIGHT || 'playwright');
