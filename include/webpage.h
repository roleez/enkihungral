#pragma once
#include <Arduino.h>

// ════════════════════════════════════════════════════════════════
//  Enki Hungrál – Weblap (PROGMEM)
//  Tartalom: akkufesz, alvási idő beállítás, szín lista, mentés, OTA
// ════════════════════════════════════════════════════════════════

const char INDEX_HTML[] PROGMEM = R"rawhtml(
<!DOCTYPE html>
<html lang="hu">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1.0">
<title>Enki Hungrál</title>
<style>
  /* ── Betűtípus ─────────────────────────────────── */
  @import url('https://fonts.googleapis.com/css2?family=Share+Tech+Mono&family=Exo+2:wght@300;500;700&display=swap');

  /* ── CSS változók ──────────────────────────────── */
  :root {
    --bg:      #0d1117;
    --surface: #161b22;
    --border:  #30363d;
    --accent:  #58a6ff;
    --accent2: #3fb950;
    --danger:  #f85149;
    --warn:    #d29922;
    --text:    #e6edf3;
    --muted:   #8b949e;
    --mono:    'Share Tech Mono', monospace;
    --sans:    'Exo 2', sans-serif;
    --radius:  6px;
    --glow:    0 0 12px rgba(88,166,255,0.35);
    font-size: 18px;
  }

  * { box-sizing: border-box; margin: 0; padding: 0; }

  body {
    background: var(--bg);
    color: var(--text);
    font-family: var(--sans);
    font-weight: 300;
    min-height: 100vh;
    display: flex;
    justify-content: center;
    padding: 1.5rem 1rem 4rem;
  }

  .container {
    width: 100%;
    max-width: 620px;
  }

  /* ── Fejléc ────────────────────────────────────── */
  header {
    display: flex;
    align-items: center;
    gap: 0.8rem;
    margin-bottom: 1.8rem;
    border-bottom: 1px solid var(--border);
    padding-bottom: 0.8rem;
  }
  .logo-dot {
    width: 12px; height: 12px;
    border-radius: 50%;
    background: var(--accent);
    box-shadow: var(--glow);
    flex-shrink: 0;
  }
  h1 {
    font-family: var(--mono);
    font-size: 1.05rem;
    letter-spacing: 0.12em;
    color: var(--accent);
  }
  .ws-badge {
    margin-left: auto;
    font-family: var(--mono);
    font-size: 0.68rem;
    padding: 3px 10px;
    border-radius: 20px;
    border: 1px solid var(--border);
    color: var(--muted);
    transition: all 0.3s;
    white-space: nowrap;
  }
  .ws-badge.connected { color: var(--accent2); border-color: var(--accent2); }

  /* ── Aktív szín státusz sor ───────────────────── */
  .active-color-bar {
    display: flex;
    align-items: center;
    gap: 0.8rem;
    background: var(--surface);
    border: 1px solid var(--border);
    border-radius: var(--radius);
    padding: 0.6rem 1rem;
    margin-bottom: 1.2rem;
  }
  /* ── Akkufeszültség kártya ──────────────────────── */
  .batt-card {
    background: var(--surface);
    border: 1px solid var(--border);
    border-radius: var(--radius);
    padding: 1rem 1.2rem;
    margin-bottom: 1.2rem;
    display: flex;
    align-items: center;
    gap: 1rem;
  }
  .batt-icon {
    font-size: 1.6rem;
    flex-shrink: 0;
  }
  .batt-label {
    font-family: var(--mono);
    font-size: 0.72rem;
    color: var(--muted);
    text-transform: uppercase;
    letter-spacing: 0.08em;
  }
  .batt-value {
    font-family: var(--mono);
    font-size: 2rem;
    font-weight: 700;
    color: var(--accent2);
    line-height: 1;
    letter-spacing: 0.05em;
  }
  .batt-value.warn  { color: var(--warn); }
  .batt-value.low   { color: var(--danger); }

  /* ── Szekció kártya ─────────────────────────────── */
  .card {
    background: var(--surface);
    border: 1px solid var(--border);
    border-radius: var(--radius);
    padding: 1rem 1.2rem;
    margin-bottom: 1rem;
  }
  .card-title {
    font-family: var(--mono);
    font-size: 0.72rem;
    color: var(--muted);
    text-transform: uppercase;
    letter-spacing: 0.08em;
    margin-bottom: 0.7rem;
  }

  /* ── Alvási idő sor ─────────────────────────────── */
  .sleep-row {
    display: flex;
    align-items: center;
    gap: 0.8rem;
  }
  .sleep-row label {
    font-size: 0.85rem;
    color: var(--text);
    flex: 1;
  }
  .sleep-row input[type="number"] {
    width: 80px;
  }
  .sleep-row span {
    font-family: var(--mono);
    font-size: 0.8rem;
    color: var(--muted);
  }

  /* ── Táblázat fejléc ────────────────────────────── */
  .table-header {
    display: grid;
    grid-template-columns: 1fr 64px 64px 64px 84px 34px;
    gap: 5px;
    padding: 0 4px 6px;
    font-family: var(--mono);
    font-size: 0.66rem;
    color: var(--muted);
    letter-spacing: 0.06em;
    border-bottom: 1px solid var(--border);
    margin-bottom: 6px;
  }

  /* ── Szín sorok ─────────────────────────────────── */
  #color-list { display: flex; flex-direction: column; gap: 5px; }

  .color-row {
    display: grid;
    grid-template-columns: 1fr 64px 64px 64px 84px 34px;
    gap: 5px;
    align-items: center;
    padding: 7px 5px;
    background: var(--bg);
    border: 1px solid var(--border);
    border-radius: var(--radius);
    transition: border-color 0.2s, box-shadow 0.2s;
    cursor: pointer;
  }
  .color-row.active-row {
    border-color: var(--accent);
    box-shadow: var(--glow);
  }

  /* ── Szín előnézet pötty a sor bal szélén ───────── */
  .color-dot {
    width: 10px; height: 10px;
    border-radius: 50%;
    display: inline-block;
    margin-right: 5px;
    flex-shrink: 0;
    vertical-align: middle;
    border: 1px solid rgba(255,255,255,0.15);
  }

  /* ── Inputok ────────────────────────────────────── */
  input[type="text"],
  input[type="number"] {
    width: 100%;
    background: var(--surface);
    border: 1px solid var(--border);
    border-radius: var(--radius);
    color: var(--text);
    font-family: var(--mono);
    font-size: 0.78rem;
    padding: 5px 7px;
    outline: none;
    transition: border-color 0.2s;
  }
  input:focus { border-color: var(--accent); }
  input[type="number"] { text-align: center; }
  input[type=number]::-webkit-inner-spin-button,
  input[type=number]::-webkit-outer-spin-button { -webkit-appearance: none; }
  input[type=number] { -moz-appearance: textfield; }

  /* ── Gombok ─────────────────────────────────────── */
  button {
    cursor: pointer;
    border: none;
    border-radius: var(--radius);
    font-family: var(--mono);
    font-size: 0.8rem;
    transition: background 0.2s, transform 0.1s;
  }
  button:active { transform: scale(0.95); }

  .btn-del {
    width: 34px; height: 34px;
    background: transparent;
    border: 1px solid var(--danger);
    color: var(--danger);
    font-size: 1rem;
  }
  .btn-del:hover { background: rgba(248,81,73,0.15); }

  .btn-add {
    margin-top: 8px;
    width: 34px; height: 34px;
    background: transparent;
    border: 1px solid var(--accent2);
    color: var(--accent2);
    font-size: 1.3rem;
    line-height: 1;
  }
  .btn-add:hover { background: rgba(63,185,80,0.15); }
  .btn-add:disabled { opacity: 0.3; cursor: not-allowed; }

  /* ── Alsó gomb sor ──────────────────────────────── */
  .btn-row {
    display: flex;
    gap: 0.7rem;
    margin-top: 1.4rem;
    flex-wrap: wrap;
  }

  .btn-save {
    flex: 1;
    padding: 10px 20px;
    background: var(--accent);
    color: #0d1117;
    font-weight: 700;
    font-size: 0.88rem;
    letter-spacing: 0.08em;
    box-shadow: var(--glow);
  }
  .btn-save:hover { background: #79b8ff; }

  .btn-ota {
    padding: 10px 20px;
    background: transparent;
    border: 1px solid var(--warn);
    color: var(--warn);
    font-size: 0.82rem;
    letter-spacing: 0.05em;
  }
  .btn-ota:hover { background: rgba(210,153,34,0.15); }

  /* ── Toast ──────────────────────────────────────── */
  #toast {
    position: fixed;
    bottom: 1.5rem; right: 1.5rem;
    background: var(--accent2);
    color: #0d1117;
    font-family: var(--mono);
    font-size: 0.78rem;
    padding: 8px 18px;
    border-radius: 20px;
    opacity: 0;
    transform: translateY(10px);
    transition: opacity 0.3s, transform 0.3s;
    pointer-events: none;
    z-index: 999;
  }
  #toast.show { opacity: 1; transform: translateY(0); }

  /* ── Reszponzív ─────────────────────────────────── */
  @media (max-width: 500px) {
    :root { font-size: 14px; }
    .table-header { display: none; }
    .color-row {
      grid-template-columns: 1fr 34px;
      grid-template-rows: auto auto;
      gap: 6px;
    }
    .inp-name { grid-column: 1; }
    .btn-del  { grid-column: 2; grid-row: 1 / 3; align-self: center; }
    .pwm-group {
      grid-column: 1;
      display: flex !important;
      gap: 5px;
      flex-wrap: wrap;
    }
    .pwm-group input { flex: 1; min-width: 55px; }
  }
