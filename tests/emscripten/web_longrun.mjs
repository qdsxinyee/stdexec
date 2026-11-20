// Long-run stability driver for arsenal's web demo under JSPI ctx.run().
// Arms console/exception capture, runs the demo for N seconds clicking the
// counter area, samples heap + count every second, and reports:
//   - zero exceptions / SuspendErrors / null-indirect-calls = pass
//   - JS heap stability (no growth trend)
// Usage: node web_longrun.mjs <page-url> <seconds>  (needs the demo served
// over http, e.g. python -m http.server in build-web/bin)
// coordinates, samples count/heap every second, final verdict.
// Usage: node web_longrun.mjs <page-url> <seconds>
import { spawn } from 'node:child_process';

const url = process.argv[2];
const seconds = Number(process.argv[3] ?? 45);
const prof = 'C:/Users/Dell/AppData/Local/Temp/jspi_long_prof_' +
             Math.floor(Math.random() * 1e9);

const chrome = spawn(
  'C:/Program Files/Google/Chrome/Application/chrome.exe',
  ['--headless=new', '--remote-debugging-port=9227', '--no-first-run',
   `--user-data-dir=${prof}`,
   '--window-size=800,600', 'about:blank'],
  { stdio: 'ignore' });

const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

let target = null;
for (let i = 0; i < 40 && !target; ++i) {
  await sleep(250);
  try {
    const res = await fetch('http://127.0.0.1:9227/json');
    const list = await res.json();
    target = list.find((t) => t.type === 'page');
  } catch { /* not up yet */ }
}
if (!target) {
  console.error('NO-TARGET');
  chrome.kill();
  process.exit(2);
}

const ws = new WebSocket(target.webSocketDebuggerUrl);
await new Promise((resolve, reject) => {
  ws.onopen = resolve;
  ws.onerror = reject;
});

let mid = 0;
const pending = new Map();
const consoleLines = [];

ws.onmessage = (ev) => {
  const msg = JSON.parse(ev.data);
  if (msg.id && pending.has(msg.id)) {
    pending.get(msg.id)(msg);
    pending.delete(msg.id);
    return;
  }
  if (msg.method === 'Runtime.consoleAPICalled') {
    for (const a of msg.params.args ?? []) {
      if (a.value !== undefined) consoleLines.push(String(a.value).slice(0, 200));
    }
  }
  if (msg.method === 'Runtime.exceptionThrown') {
    consoleLines.push('EXCEPTION: ' +
      String(msg.params.exceptionDetails?.exception?.description ??
             msg.params.exceptionDetails?.text ?? '?').slice(0, 300));
  }
  if (msg.method === 'Log.entryAdded') {
    consoleLines.push('LOG: ' + String(msg.params.entry.text).slice(0, 200));
  }
};

const call = (method, params = {}) =>
  new Promise((resolve) => {
    const id = ++mid;
    pending.set(id, resolve);
    ws.send(JSON.stringify({ id, method, params }));
  });

await call('Runtime.enable');
await call('Log.enable');
await call('Page.enable');

await call('Page.navigate', { url });
await sleep(4000);

// Find the canvas position so clicks land in page coordinates.
const canvasPos = await call('Runtime.evaluate',
  { expression:
    'JSON.stringify((c => ({x: c.getBoundingClientRect().left, y: c.getBoundingClientRect().top, w: c.width, h: c.height}))(document.querySelector("canvas")))',
    returnByValue: true });
const cp = JSON.parse(canvasPos.result?.result?.value ?? '{"x":0,"y":0}');
console.log('CANVAS:', JSON.stringify(cp));

// Click the button via dispatchMouseEvent at canvas coords + in-canvas offset.
// The demo button sits near the canvas top-left (title bar 32px + column
// start). Sample: click at (55, 75) in canvas space.
let clicks = 0;
let lastCount = -1;
let lastTitle = '';
const samples = [];
const heapSamples = [];
const deadline = Date.now() + seconds * 1000;
while (Date.now() < deadline) {
  // Button center in canvas pixels (title bar 32 + column start ~ 80);
  // canvas origin is page (0, 29).
  const px = cp.x + 55;
  const py = cp.y + 29 + 80;
  await call('Input.dispatchMouseEvent',
             { type: 'mousePressed', x: px, y: py, button: 'left',
               clickCount: 1 });
  await call('Input.dispatchMouseEvent',
             { type: 'mouseReleased', x: px, y: py, button: 'left',
               clickCount: 1 });
  clicks++;
  await sleep(1000);
  const res = await call('Runtime.evaluate',
    { expression:
      'JSON.stringify({title: document.title, ' +
      'count: (document.body.innerText.match(/Count: (\\d+)/) || [,"-"])[1], ' +
      'heap: performance.memory ? Math.round(performance.memory.usedJSHeapSize/1024) : -1})',
      returnByValue: true });
  const v = res.result?.result?.value;
  if (v) {
    const o = JSON.parse(v);
    samples.push(o);
    heapSamples.push(o.heap);
    lastCount = Number(o.count);
    lastTitle = o.title;
    console.log(`t+${clicks}s  count=${o.count}  heap=${o.heap}KB  title="${o.title}"`);
  }
}

const finalRes = await call('Runtime.evaluate',
  { expression:
    'JSON.stringify({count: (document.body.innerText.match(/Count: (\\d+)/) || [,"-"])[1], ' +
    'errs: (globalThis.__stdexecErrs ?? []).length})',
    returnByValue: true });
console.log('FINAL:', finalRes.result?.result?.value);
console.log('CLICKS:', clicks);
console.log('HEAP first/last:', heapSamples[0] + 'KB / ' + heapSamples[heapSamples.length-1] + 'KB');
console.log('CONSOLE-LINES:', consoleLines.length);
const bad = consoleLines.filter((l) =>
  l.startsWith('EXCEPTION') || l.includes('trap') || l.includes('Aborted') ||
  l.includes('SuspendError') || l.includes('null indirect'));
console.log('BAD-LINES:', bad.length ? bad.join(' || ') : '(none)');

chrome.kill();
process.exit(bad.length ? 1 : 0);
