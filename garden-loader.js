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
  try {
    const heap = sim.HEAP16;
    for (let i = 0; i < frames; i++) {
      const L = heap[(ptr >> 1) + i * 2] / 32768;
      const R = heap[(ptr >> 1) + i * 2 + 1] / 32768;
      ringL[ringW % RING_N] = L;
      ringR[ringW % RING_N] = R;
      ringW++;
    }
  } catch (e) { logErr('audio', e); }
}
function logErr(where, e) {
  try { window.__simLog.push(where + ': ' + (e && (e.message || e))); } catch (x) {}
}
function startAudio() {
  // A closed context cannot be resumed. Rebuild it on the next user gesture.
  if (AC && AC.state === 'closed') { AC = null; spn = null; g = null; }
  if (AC) {
    if (AC.state !== 'running') {
      // Schedule the unlock buffer inside the gesture, before resume changes state.
      iosUnlock();
      try {
        const resume = AC.resume();
        if (resume && resume.catch) resume.catch(e => logErr('audio resume', e));
      } catch (e) { logErr('audio resume', e); }
    }
    return;
  }
  try {
    AC = new (window.AudioContext || window.webkitAudioContext)();
    // Start at the freshest audio — anything already queued is stale.
    ringRd = Math.max(0, ringW - 1024);
    spn = AC.createScriptProcessor(2048, 0, 2);
    // The firmware streams 16 kHz stereo; the context runs at ~44.1/48 kHz.
    // Walk the ring with a fractional cursor so pitch stays exact.
    let rpos = 0;
    spn.onaudioprocess = e => {
      const oL = e.outputBuffer.getChannelData(0);
      const oR = e.outputBuffer.getChannelData(1);
      const step = AUD_RATE / AC.sampleRate;
      // Keep the read cursor inside audio actually supplied by the firmware.
      if (ringRd > ringW) ringRd = ringW;
      if (ringW - ringRd > AUD_RATE) ringRd = ringW - AUD_RATE;
      let n = 0;
      const available = Math.max(0, ringW - ringRd);
      for (let i = 0; i < oL.length; i++) {
        rpos += step;
        while (rpos >= 1) { rpos -= 1; n++; }
        let L = 0, R = 0;
        if (n < available) {
          const idx = (ringRd + n) % RING_N;
          L = ringL[idx]; R = ringR[idx];
        }
        oL[i] = L; oR[i] = R;
      }
      ringRd += Math.min(n, available);
    };
    g = AC.createGain(); g.gain.value = ampOn ? 0.8 : 0;
    spn.connect(g); g.connect(AC.destination);
    // The silent source must be started while the context is still suspended
    // and while this call is running inside a trusted user gesture.
    iosUnlock();
    const resume = AC.resume();
    if (resume && resume.catch) resume.catch(e => logErr('audio resume', e));
  } catch (e) {
    logErr('audio init', e);
    try { if (AC && AC.state !== 'closed') AC.close(); } catch (x) {}
    AC = null; spn = null; g = null;
  }
}

// iOS Safari only unwedges WebAudio when a short buffer is played from
// inside the same gesture that created/resumed the context — and that
// gesture must be touchend/click (pointerdown is not always honored).
function iosUnlock() {
  try {
    // Do not reject a suspended context here: that is precisely when the
    // gesture-bound silent-buffer kick needs to be scheduled.
    if (!AC || AC.state === 'closed') return;
    if (!iosUnlock._buf) {
      iosUnlock._buf = AC.createBuffer(1, 1, AC.sampleRate);
      iosUnlock._buf.getChannelData(0)[0] = 0;
    }
    const src = AC.createBufferSource();
    src.buffer = iosUnlock._buf;
    src.connect(AC.destination);
    src.start(0);
  } catch (e) { logErr('audio unlock', e); }
}
// touchend + click unlock audio on iOS; pointerdown covers desktop/Android.
window.addEventListener('touchend', startAudio, { once: false, passive: true });
window.addEventListener('click', startAudio, { once: false });
window.addEventListener('pointerdown', startAudio, { once: false });
// iOS pauses audio contexts when the tab backgrounds; re-kick on return.
document.addEventListener('visibilitychange', () => {
  if (!document.hidden && AC && AC.state === 'suspended') {
    iosUnlock();
    try {
      const resume = AC.resume();
      if (resume && resume.catch) resume.catch(e => logErr('audio resume', e));
    } catch (e) { logErr('audio resume', e); }
  }
});