</style>
</head>
<body>
<div class="container">

<!-- Fejléc -->
<header>
  <div class="logo-dot"></div>
  <h1>ENKI HUNGRÁL</h1>
  <div class="ws-badge" id="wsBadge">● OFFLINE</div>
</header>

<!-- Aktív szín státusz sor -->
<div class="active-color-bar" id="activeColorBar">
  <span class="color-dot" id="activeBarDot" style="width:35px;height:35px;margin-right:8px"></span>
  <span id="activeBarName" style="font-family:var(--mono);font-size:0.85rem;flex:1">—</span>
  <span style="font-family:var(--mono);font-size:0.78rem;color:var(--muted)">
    R:<span id="activeBarR">—</span>
    G:<span id="activeBarG">—</span>
    B:<span id="activeBarB">—</span>
    Freq:<span id="activeBarF">—</span>
  </span>
</div>

<!-- Akkufeszültség -->
<div class="batt-card">
  <div class="batt-icon">🔋</div>
  <div>
    <div class="batt-label">Akkumulátor feszültsége</div>
    <div class="batt-value" id="battValue">— V</div>
  </div>
</div>

<!-- Alvási idő beállítás -->
<div class="card">
  <div class="card-title">Világítási idő</div>
  <div class="sleep-row">
    <label>Alvásig hátralévő idő (perc)</label>
    <input type="number" id="sleepInput" min="5" max="60" value="10">
    <span>perc</span>
  </div>
