// WinFlorida Garden Sim — WASM runtime loader.
// Runs the REAL firmware (compiled from the sketch) against browser HAL.
let sim = null, blitDirty = true, runStarted = false;
const PANEL_W = 480, PANEL_H = 320;
const screenCv = document.getElementById('screen');
const sctx = screenCv.getContext('2d', { alpha: false });
screenCv.width = PANEL_W; screenCv.height = PANEL_H;
const img = sctx.createImageData(PANEL_W, PANEL_H);
const pix32 = new Uint32Array(img.data.buffer);

// ---- audio ring (16 kHz stereo, straight from the firmware's I2S stream) ----
const AUD_RATE = 16000;
const RING_N = 65536; // stereo pairs (~4 s)
const ringL = new Float32Array(RING_N);
const ringR = new Float32Array(RING_N);
let ringW = 0, ringRd = 0, AC = null, ampOn = false, spn = null;

function audioPush(ptr, frames) {
  if (!sim) return;
  const heap = sim.HEAP16;
  for (let i = 0; i < frames; i++) {
    const L = heap[(ptr >> 1) + i * 2] / 32768;
    const R = heap[(ptr >> 1) + i * 2 + 1] / 32768;
    ringL[ringW % RING_N] = L;
    ringR[ringW % RING_N] = R;
    ringW++;
  }
}
function startAudio() {
  if (AC) { if (AC.state === 'suspended') AC.resume(); return; }
  try {
    AC = new (window.AudioContext || window.webkitAudioContext)();
    spn = AC.createScriptProcessor(2048, 0, 2);
    // The firmware streams 16 kHz stereo; the context runs at ~44.1/48 kHz.
    // Walk the ring with a fractional cursor so pitch stays exact.
    let rpos = 0;
    spn.onaudioprocess = e => {
      const oL = e.outputBuffer.getChannelData(0);
      const oR = e.outputBuffer.getChannelData(1);
      const step = AUD_RATE / AC.sampleRate;
      let n = 0;
      for (let i = 0; i < oL.length; i++) {
        rpos += step;
        while (rpos >= 1) { rpos -= 1; n++; }
        let L = 0, R = 0;
        if (ringRd + n <= ringW) {
          L = ringL[(ringRd + n) % RING_N];
          R = ringR[(ringRd + n) % RING_N];
        }
        oL[i] = L; oR[i] = R;
      }
      ringRd += n;
    };
    g = AC.createGain(); g.gain.value = ampOn ? 0.8 : 0;
    spn.connect(g); g.connect(AC.destination);
    AC.resume();
  } catch (e) {}
}
window.addEventListener('pointerdown', startAudio, { once: false });

// ---- NVS (Preferences) → localStorage ----
const NVS_KEY = 'wfg-nvs-v42-S3';
let nvs = {};
try { nvs = JSON.parse(localStorage.getItem(NVS_KEY) || '{}'); } catch (e) { nvs = {}; }
let nvsTimer = null;
function nvsFlush() {
  clearTimeout(nvsTimer);
  nvsTimer = setTimeout(() => { try { localStorage.setItem(NVS_KEY, JSON.stringify(nvs)); } catch (e) {} }, 250);
}

// ---- touch / boot state ----
let touchPanelX = 0, touchPanelY = 0, touchDown = false, bootDown = false;

function panelPoint(ev) {
  const r = screenCv.getBoundingClientRect();
  const px = (ev.clientX - r.left) * PANEL_W / r.width;
  const py = (ev.clientY - r.top) * PANEL_H / r.height;
  return [Math.max(0, Math.min(PANEL_W - 1, Math.round(px))),
          Math.max(0, Math.min(PANEL_H - 1, Math.round(py)))];
}
screenCv.addEventListener('pointerdown', ev => {
  ev.preventDefault(); startAudio();
  const [x, y] = panelPoint(ev);
  touchPanelX = x; touchPanelY = y; touchDown = true;
});
window.addEventListener('pointermove', ev => {
  if (!touchDown) return;
  const [x, y] = panelPoint(ev);
  touchPanelX = x; touchPanelY = y;
});
window.addEventListener('pointerup', () => { touchDown = false; });
window.addEventListener('pointercancel', () => { touchDown = false; });