// ---- NVS (Preferences) + named browser save slots ----
const NVS_KEY = 'wfg-nvs-v42-S3';
const SAVE_SLOTS_KEY = NVS_KEY + '-save-slots-v1';
function cloneSaveData(value) { return JSON.parse(JSON.stringify(value || {})); }
let nvs = {};
try { nvs = JSON.parse(localStorage.getItem(NVS_KEY) || '{}'); } catch (e) { nvs = {}; }
let saveSlots;
try { saveSlots = JSON.parse(localStorage.getItem(SAVE_SLOTS_KEY) || 'null'); } catch (e) { saveSlots = null; }
if (!saveSlots || !Array.isArray(saveSlots.slots) || !saveSlots.slots.length) {
  // One-time migration: preserve the existing game as the first named save.
  saveSlots = { activeId: 'main', slots: [{ id: 'main', name: 'Main Save', data: cloneSaveData(nvs), updatedAt: Date.now() }] };
}
if (!saveSlots.slots.some(s => s.id === saveSlots.activeId)) saveSlots.activeId = saveSlots.slots[0].id;
const activeSaveSlot = () => saveSlots.slots.find(s => s.id === saveSlots.activeId) || saveSlots.slots[0];
function persistSaveSlots() {
  try { localStorage.setItem(SAVE_SLOTS_KEY, JSON.stringify(saveSlots)); }
  catch (e) { console.warn('Could not save named save slots', e); }
}
// If this browser has the new slot index but not the active NVS key, restore
// that slot. Otherwise the legacy NVS key remains the source of truth.
try {
  if (localStorage.getItem(NVS_KEY) === null && activeSaveSlot().data) nvs = cloneSaveData(activeSaveSlot().data);
} catch (e) {}
function snapshotActiveSave() {
  const slot = activeSaveSlot();
  if (!slot) return;
  slot.data = cloneSaveData(nvs);
  slot.updatedAt = Date.now();
  persistSaveSlots();
}
// One-time: raise stored music volume to the new 100% default (old builds shipped 40).
try {
  if (!localStorage.getItem('wfg-mvol-100-v2')) {
    if (typeof nvs.mvol === 'number' && nvs.mvol < 100) nvs.mvol = 100;
    localStorage.setItem('wfg-mvol-100-v2', '1');
  }
  if (!localStorage.getItem('wfg-mvol-migrated')) {
    if (typeof nvs.mvol === 'number' && nvs.mvol < 100) nvs.mvol = 100;
    localStorage.setItem('wfg-mvol-migrated', '1');
  }
} catch (e) {}
let nvsTimer = null;
function nvsFlush(immediate) {
  clearTimeout(nvsTimer);
  const write = () => {
    try { localStorage.setItem(NVS_KEY, JSON.stringify(nvs)); } catch (e) {}
    snapshotActiveSave();
  };
  if (immediate) write(); else nvsTimer = setTimeout(write, 250);
}
// Debug/testing affordance: seed NVS from the URL (?nvs={...json...}) before boot.
try {
  const qNvs = new URLSearchParams(location.search).get('nvs');
  window.__qNvsDebug = qNvs ? ('len=' + qNvs.length) : 'none';
  if (qNvs) { Object.assign(nvs, JSON.parse(qNvs)); window.__qNvsDebug += ' merged started=' + nvs.started; nvsFlush(true); }
} catch (e) { window.__qNvsDebug = 'ERR ' + e.message; }
document.addEventListener('visibilitychange', () => { if (document.hidden) nvsFlush(true); });
window.addEventListener('pagehide', () => nvsFlush(true));