</div>

<!-- Szín lista -->
<div class="card">
  <div class="card-title">Színek (max 25)</div>

  <!-- Fejléc – desktop -->
  <div class="table-header">
    <span>Név</span>
    <span style="text-align:center">R</span>
    <span style="text-align:center">G</span>
    <span style="text-align:center">B</span>
    <span style="text-align:center">Hz</span>
    <span></span>
  </div>

  <div id="color-list"></div>

  <!-- + gomb -->
  <button class="btn-add" id="btnAdd" title="Szín hozzáadása">+</button>
</div>

<!-- Alsó gombok -->
<div class="btn-row">
  <button class="btn-save" id="btnSave">BEÁLLÍTÁSOK MENTÉSE</button>
  <button class="btn-ota"  id="btnOta">Rendszerfrissítés</button>
</div>

</div><!-- /.container -->

<div id="toast">Mentve ✓</div>

<script>
// ────────────────────────────────────────────
//  Állapot
// ────────────────────────────────────────────
let colors      = [];
let activeIndex = -1;
let sleepM      = 10;
let ws          = null;

// ────────────────────────────────────────────
//  WebSocket
// ────────────────────────────────────────────
function connectWS() {
  ws = new WebSocket('ws://' + location.hostname + '/ws');

  ws.onopen = () => {
    document.getElementById('wsBadge').textContent = '● ONLINE';
    document.getElementById('wsBadge').classList.add('connected');
  };
  ws.onclose = () => {
    document.getElementById('wsBadge').textContent = '● OFFLINE';
    document.getElementById('wsBadge').classList.remove('connected');
    setTimeout(connectWS, 3000);
  };
  ws.onerror = () => ws.close();

  ws.onmessage = (evt) => {
    try {
      const d = JSON.parse(evt.data);
      applyState(d);
    } catch(e) { /* nem JSON */ }
  };
}

function wsSend(msg) {
  if (ws && ws.readyState === WebSocket.OPEN) ws.send(msg);
}