// BOOT: press/release events (the wasm loop reads the pin each pass)
const bootBtn = document.getElementById('bootBtn');
function bootPress(ev) {
  if (ev) ev.preventDefault();
  startAudio();
  if (!bootDown) { bootDown = true; if (sim) sim._touchBoot(1); }
}
function bootRelease(ev) {
  if (ev) ev.preventDefault();
  if (bootDown) { bootDown = false; if (sim) sim._touchBoot(0); }
}
if (bootBtn) {
  bootBtn.addEventListener('pointerdown', bootPress, { passive: false });
  bootBtn.addEventListener('pointerup', bootRelease, { passive: false });
  bootBtn.addEventListener('pointerleave', () => bootRelease());
}
window.addEventListener('keydown', e => { if (e.key === 'b' && !bootDown) bootPress(); });
window.addEventListener('keyup', e => { if (e.key === 'b') bootRelease(); });

// ---- sleep display ----
function onDisplayPower(on) {
  screenCv.style.filter = on ? 'brightness(0.25)' : 'none';
}
function onInvert(i) { invert = !!i; }
function onAmp(on) { ampOn = !!on; if (g) g.gain.value = on ? 0.8 : 0; }
let g = null;

// ---- blit: RGB565 framebuffer → canvas ----
let invert = false;
const LUT = new Uint32Array(65536);
for (let c = 0; c < 65536; c++) {
  const r = ((c >> 11) & 31) * 255 / 31 | 0, g = ((c >> 5) & 63) * 255 / 63 | 0, b = (c & 31) * 255 / 31 | 0;
  LUT[c] = 0xFF000000 | (b << 16) | (g << 8) | r;
}
const INV_LUT = new Uint32Array(65536);
for (let c = 0; c < 65536; c++) INV_LUT[c] = LUT[c ^ 0xFFFF];
function blit() {
  if (!sim) return;
  const fb = sim.HEAPU16, N = PANEL_W * PANEL_H;
  const lut = invert ? INV_LUT : LUT;
  for (let i = 0; i < N; i++) pix32[i] = lut[fb[i]];
  sctx.putImageData(img, 0, 0);
}

function rafLoop() {
  if (runStarted && blitDirty) { blit(); blitDirty = false; }
  requestAnimationFrame(rafLoop);
}
requestAnimationFrame(rafLoop);

// ---- boot the firmware ----
createGardenSim({
  gardenDirty: true,
  t0: performance.now(),
  audioPush,
  audioLevel: () => (ampOn ? 1 : 0),
  touchCount: () => (touchDown ? 1 : 0),
  touchPanelPoint: out => {
    sim.HEAP32[out >> 2] = touchPanelX;
    sim.HEAP32[(out >> 2) + 1] = touchPanelY;
  },
  bootPressed: () => bootDown,
  nvsSet: (k, v) => {
    if (v === '\x01') delete nvs[k]; else nvs[k] = v;
    nvsFlush();
  },
  nvsGet: k => (k in nvs ? nvs[k] : ''),
  nvsClear: () => { nvs = {}; nvsFlush(); },
  httpGet: async url => {
    try {
      const r = await fetch(url, { redirect: 'follow' });
      const body = await r.text();
      return { status: r.status, body };
    } catch (e) { return { status: 0, body: '' }; }
  },
  onDisplayPower,
  onReboot: () => location.reload(),
  onInvert, onAmp,
}).then(M => {
  sim = M;
  window.__sim = M; window.__simLog = [];
  // main() runs setup() then loops forever (asyncified between frames)
  try { const r = M._main(); if (r && r.catch) r.catch(e => { console.error('MAIN ABORT', e); __simLog.push('abort:' + e); }); runStarted = true; }
  catch (e) { console.error('sim crash', e); __simLog.push('crash:' + e); }
  // landing-page version badge (the sim's own OTA screen does the real check)
  fetch('https://github.com/VYBN3D/WinFloridaGardenSim/releases/latest/download/version.txt')
    .then(r => r.ok ? r.text() : Promise.reject())
    .then(t => {
      const v = t.trim();
      const el = document.getElementById('verNum');
      if (el) el.textContent = v + '-S3';
    }).catch(() => {});
});