function saveStatus(message) {
  const el = document.getElementById('saveStatus');
  if (el) el.textContent = message;
}
function refreshSaveMenu() {
  const select = document.getElementById('saveSlotSelect');
  const name = document.getElementById('saveNameInput');
  if (!select) return;
  select.innerHTML = '';
  saveSlots.slots.forEach(slot => {
    const option = document.createElement('option');
    option.value = slot.id;
    option.textContent = slot.name || 'Unnamed Save';
    option.selected = slot.id === saveSlots.activeId;
    select.appendChild(option);
  });
  const active = activeSaveSlot();
  if (name && (!name.value || name.dataset.slotId !== active.id)) name.value = active.name;
  if (name) name.dataset.slotId = active.id;
  const del = document.getElementById('saveDeleteBtn');
  if (del) del.disabled = saveSlots.slots.length <= 1;
}
function switchSaveSlot(id) {
  const target = saveSlots.slots.find(s => s.id === id);
  if (!target || id === saveSlots.activeId) return;
  nvsFlush(true);
  saveSlots.activeId = id;
  nvs = cloneSaveData(target.data);
  try { localStorage.setItem(NVS_KEY, JSON.stringify(nvs)); } catch (e) {}
  persistSaveSlots();
  location.reload();
}
function createSaveSlot(name, fresh) {
  nvsFlush(true);
  const cleanName = (name || '').trim() || (fresh ? 'New Game' : 'Save ' + (saveSlots.slots.length + 1));
  const id = 'save-' + Date.now() + '-' + Math.floor(Math.random() * 1000000);
  const data = fresh ? {} : cloneSaveData(nvs);
  saveSlots.slots.push({ id, name: cleanName, data, updatedAt: Date.now() });
  saveSlots.activeId = id;
  nvs = data;
  try { localStorage.setItem(NVS_KEY, JSON.stringify(nvs)); } catch (e) {}
  persistSaveSlots();
  location.reload();
}
function renameSaveSlot(id, name) {
  const slot = saveSlots.slots.find(s => s.id === id);
  const cleanName = (name || '').trim();
  if (!slot || !cleanName) { saveStatus('Enter a save name first.'); return; }
  slot.name = cleanName.slice(0, 32);
  persistSaveSlots(); refreshSaveMenu();
  saveStatus('Save renamed.');
}
function deleteSaveSlot(id) {
  if (saveSlots.slots.length <= 1) { saveStatus('Keep at least one save slot.'); return; }
  const slot = saveSlots.slots.find(s => s.id === id);
  if (!slot || !confirm('Delete the save “' + slot.name + '”? This cannot be undone.')) return;
  const wasActive = id === saveSlots.activeId;
  saveSlots.slots = saveSlots.slots.filter(s => s.id !== id);
  if (wasActive) {
    saveSlots.activeId = saveSlots.slots[0].id;
    nvs = cloneSaveData(saveSlots.slots[0].data);
    try { localStorage.setItem(NVS_KEY, JSON.stringify(nvs)); } catch (e) {}
  }
  persistSaveSlots();
  if (wasActive) location.reload();
  else { refreshSaveMenu(); saveStatus('Save deleted.'); }
}
function bindSaveMenu() {
  const select = document.getElementById('saveSlotSelect');
  const name = document.getElementById('saveNameInput');
  const byId = id => document.getElementById(id);
  if (!select) return;
  refreshSaveMenu();
  select.addEventListener('change', () => switchSaveSlot(select.value));
  byId('saveNowBtn')?.addEventListener('click', () => { nvsFlush(true); saveStatus('Saved ' + activeSaveSlot().name + '.'); });
  byId('saveAsBtn')?.addEventListener('click', () => createSaveSlot(name?.value || '', false));
  byId('saveRenameBtn')?.addEventListener('click', () => renameSaveSlot(select.value, name?.value || ''));
  byId('saveDeleteBtn')?.addEventListener('click', () => deleteSaveSlot(select.value));
  byId('saveNewGameBtn')?.addEventListener('click', () => {
    const suggested = 'New Game ' + (saveSlots.slots.length + 1);
    const newName = (name?.value || '').trim() || suggested;
    if (confirm('Start a fresh game in a new save slot named “' + newName + '”? Your existing saves will be kept.')) createSaveSlot(newName, true);
  });
  byId('saveRebootBtn')?.addEventListener('click', () => { nvsFlush(true); location.reload(); });
  byId('saveWakeBtn')?.addEventListener('click', () => {
    if (!displaySleeping) { saveStatus('The screen is already awake.'); return; }
    startAudio();
    bootDown = true;
    setTimeout(() => { bootDown = false; }, 350);
    saveStatus('Wake signal sent.');
  });
}
window.addEventListener('DOMContentLoaded', bindSaveMenu);

