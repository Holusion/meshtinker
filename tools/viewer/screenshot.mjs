// A screenshot of tools/viewer/view.html, through headless Chromium's DevTools protocol, without other
// dependencies:
//   node tools/viewer/screenshot.mjs OUT.png URL [--view OUT2.png=QUERY]... [--wait SECONDS] [--size 1200x900]
//                                    [--chromium /usr/bin/chromium]
// URL is a view.html page tools/viewer/serve.py serves. Waits until the model is drawn (or --wait runs out),
// then writes the screenshot and prints the page's errors and warnings. Each --view then draws the same model from
// QUERY's view parameters (az=30&el=15&zoom=2, or eye=...&target=...), without loading it again, into its own file. WebGL runs on SwiftShader, so renders are slow
// but don't need a GPU.
import { spawn } from 'node:child_process';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';

const args = process.argv.slice(2);
const option = (name, fallback) => {
  const i = args.indexOf(name);
  if (i < 0) return fallback;
  const value = args[i + 1];
  args.splice(i, 2);
  return value;
};
const views = [];
for (let view = option('--view'); view !== undefined; view = option('--view')) {
  const at = view.indexOf('=');
  views.push([view.slice(0, at), view.slice(at + 1)]);
}
const wait = Number(option('--wait', '60'));
const [width, height] = option('--size', '1200x900').split('x').map(Number);
const chromium = option('--chromium', '/usr/bin/chromium');
const [out, url] = args;
if (!out || !url) {
  console.error('Usage: node tools/viewer/screenshot.mjs OUT.png URL [--wait SECONDS] [--size WxH]');
  process.exit(2);
}

const profile = fs.mkdtempSync(path.join(os.tmpdir(), 'meshtinker-chromium-'));
const browser = spawn(chromium, ['--headless=new', '--no-sandbox', '--enable-unsafe-swiftshader', '--use-angle=swiftshader',
  '--remote-debugging-port=0', `--user-data-dir=${profile}`, `--window-size=${width},${height}`, 'about:blank'],
{ stdio: ['ignore', 'ignore', 'pipe'] });
const endpoint = await new Promise((resolve, reject) => {
  let text = '';
  browser.stderr.on('data', (chunk) => {
    text += chunk;
    const match = text.match(/DevTools listening on (ws:\/\/\S+)/);
    if (match) resolve(match[1]);
  });
  browser.on('exit', () => reject(new Error('chromium exited')));
});

const port = new URL(endpoint).port;
const targets = await (await fetch(`http://127.0.0.1:${port}/json/list`)).json();
const page = targets.find((t) => t.type === 'page');
const socket = new WebSocket(page.webSocketDebuggerUrl);
await new Promise((resolve) => socket.addEventListener('open', resolve));
let next = 0;
const pending = new Map();
const messages = [];
socket.addEventListener('message', (event) => {
  const message = JSON.parse(event.data);
  if (message.id !== undefined && pending.has(message.id)) {
    pending.get(message.id)(message);
    pending.delete(message.id);
  } else if (message.method === 'Runtime.consoleAPICalled' && ['error', 'warning'].includes(message.params.type)) {
    messages.push(`${message.params.type}: ${message.params.args.map((a) => a.value ?? a.description ?? '').join(' ')}`);
  } else if (message.method === 'Runtime.exceptionThrown') {
    messages.push(`exception: ${message.params.exceptionDetails.exception?.description ?? message.params.exceptionDetails.text}`);
  }
});
const send = (method, params = {}) => new Promise((resolve) => {
  const id = next++;
  pending.set(id, resolve);
  socket.send(JSON.stringify({ id, method, params }));
});
await send('Runtime.enable');
await send('Page.enable');
await send('Emulation.setDeviceMetricsOverride', { width, height, deviceScaleFactor: 1, mobile: false });
await send('Page.navigate', { url });

// view.html sets window.renderDone once it has drawn the model (-1 if it failed to load).
const start = Date.now();
let done;
while ((Date.now() - start) / 1000 < wait) {
  await new Promise((r) => setTimeout(r, 500));
  const result = await send('Runtime.evaluate', { expression: 'window.renderDone', returnByValue: true });
  done = result.result?.result?.value;
  if (done !== undefined) break;
}
const shot = await send('Page.captureScreenshot', { format: 'png' });
fs.writeFileSync(out, Buffer.from(shot.result.data, 'base64'));
const rendered = done !== undefined && done >= 0;
console.log(JSON.stringify({ out, rendered, seconds: (Date.now() - start) / 1000, messages: messages.slice(0, 20) }));
for (const [file, query] of rendered ? views : []) {
  const begin = Date.now();
  await send('Runtime.evaluate', { expression: `window.view(${JSON.stringify(query)})`, returnByValue: true });
  let drawn;
  while ((Date.now() - begin) / 1000 < wait) {
    await new Promise((r) => setTimeout(r, 200));
    const result = await send('Runtime.evaluate', { expression: 'window.renderDone', returnByValue: true });
    drawn = result.result?.result?.value;
    if (drawn !== undefined) break;
  }
  const view = await send('Page.captureScreenshot', { format: 'png' });
  fs.writeFileSync(file, Buffer.from(view.result.data, 'base64'));
  console.log(JSON.stringify({ out: file, rendered: drawn !== undefined, seconds: (Date.now() - begin) / 1000, messages: [] }));
}
socket.close();
browser.kill();
await new Promise((r) => browser.on('exit', r));
try { fs.rmSync(profile, { recursive: true, force: true }); } catch { /* Chromium may still be writing its profile */ }