// ────────────────────────────────────────────
//  Állapot alkalmazása (HTTP és WS is használja)
// ────────────────────────────────────────────
function applyState(d) {
  if (d.batt !== undefined) {
    const v    = parseFloat(d.batt);
    const el   = document.getElementById('battValue');
    el.textContent = v.toFixed(2) + ' V';
    el.className   = 'batt-value' +
      (v < 3.5 ? ' low' : v < 3.8 ? ' warn' : '');
  }
  if (d.sleepm !== undefined) {
    sleepM = d.sleepm;
    document.getElementById('sleepInput').value = sleepM;
  }
  if (d.colors) {
    colors      = d.colors;
    activeIndex = d.activeIndex ?? -1;

    // Ne rendereljük újra a listát, ha épp egy input mezőben van a fókusz
    const focused = document.activeElement;
    const isEditingList = focused &&
      focused.tagName === 'INPUT' &&
      focused.closest('#color-list');

    if (!isEditingList) {
      renderList();
    }
    updateActiveDot();
  }
}

// ────────────────────────────────────────────
//  Oldalbetöltés: adatok HTTP-n
// ────────────────────────────────────────────
async function loadStatus() {
  try {
    const res = await fetch('/status');
    const d   = await res.json();
    applyState(d);
  } catch(e) {
    console.error('Status hiba:', e);
  }
}

// ────────────────────────────────────────────
//  Lista renderelése
// ────────────────────────────────────────────
function renderList() {
  const list = document.getElementById('color-list');
  list.innerHTML = '';

  colors.forEach((c, idx) => {
    const row = document.createElement('div');
    row.className  = 'color-row' + (idx === activeIndex ? ' active-row' : '');
    row.dataset.idx = idx;

    const dotColor = `rgb(${c.r},${c.g},${c.b})`;

    row.innerHTML = `
      <div style="display:flex;align-items:center;min-width:0">
        <span class="color-dot" id="dot${idx}"
              style="background:${dotColor};box-shadow:0 0 6px ${dotColor}88"></span>
        <input class="inp-name" type="text" value="${escHtml(c.name)}"
               maxlength="31" placeholder="Szín neve" data-idx="${idx}"
               style="flex:1;min-width:0">
      </div>
      <div class="pwm-group" style="display:contents">
        <input type="number" class="inp-r" value="${c.r}" min="0"   max="255"   data-idx="${idx}">
        <input type="number" class="inp-g" value="${c.g}" min="0"   max="255"   data-idx="${idx}">
        <input type="number" class="inp-b" value="${c.b}" min="0"   max="255"   data-idx="${idx}">
        <input type="number" class="inp-f" value="${c.freq}" min="1" max="40000" data-idx="${idx}">
      </div>
      <button class="btn-del" data-idx="${idx}" title="Törlés">−</button>
    `;
    list.appendChild(row);
  });

  document.getElementById('btnAdd').disabled = (colors.length >= 25);
  bindRowEvents();
  applyMobileLayout();
}

function applyMobileLayout() {
  if (window.innerWidth <= 500) {
    document.querySelectorAll('.pwm-group').forEach(el => el.style.display = 'flex');
  }
}
window.addEventListener('resize', applyMobileLayout);

// ────────────────────────────────────────────
//  Sor eseménykezelők
// ────────────────────────────────────────────
function bindRowEvents() {

  // Sor kattintás → aktív szín váltás (ha nem input/gomb)
  document.querySelectorAll('.color-row').forEach((row, idx) => {
    row.addEventListener('click', (e) => {
      if (e.target.tagName === 'INPUT' || e.target.tagName === 'BUTTON') return;
      if (idx === activeIndex) return;
      activeIndex = idx;
      updateHighlight(idx);
      const c = colors[idx];
      wsSend(`SET:${idx}:${c.r}:${c.g}:${c.b}:${c.freq}`);
    });
  });

  // Törlés
  document.querySelectorAll('.btn-del').forEach(btn => {
    btn.addEventListener('click', () => {
      const idx = parseInt(btn.dataset.idx);
      colors.splice(idx, 1);
      if (activeIndex >= colors.length) activeIndex = colors.length - 1;
      wsSend('DEL:' + idx);
      renderList();
    });
  });

  // Név (debounced)
  document.querySelectorAll('.inp-name').forEach(inp => {
    inp.addEventListener('input', debounce(() => {
      const idx = parseInt(inp.dataset.idx);
      if (idx < colors.length) {
        colors[idx].name = inp.value;
        wsSend(`NAME:${idx}:${inp.value}`);
      }
    }, 400));
  });

  // PWM értékek – azonnal küld és frissíti a pöttyöt
  const pwmHandler = (inp, field) => {
    inp.addEventListener('input', () => {
      const idx = parseInt(inp.dataset.idx);
      if (idx >= colors.length) return;
      let val = parseInt(inp.value) || 0;
      if (field === 'freq') {
        val = Math.max(1, Math.min(40000, val));
        colors[idx].freq = val;
      } else {
        val = Math.max(0, Math.min(255, val));
        colors[idx][field] = val;
      }
      // Szín pötty frissítése
      const dot = document.getElementById('dot' + idx);
      if (dot) {
        const c = colors[idx];
        const rgb = `rgb(${c.r},${c.g},${c.b})`;
        dot.style.background  = rgb;
        dot.style.boxShadow   = `0 0 6px ${rgb}88`;
      }
      activeIndex = idx;
      updateHighlight(idx);
      const c = colors[idx];
      wsSend(`SET:${idx}:${c.r}:${c.g}:${c.b}:${c.freq}`);
    });
  };

  document.querySelectorAll('.inp-r').forEach(i => pwmHandler(i, 'r'));
  document.querySelectorAll('.inp-g').forEach(i => pwmHandler(i, 'g'));
  document.querySelectorAll('.inp-b').forEach(i => pwmHandler(i, 'b'));
  document.querySelectorAll('.inp-f').forEach(i => pwmHandler(i, 'freq'));
}

