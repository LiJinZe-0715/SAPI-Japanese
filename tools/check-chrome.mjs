// Verifies the SAPI engine through the browser, which is the whole point of the project.
//
// Launches Chrome with a throwaway profile and a DevTools port, then asks the page for
// speechSynthesis.getVoices() and speaks one sentence with a non-default pitch and rate, so the
// path Chrome uses for them (SetRate, and <pitch> XML around the text) is exercised too. Chrome
// reaches SAPI through tts_win.cc, so a voice list here is the same list the real site will see.
//
//   node tools/check-chrome.mjs [--headed]
//
// Exit code 0 when a Japanese voice is listed, the utterance ends and at least one boundary
// event fired; 1 otherwise. Chrome and the temp profile are cleaned up either way.

import { spawn } from 'node:child_process';
import { mkdtempSync, rmSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';

const CHROME = process.env.CHROME || 'C:/Program Files/Google/Chrome/Application/chrome.exe';
const PORT = 9333;
const headed = process.argv.includes('--headed');

const sleep = (ms) => new Promise((resolve) => setTimeout(resolve, ms));

async function firstPage() {
  for (let attempt = 0; attempt < 60; attempt++) {
    try {
      const list = await (await fetch(`http://127.0.0.1:${PORT}/json/list`)).json();
      const page = list.find((t) => t.type === 'page');
      if (page) return page;
    } catch {
      // DevTools is not listening yet; keep polling.
    }
    await sleep(250);
  }
  throw new Error('Chrome did not open a DevTools endpoint');
}

function connect(url) {
  const socket = new WebSocket(url);
  const pending = new Map();
  let nextId = 0;
  socket.addEventListener('message', (event) => {
    const message = JSON.parse(event.data);
    const settle = pending.get(message.id);
    if (settle) {
      pending.delete(message.id);
      settle(message);
    }
  });
  const ready = new Promise((resolve, reject) => {
    socket.addEventListener('open', resolve, { once: true });
    socket.addEventListener('error', reject, { once: true });
  });
  return {
    ready,
    call(method, params = {}) {
      const id = ++nextId;
      return new Promise((resolve) => {
        pending.set(id, resolve);
        socket.send(JSON.stringify({ id, method, params }));
      });
    },
    close: () => socket.close(),
  };
}

async function evaluate(client, expression) {
  const reply = await client.call('Runtime.evaluate',
                                  { expression, awaitPromise: true, returnByValue: true });
  if (reply.result?.exceptionDetails) {
    throw new Error(reply.result.exceptionDetails.text);
  }
  return reply.result?.result?.value;
}

async function check() {
  const page = await firstPage();
  const client = connect(page.webSocketDebuggerUrl);
  await client.ready;
  try {
    const voices = JSON.parse(await evaluate(client, `(async () => {
      // getVoices() is empty until the platform list has been fetched; voiceschanged is the
      // signal Chrome gives when it is ready.
      if (speechSynthesis.getVoices().length === 0) {
        await new Promise((resolve) => {
          speechSynthesis.addEventListener('voiceschanged', resolve, { once: true });
          setTimeout(resolve, 5000);
        });
      }
      return JSON.stringify(speechSynthesis.getVoices().map(
          (v) => ({ name: v.name, lang: v.lang, local: v.localService })));
    })()`));

    console.log(`${voices.length} voice(s) visible to Chrome:`);
    for (const voice of voices) {
      console.log(`  ${voice.lang}  ${voice.name}${voice.local ? '' : '  (network)'}`);
    }

    const target = voices.find((v) => v.lang === 'ja-JP' && v.local);
    if (!target) {
      console.error('no Japanese voice; the engine is not registered for this browser');
      return false;
    }

    const spoken = JSON.parse(await evaluate(client, `(async () => {
      const utter = new SpeechSynthesisUtterance('こんにちは。今日はいい天気ですね。');
      utter.voice = speechSynthesis.getVoices().find((v) => v.name === ${JSON.stringify(target.name)});
      utter.lang = ${JSON.stringify(target.lang)};
      utter.pitch = 1.5;
      utter.rate = 1.5;
      const boundaries = [];
      utter.onboundary = (event) => boundaries.push(event.name + '@' + event.charIndex);
      const outcome = await new Promise((resolve) => {
        utter.onend = () => resolve('end');
        utter.onerror = (event) => resolve('error: ' + (event.error || 'unknown'));
        speechSynthesis.speak(utter);
        setTimeout(() => resolve('timeout'), 30000);
      });
      return JSON.stringify({ outcome, boundaries });
    })()`));
    console.log(`speak("${target.name}") -> ${spoken.outcome}, boundaries ${JSON.stringify(spoken.boundaries)}`);
    return spoken.outcome === 'end' && spoken.boundaries.length > 0;
  } finally {
    client.close();
  }
}

const profile = mkdtempSync(join(tmpdir(), 'voicevox-tts-'));
const chrome = spawn(
    CHROME,
    [
      headed ? '--window-position=40,40' : '--headless=new',
      `--remote-debugging-port=${PORT}`,
      `--user-data-dir=${profile}`,
      '--no-first-run',
      '--no-default-browser-check',
      '--autoplay-policy=no-user-gesture-required',
      'about:blank',
    ],
    { stdio: 'ignore' });

let passed = false;
try {
  passed = await check();
} catch (error) {
  console.error(`check failed: ${error.message}`);
} finally {
  chrome.kill();
  await sleep(300);
  try {
    rmSync(profile, { recursive: true, force: true });
  } catch {
    // Chrome may still hold a handle; a temp profile is not worth chasing.
  }
}
process.exitCode = passed ? 0 : 1;