// ---- touch / boot state ----
let touchPanelX = 0, touchPanelY = 0, touchDown = false, bootDown = false;

function panelPoint(ev) {
  const r = screenCv.getBoundingClientRect();
  const px = (ev.clientX - r.left) * PANEL_W / r.width;
  const py = (ev.clientY - r.top) * PANEL_H / r.height;
  return [Math.max(0, Math.min(PANEL_W - 1, Math.round(px))),
          Math.max(0, Math.min(PANEL_H - 1, Math.round(py)))];
}
let bootPulseTimer = null;
function screenNeedsBoot() {
  if (!sim) return true;
  try {
    const fb = sim.HEAPU16, base = sim._garden_fb_ptr() >> 1;
    // Sample the framebuffer only on a user tap, not every animation frame.
    for (let i = 0; i < PANEL_W * PANEL_H; i += 8) if (fb[base + i] !== 0) return false;
    return true;
  } catch (e) { return false; }
}
screenCv.addEventListener('pointerdown', ev => {
  ev.preventDefault(); startAudio();
  // A tap on a blank/asleep screen acts like a short BOOT-button press.
  // Once the firmware has drawn a screen, taps remain ordinary touch input.
  if (displaySleeping || screenNeedsBoot()) {
    bootDown = true;
    clearTimeout(bootPulseTimer);
    bootPulseTimer = setTimeout(() => { bootDown = false; bootPulseTimer = null; }, 350);
  }
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
  // While the firmware is in its sleep loop, this same press is the wake pin.
  bootDown = true;
}
function bootRelease(ev) {
  if (ev) ev.preventDefault();
  if (bootDown) bootDown = false;
}
if (bootBtn) {
  bootBtn.addEventListener('pointerdown', bootPress, { passive: false });
  bootBtn.addEventListener('pointerup', bootRelease, { passive: false });
  bootBtn.addEventListener('pointerleave', () => bootRelease());
}
window.addEventListener('keydown', e => { if (e.key === 'b' && !bootDown) bootPress(); });
window.addEventListener('keyup', e => { if (e.key === 'b') bootRelease(); });

// ---- sleep display ----
let displaySleeping = false;
function onDisplayPower(on) {
  // The firmware callback's true value means the display is in sleep mode.
  // Preserve the original web-port mapping: true dims, false wakes.
  displaySleeping = !!on;
  screenCv.style.filter = on ? 'brightness(0.25)' : 'none';
  const wake = document.getElementById('saveWakeBtn');
  if (wake) wake.disabled = !displaySleeping;
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
  try {
    // The framebuffer is a static array inside the wasm module — read it at the
    // address the shim exported, never at offset 0.
    const fbBase = sim._garden_fb_ptr() >> 1;
    const fb = sim.HEAPU16, N = PANEL_W * PANEL_H;
    const lut = invert ? INV_LUT : LUT;
    for (let i = 0; i < N; i++) pix32[i] = lut[fb[fbBase + i]];
    sctx.putImageData(img, 0, 0);
  } catch (e) { logErr('blit', e); }
}

function rafLoop() {
  if (runStarted && sim) blit();   // blit every frame — the sim redraws async inside wasm
  requestAnimationFrame(rafLoop);
}
requestAnimationFrame(rafLoop);

// ---- hero leaf + version badge ----
const LEAF = new Uint8Array([0,0,0,0,0,0,0,127,128,1,255,224,7,128,120,14,0,28,28,0,12,24,0,192,56,31,192,48,120,128,48,193,128,49,129,128,49,1,128,49,33,0,50,67,0,26,198,0,24,140,0,28,248,12,13,240,28,3,0,120,7,255,224,14,127,128,0,0,0,0,0,0]);
(function heroLeaf() {
  const cv = document.getElementById('leafCv');
  if (!cv) return;
  const lc = cv.getContext('2d');
  lc.fillStyle = '#46d47e';
  for (let r = 0; r < 24; r++) for (let c = 0; c < 24; c++)
    if (LEAF[(r * 24 + c) >> 3] & (0x80 >> ((r * 24 + c) & 7))) lc.fillRect(c, r, 1, 1);
})();

// ---- strain catalog table (exact firmware data) ----
const strainNames = ["Northern Lights","Blue Dream","Banana Papaya","Superbuff Cherry","Jack Herer","Granddaddy Purple","White Widow","Blue Lobster","OG Kush","Sour Diesel","Gelato","Titan Express","Zesty Garlic Cookies","Moon Beam Gelato","Toad Venom","Foreign Kushmints"];
const strainType = ["INDICA","SATIVA","HYBRID","HYBRID","SATIVA","INDICA","HYBRID","HYBRID","INDICA","SATIVA","HYBRID","HYBRID","HYBRID","HYBRID","HYBRID","INDICA"];
const strainNotes = ["Earthy pine body calm","Sweet berry heady lift","Tropical banana cream","Cherry gas social buzz","Pine citrus bright focus","Grape berry cozy body","Pepper earth steady buzz","Blue candy gas","Fuel pine couch calm","Diesel citrus daytime fuel","Creamy dessert calm","Fast gas express rush","Garlic cookie funk","Moonlit cream dessert","Swamp funk mind melt","Minty gas deep melt"];
const strainDifficulty = ["EASY","MEDIUM","MEDIUM","MEDIUM","MEDIUM","MEDIUM","MEDIUM","HARD","HARD","HARD","HARD","HARD","HARD","HARD","EXPERT","EXPERT"];
const strainPace = [1,0,0,0,0,0,0,-1,-1,-1,-1,-1,-1,-1,-2,-2];
const strainThirst = [14,18,18,18,18,18,18,24,24,24,24,24,24,24,32,32];
const strainSeedPrice = [8,12,12,12,12,12,12,18,18,18,18,18,18,18,30,30];
const strainFlowerValue = [10,14,14,14,14,14,14,20,20,20,20,20,20,20,32,32];
const strainHashValue = [25,35,35,35,35,35,35,50,50,50,50,50,50,50,80,80];
(function fillStrains() {
  const tb = document.getElementById('strainTable');
  if (!tb) return;
  tb.innerHTML = '<tr><th>STRAIN</th><th>TYPE</th><th>SEED</th><th>FLOWER</th><th>HASH</th><th>PACE</th><th>DIFFICULTY</th></tr>';
  const paceTxt = p => p > 0 ? '+' + p + ' FAST' : p < 0 ? p + ' SLOW' : 'NORMAL';
  strainNames.forEach((n, i) => {
    const tr = document.createElement('tr');
    tr.innerHTML = `<td><b>${n}</b><br><small>${strainNotes[i]}</small></td><td>${strainType[i]}</td>` +
      `<td>$${strainSeedPrice[i]}</td><td>$${strainFlowerValue[i]}</td><td>$${strainHashValue[i]}</td>` +
      `<td>${paceTxt(strainPace[i])} · ${strainThirst[i]}% thirst</td><td>${strainDifficulty[i]}</td>`;
    tb.appendChild(tr);
  });
})();

// ---- fullscreen (native API + orientation lock, CSS fallback for iOS) ----
const device = document.getElementById('device'), fsBtn = document.getElementById('fsBtn');
function isFs() { return (document.fullscreenElement === device) || device.classList.contains('fs'); }
// Page lock while fullscreen: phones scroll/pinch-zoom/double-tap-zoom out of
// the fullscreen view otherwise (iOS Safari especially). Block all gestures.
let fsGestureBlock = null;
function fsLockOn() {
  document.documentElement.style.overflow = 'hidden';
  document.body.style.overflow = 'hidden';
  const stop = e => { e.preventDefault(); };
  document.addEventListener('touchmove', stop, { passive: false });
  document.addEventListener('gesturestart', stop, { passive: false });
  document.addEventListener('gesturechange', stop, { passive: false });
  fsGestureBlock = stop;
}
function fsLockOff() {
  document.documentElement.style.overflow = '';
  document.body.style.overflow = '';
  if (fsGestureBlock) {
    document.removeEventListener('touchmove', fsGestureBlock);
    document.removeEventListener('gesturestart', fsGestureBlock);
    document.removeEventListener('gesturechange', fsGestureBlock);
    fsGestureBlock = null;
  }
}
async function toggleFs() {
  if (isFs()) {
    try { if (document.fullscreenElement) await document.exitFullscreen(); } catch (e) {}
    device.classList.remove('fs');
    try { if (screen.orientation && screen.orientation.unlock) screen.orientation.unlock(); } catch (e) {}
    fsBtn.textContent = '⛶ FULLSCREEN';
    fsLockOff();
    return;
  }
  fsLockOn();
  try {
    if (device.requestFullscreen) {
      await device.requestFullscreen({ navigationUI: 'hide' });
      try { if (screen.orientation && screen.orientation.lock) await screen.orientation.lock('landscape'); } catch (e) {}
    } else throw new Error('no native fs');
    fsBtn.textContent = '✕ EXIT';
  } catch (e) {
    device.classList.add('fs');       // iOS: pseudo-fullscreen
    try { if (screen.orientation && screen.orientation.lock) await screen.orientation.lock('landscape'); } catch (e2) {}
    fsBtn.textContent = '✕ EXIT';
  }
}
if (fsBtn) fsBtn.addEventListener('click', toggleFs);
const fsExit = document.getElementById('fsExit');
if (fsExit) fsExit.addEventListener('click', toggleFs);
document.addEventListener('fullscreenchange', () => {
  if (!document.fullscreenElement) { device.classList.remove('fs'); if (fsBtn) fsBtn.textContent = '⛶ FULLSCREEN'; fsLockOff(); }
  else fsLockOn();
});

// ---- boot the firmware ----
// Sound hint: browsers can't start audio before the first user gesture; hide
// the pill the moment audio actually starts.
const sndHint = document.getElementById('sndHint');
if (sndHint) {
  const hideHint = () => { sndHint.classList.add('gone'); window.removeEventListener('pointerdown', hideHint, true); };
  window.addEventListener('pointerdown', hideHint, true);
}

createGardenSim({
  locateFile: f => f + '?b=' + Date.now(),   // never serve a stale wasm from cache
  gardenDirty: true,
  t0: Date.now(),   // millis() = Date.now() - t0 must fit in uint32 — never performance.now()
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
  window.addEventListener('error', e => logErr('page', e.error || e.message));
  window.addEventListener('unhandledrejection', e => logErr('rejection', e.reason));
  // main() runs setup() then loops forever (asyncified between frames)
  try { const r = M._main(); if (r && r.catch) r.catch(e => { logErr('MAIN ABORT', e); }); runStarted = true; }
  catch (e) { logErr('crash', e); }
  // landing-page version badges (the sim's own OTA screen does the real check)
  fetch('https://github.com/VYBN3D/WinFloridaGardenSim/releases/latest/download/version.txt')
    .then(r => r.ok ? r.text() : Promise.reject())
    .then(t => {
      const v = t.trim();
      const el = document.getElementById('verNum');
      const ab = document.getElementById('aboutVer');
      if (el) el.textContent = v + '-S3';
      if (ab) ab.textContent = v + '-S3';
    }).catch(() => {});
});
