# Capture: screenshots of the DAOS monitor for the docs

Run on the laptop, with the page forwarded from the node (`ssh -O forward -L 8766:localhost:8766 <node>`; [live-monitoring](../../docs/howto/live-monitoring.md)). `shot_monitor.js` needs Node and Playwright (`npx playwright install chromium` once); set `PLAYWRIGHT` to the Playwright package if `require('playwright')` can't find it, for example `PLAYWRIGHT=~/.npm/_npx/<hash>/node_modules/playwright`.

| Script | What it makes |
|---|---|
| [`shot_monitor.js`](shot_monitor.js) `OUT.png [URL]` | One screenshot of the DAOS monitor (default `http://localhost:8766/`), light theme on white, 1400 px wide, cropped below the last panel (the docs' `sc26_daos_monitor.png`) |