function updateHighlight(idx) {
  document.querySelectorAll('.color-row').forEach((r, i) =>
    r.classList.toggle('active-row', i === idx));
  updateActiveDot();
}

// ────────────────────────────────────────────
//  + gomb: szín hozzáadása
// ────────────────────────────────────────────
document.getElementById('btnAdd').addEventListener('click', () => {
  if (colors.length >= 25) return;
  colors.push({ name: 'Új szín', r: 128, g: 128, b: 128, freq: 1000 });
  wsSend('ADD');
  renderList();
});

// ────────────────────────────────────────────
//  Mentés gomb
// ────────────────────────────────────────────
document.getElementById('btnSave').addEventListener('click', () => {
  // Alvási idő beolvasása és elküldése
  const m = parseInt(document.getElementById('sleepInput').value) || 10;
  sleepM  = Math.max(5, Math.min(60, m));
  document.getElementById('sleepInput').value = sleepM;
  wsSend('SLEEP:' + sleepM);

  // Mentés
  wsSend('SAVE');
  showToast('Beállítások mentve ✓');
});

// ────────────────────────────────────────────
//  OTA gomb
// ────────────────────────────────────────────
document.getElementById('btnOta').addEventListener('click', () => {
  window.location.href = '/update';
});

// ────────────────────────────────────────────
//  Segédfüggvények
// ────────────────────────────────────────────
function escHtml(s) {
  return s.replace(/&/g,'&amp;').replace(/"/g,'&quot;').replace(/</g,'&lt;').replace(/>/g,'&gt;');
}

// ────────────────────────────────────────────
//  Aktív szín pötty + státusz sor frissítése
// ────────────────────────────────────────────
function updateActiveDot() {
  if (activeIndex < 0 || activeIndex >= colors.length) return;
  const c   = colors[activeIndex];
  const rgb = `rgb(${c.r},${c.g},${c.b})`;

  const dot = document.getElementById('activeDot');
  if (dot) {
    dot.style.background = rgb;
    dot.style.boxShadow  = `0 0 12px ${rgb}`;
  }

  const barDot = document.getElementById('activeBarDot');
  if (barDot) {
    barDot.style.background = rgb;
    barDot.style.boxShadow  = `0 0 6px ${rgb}88`;
  }
  const nameEl = document.getElementById('activeBarName');
  const rEl    = document.getElementById('activeBarR');
  const gEl    = document.getElementById('activeBarG');
  const bEl    = document.getElementById('activeBarB');
  const fEl    = document.getElementById('activeBarF');
  if (nameEl) nameEl.textContent = c.name || '—';
  if (rEl)    rEl.textContent    = c.r;
  if (gEl)    gEl.textContent    = c.g;
  if (bEl)    bEl.textContent    = c.b;
  if (fEl)    fEl.textContent    = c.freq + ' Hz';
}

function debounce(fn, d) {
  let t;
  return (...a) => { clearTimeout(t); t = setTimeout(() => fn(...a), d); };
}

function showToast(msg) {
  const el = document.getElementById('toast');
  el.textContent = msg;
  el.classList.add('show');
  setTimeout(() => el.classList.remove('show'), 2400);
}

// ────────────────────────────────────────────
//  Indítás
// ────────────────────────────────────────────
loadStatus().then(() => connectWS());
</script>
</body>
</html>
)rawhtml";
