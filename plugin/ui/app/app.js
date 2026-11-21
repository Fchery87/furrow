'use strict';

const $ = id => document.getElementById(id);
const PALETTE = ['#D99B38', '#5A98D4', '#D96B4F', '#6FB065', '#C66A92', '#857FD1'];
const NOTE_NAMES = ['C', 'C#', 'D', 'D#', 'E', 'F', 'F#', 'G', 'G#', 'A', 'A#', 'B'];
const PROB_CYCLE = [100, 75, 50, 25];
const LANE_FIELDS = { note: 4, mute: 5, solo: 6, volume: 7, pan: 8, bendRange: 9, swing: 10, modACc: 13, modBCc: 14 };
const RACK_FIELDS = { seed: 1, currentPattern: 2, globalSwing: 4, truncate: 5 };
const STEP_FIELDS = [
  { key: 'vel',     idx: 1,  label: 'Velocity',    min: 1,    max: 127,    def: 100,  midiOnly: false, help: 'Hit strength from 1 to 127. Double-click a bar to reset.', scale: ['1', '64', '127'], fmt: v => String(v) },
  { key: 'prob',    idx: 12, label: 'Probability', min: 0,    max: 100,    def: 100,  midiOnly: false, help: 'Chance each step fires under the current probability seed.', scale: ['0%', '50%', '100%'], fmt: v => v + '%' },
  { key: 'pitch',   idx: 3,  label: 'Pitch',       min: -24,  max: 24,     def: 0,    bipolar: true, midiOnly: false, help: 'Semitone transpose per step (-24 to +24 st).', scale: ['-24', '0', '+24 st'], fmt: v => (v > 0 ? '+' : '') + v + ' st' },
  { key: 'fine',    idx: 4,  label: 'Fine',        min: -100, max: 100,    def: 0,    bipolar: true, midiOnly: false, help: 'Fine pitch offset in cents (-100 to +100 c).', scale: ['-100', '0', '+100 c'], fmt: v => (v > 0 ? '+' : '') + v + ' c' },
  { key: 'pan',     idx: 5,  label: 'Pan',         min: -64,  max: 63,     def: 0,    bipolar: true, midiOnly: false, help: 'Per-step stereo balance (64 L to 63 R).', scale: ['64 L', 'C', '63 R'], fmt: v => v === 0 ? 'C' : (v < 0 ? Math.abs(v) + ' L' : v + ' R') },
  { key: 'shift',   idx: 8,  label: 'Shift',       min: 0,    max: 9900,   def: 0,    midiOnly: false, help: 'Micro-timing delay inside the step (0% to 99%).', scale: ['0%', '50%', '99%'], fmt: v => (v / 100).toFixed(0) + '%' },
  { key: 'gate',    idx: 9,  label: 'Gate',        min: 500,  max: 160000, def: 5000, midiOnly: false, help: 'Note hold duration in steps.', scale: ['0.05', '8.0', '16.0 st'], fmt: v => (v / 10000).toFixed(2) + ' st' },
  { key: 'ratchet', idx: 10, label: 'Ratchet',     min: 1,    max: 8,      def: 1,    midiOnly: false, help: 'Subdivides a single step into 1 to 8 rapid hits.', scale: ['x1', 'x4', 'x8'], fmt: v => 'x' + v },
  { key: 'rel',     idx: 2,  label: 'Release',     min: 0,    max: 127,    def: 64,   midiOnly: true,  help: 'MIDI note-off release velocity (MIDI lanes only).', scale: ['0', '64', '127'], fmt: v => String(v) },
  { key: 'modA',    idx: 6,  label: 'Mod A',       min: 0,    max: 127,    def: 64,   midiOnly: true,  help: 'MIDI CC Mod A value per step (MIDI lanes only).', scale: ['0', '64', '127'], fmt: v => String(v) },
  { key: 'modB',    idx: 7,  label: 'Mod B',       min: 0,    max: 127,    def: 64,   midiOnly: true,  help: 'MIDI CC Mod B value per step (MIDI lanes only).', scale: ['0', '64', '127'], fmt: v => String(v) },
];

const CELL_GAP = 3, GROUP_GAP = 7, ROW_H = 46, CELL_H = 36;
const THEME = {
  bg: '#111215',
  trough: '#0D0E12',
  cell: '#1D2027',
  cellAlt: '#181A20',
  border: '#252932',
  selectedRow: 'rgba(236,238,242,0.035)',
  hover: 'rgba(236,238,242,0.24)',
  playTint: 'rgba(236,238,242,0.12)',
  playBar: '#ECEEF2',
  text: '#ECEEF2',
  textMuted: '#7F8797',
  textFaint: '#505766',
  well: '#0C0D10',
  green: '#69C27B',
  danger: '#E0605B',
};

let state = {
  standalone: false, patternMidiChannel: 0, nonEmptyPatterns: [0],
  browserFolders: [], browserListing: null, lanes: [], patterns: ['Pattern 1'],
  pattern: 0, pendingPattern: -1, playing: false, bpm: 120, globalSwing: 0,
  truncate: false, seed: 0, canUndo: false, canRedo: false, error: '', midiPreview: null,
};
let transport = { playing: false, bpm: 120, beat: 0, pattern: 0, pendingPattern: -1, playheads: {}, meters: {} };

let selectedSlot = 0;
let activeTab = 0;
let activeBank = 0;
let dragLaneSlot = -1;
let browserOpen = false;
let inspectorOpen = true;
let editorOpen = true;
let zoomMode = '30';
let browserAutoSwap = false;
let browserFilter = '';
let browserCursor = -1;
let audioDragTail = false;
let lastSwingRendered = null;
let focusCell = { slot: 0, step: 0 };
let hoverGrid = null;
let hoverBar = null;
let openMenu = null;
let openMenuTrigger = null;
let renamingPattern = false;
let activeLiveDrag = 0;
let gridDrag = null;
let barDrag = null;
let prevPlayheads = {};
let hitFlashes = {};
let meterDisplay = {};
let meterTargets = {};
let rafScheduled = false;

function emit(action, data = {}) {
  const msg = { action, ...data };
  if (window.__JUCE__?.backend?.emitEvent) window.__JUCE__.backend.emitEvent('edit', msg);
}
function esc(v) { return String(v ?? '').replace(/[&<>"']/g, c => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;' }[c])); }
const clamp = (v, lo, hi) => Math.min(hi, Math.max(lo, v));
const noteName = n => NOTE_NAMES[((n % 12) + 12) % 12] + (Math.floor(n / 12) - 1);
const currentLane = () => (state.lanes || []).find(l => l.slot === selectedSlot);
const editingInspector = () => activeLiveDrag > 0 || (['INPUT', 'SELECT'].includes(document.activeElement?.tagName) && $('inspector')?.contains?.(document.activeElement));

function maxCols() {
  const longest = Math.max(16, ...(state.lanes || []).map(l => Number(l.length) || 16));
  return Math.min(512, Math.ceil(longest / 4) * 4);
}
function getCellW(cols) {
  if (zoomMode === 'fit') {
    const vpW = ($('gridScroll')?.clientWidth || 620) - 24;
    const groups = Math.max(0, Math.floor((cols - 1) / 4));
    return clamp(Math.floor((vpW - groups * GROUP_GAP - (cols - 1) * CELL_GAP - 12) / cols), 18, 44);
  }
  return Number(zoomMode) || 30;
}
function stepX(i, cellW) { return 6 + i * (cellW + CELL_GAP) + Math.floor(i / 4) * GROUP_GAP; }
function totalGridWidth(cols, cellW) { return stepX(cols, cellW) + 8; }
function xToStep(x, cols, cellW) {
  for (let i = 0; i < cols; i++) {
    const sx = stepX(i, cellW);
    if (x >= sx - 2 && x <= sx + cellW + 2) return i;
  }
  return -1;
}
function roundRectPath(g, x, y, w, h, r) {
  g.beginPath();
  if (typeof g.roundRect === 'function') g.roundRect(x, y, w, h, r);
  else if (typeof g.rect === 'function') g.rect(x, y, w, h);
  else g.fillRect(x, y, w, h);
}
function setHint(text) { if ($('hintText')) $('hintText').textContent = text; }

/* ---------- Rotary SVG Knob (Linear & Logarithmic + Live Drag Gesture) ---------- */
function makeKnob(o) {
  const size = o.small ? 22 : 38, c = size / 2, r = c - (o.small ? 2.5 : 3.5);
  const ns = 'http://www.w3.org/2000/svg';
  const wrap = document.createElement('div');
  wrap.className = o.small ? 'knob small' : 'knob';
  const dial = document.createElement('div');
  dial.className = 'dial'; dial.tabIndex = 0; dial.setAttribute('role', 'slider');
  dial.setAttribute('aria-label', o.label);
  const svg = document.createElementNS(ns, 'svg');
  svg.setAttribute('viewBox', `0 0 ${size} ${size}`);
  const pt = (deg, rad) => { const a = deg * Math.PI / 180; return [c + rad * Math.sin(a), c - rad * Math.cos(a)]; };
  const arc = (a0, a1) => {
    if (a1 < a0) [a0, a1] = [a1, a0];
    if (a1 - a0 < 0.6) return '';
    const [x0, y0] = pt(a0, r), [x1, y1] = pt(a1, r);
    return `M${x0} ${y0} A${r} ${r} 0 ${a1 - a0 > 180 ? 1 : 0} 1 ${x1} ${y1}`;
  };
  const mkPath = (stroke, w) => {
    const p = document.createElementNS(ns, 'path');
    p.setAttribute('fill', 'none'); p.setAttribute('stroke', stroke);
    p.setAttribute('stroke-width', w); p.setAttribute('stroke-linecap', 'round');
    svg.appendChild(p); return p;
  };
  const trackW = o.small ? 2.4 : 3;
  mkPath('#292D38', trackW).setAttribute('d', arc(-135, 135));
  const valArc = mkPath(o.color || '#ECEEF2', trackW);
  const cap = document.createElementNS(ns, 'circle');
  cap.setAttribute('cx', c); cap.setAttribute('cy', c); cap.setAttribute('r', Math.max(3, r - (o.small ? 3.5 : 5)));
  cap.setAttribute('fill', '#191C22'); cap.setAttribute('stroke', '#2E3340'); cap.setAttribute('stroke-width', '1');
  svg.appendChild(cap);
  const needle = mkPath('#ECEEF2', o.small ? 1.5 : 1.8);
  dial.appendChild(svg);
  const out = document.createElement('output');
  let val = o.value;

  const valToNorm = v => {
    if (o.log) return Math.log(clamp(v, o.min, o.max) / o.min) / Math.log(o.max / o.min);
    return clamp((v - o.min) / (o.max - o.min || 1), 0, 1);
  };
  const normToVal = f => {
    const cf = clamp(f, 0, 1);
    let raw = o.log ? o.min * Math.pow(o.max / o.min, cf) : o.min + cf * (o.max - o.min);
    if (o.step) raw = Math.round(raw / o.step) * o.step;
    return +clamp(raw, o.min, o.max).toFixed(4);
  };

  const draw = () => {
    const f = valToNorm(val);
    const ang = -135 + 270 * f;
    valArc.setAttribute('d', arc(o.bipolar ? 0 : -135, ang));
    const [x0, y0] = pt(ang, o.small ? 1.5 : Math.max(2, r - 10));
    const [x1, y1] = pt(ang, r - (o.small ? 3.5 : 5.5));
    needle.setAttribute('d', `M${x0} ${y0} L${x1} ${y1}`);
    out.textContent = o.fmt(val);
    dial.setAttribute('aria-valuenow', String(val));
  };
  const commit = v => {
    const next = o.step ? +clamp(Math.round(v / o.step) * o.step, o.min, o.max).toFixed(4) : +clamp(v, o.min, o.max).toFixed(4);
    if (next === val) return;
    val = next; draw(); o.onChange(val, undefined);
  };

  dial.addEventListener('pointerdown', e => {
    e.preventDefault(); dial.focus?.(); dial.setPointerCapture?.(e.pointerId);
    activeLiveDrag++;
    let lastY = e.clientY;
    let norm = valToNorm(val);
    let started = false;
    let lastEmit = 0;
    const mv = ev => {
      const sens = ev.shiftKey ? 500 : 140;
      norm = clamp(norm + (lastY - ev.clientY) / sens, 0, 1);
      lastY = ev.clientY;
      const next = normToVal(norm);
      if (next === val) return;
      val = next;
      draw();
      const now = Date.now();
      if (!started) {
        started = true; lastEmit = now;
        o.onChange(val, 'start');
      } else if (now - lastEmit >= 40) {
        lastEmit = now;
        o.onChange(val, 'move');
      }
    };
    const up = () => {
      dial.removeEventListener('pointermove', mv);
      dial.removeEventListener('pointerup', up);
      dial.removeEventListener('pointercancel', up);
      activeLiveDrag = Math.max(0, activeLiveDrag - 1);
      if (started) o.onChange(val, 'end');
    };
    dial.addEventListener('pointermove', mv);
    dial.addEventListener('pointerup', up);
    dial.addEventListener('pointercancel', up);
  });
  dial.addEventListener('dblclick', () => commit(o.def));
  dial.addEventListener('keydown', e => {
    const dir = { ArrowUp: 1, ArrowRight: 1, ArrowDown: -1, ArrowLeft: -1 }[e.key];
    if (!dir) return;
    e.preventDefault();
    if (o.log) commit(normToVal(valToNorm(val) + dir * (e.shiftKey ? 0.01 : 0.04)));
    else commit(val + dir * (o.step || 1));
  });
  draw();
  const lbl = document.createElement('span'); lbl.className = 'k-label'; lbl.textContent = o.label;
  if (o.small) {
    const t = document.createElement('div'); t.className = 'k-text'; t.append(lbl, out); wrap.append(dial, t);
  } else wrap.append(dial, lbl, out);
  return wrap;
}

/* ---------- Popovers & Menus ---------- */
function closeMenu() {
  if (openMenu) { openMenu.remove(); openMenu = null; openMenuTrigger = null; }
}
function showMenu(x, y, items, opts = {}) {
  closeMenu();
  const el = document.createElement('div');
  el.className = 'menu' + (opts.alignRight ? ' align-right' : '');
  el.setAttribute('role', 'menu');
  for (const item of items) {
    if (item === '-') { el.appendChild(document.createElement('hr')); continue; }
    const b = document.createElement('button');
    b.type = 'button'; b.textContent = item.label;
    if (item.checked !== undefined) b.setAttribute('aria-checked', String(!!item.checked));
    if (item.danger) b.className = 'danger';
    b.addEventListener('click', () => { closeMenu(); item.action(); });
    el.appendChild(b);
  }
  document.body.appendChild(el);
  const r = el.getBoundingClientRect?.() || { width: 192, height: 220 };
  const left = opts.alignRight ? Math.max(8, x - r.width) : Math.min(x, (window.innerWidth || 1000) - r.width - 8);
  el.style.left = left + 'px';
  el.style.top = Math.min(y, (window.innerHeight || 700) - r.height - 8) + 'px';
  openMenu = el;
  openMenuTrigger = opts.trigger || null;
}

/* ---------- Top Bar ---------- */
function renderTop() {
  const names = state.patterns || ['Pattern 1'];
  if ($('patternIdx')) $('patternIdx').textContent = String((state.pattern || 0) + 1).padStart(2, '0');
  if (!renamingPattern && $('patternName')) $('patternName').textContent = names[state.pattern] || `Pattern ${state.pattern + 1}`;
  const pending = transport.playing ? transport.pendingPattern : state.pendingPattern;
  if ($('queuedBadge')) {
    const show = pending >= 0 && pending !== state.pattern;
    $('queuedBadge').hidden = !show;
    if (show) $('queuedBadge').textContent = `Next bar: ${names[pending] || 'Pattern ' + (pending + 1)}`;
  }
  if ($('playPill')) $('playPill').className = 'transport-cluster' + (transport.playing ? ' playing' : '');
  const bpm = Math.round(transport.bpm || state.bpm || 120);
  if ($('standalonePlayBtn')) {
    $('standalonePlayBtn').hidden = !state.standalone;
    $('standalonePlayBtn').textContent = transport.playing ? 'Stop' : 'Play';
    $('standalonePlayBtn').className = 'play-btn' + (transport.playing ? ' active' : '');
    $('standalonePlayBtn').setAttribute('aria-pressed', String(!!transport.playing));
  }
  if ($('fullscreenBtn')) $('fullscreenBtn').hidden = !state.standalone;
  if ($('playDot')) $('playDot').hidden = !!state.standalone;
  if ($('bpmText')) {
    $('bpmText').hidden = !!state.standalone;
    $('bpmText').textContent = `${bpm} BPM`;
  }
  if ($('bpmWrap')) $('bpmWrap').hidden = !state.standalone;
  if ($('bpmInput')) {
    $('bpmInput').hidden = !state.standalone;
    if (document.activeElement !== $('bpmInput')) $('bpmInput').value = String(bpm);
  }
  if ($('undoBtn')) $('undoBtn').disabled = !state.canUndo;
  if ($('redoBtn')) $('redoBtn').disabled = !state.canRedo;
  if ($('error')) $('error').textContent = state.error || '';
  if ($('wavTailBtn')) {
    $('wavTailBtn').setAttribute('aria-pressed', String(audioDragTail));
    $('wavTailBtn').className = 'tail-toggle' + (audioDragTail ? ' active' : '');
  }
  if ($('inspectorToggleBtn')) {
    $('inspectorToggleBtn').setAttribute('aria-pressed', String(inspectorOpen));
    $('inspectorToggleBtn').className = 'btn ghost icon-only' + (inspectorOpen ? ' active' : '');
  }
  if ($('centerCol')) $('centerCol').className = 'center-col' + (editorOpen ? '' : ' editor-collapsed');
  if ($('editorToggleBtn')) $('editorToggleBtn').setAttribute('aria-expanded', String(editorOpen));

  const slot = $('globalSwingSlot');
  const swingVal = state.globalSwing || 0;
  if (slot && typeof document.createElementNS === 'function' && swingVal !== lastSwingRendered && activeLiveDrag === 0) {
    lastSwingRendered = swingVal;
    slot.innerHTML = '';
    slot.appendChild(makeKnob({
      label: 'Swing', small: true, min: 0, max: 10000, step: 100, def: 0, value: swingVal, color: '#ECEEF2',
      fmt: v => Math.round(v / 100) + '%',
      onChange: (v, gesture) => {
        const rounded = Math.round(v);
        state.globalSwing = rounded;
        lastSwingRendered = rounded;
        emit('setRack', gesture ? { field: RACK_FIELDS.globalSwing, value: rounded, gesture } : { field: RACK_FIELDS.globalSwing, value: rounded });
      },
    }));
  }
}

function renderMidiPreviewBanner() {
  const bar = $('midiPreviewBar');
  if (!bar) return;
  const preview = state.midiPreview;
  if (!preview) { bar.hidden = true; bar.innerHTML = ''; return; }
  bar.hidden = false;
  const diags = (preview.diagnostics || []).map(d => `<div class="midi-banner-diag">${esc(d.message)}</div>`).join('');
  bar.innerHTML =
    `<div class="midi-banner-info"><span class="midi-banner-title">MIDI import preview: ${preview.lanes} lanes, ${preview.lengthSteps} steps</span>${diags}</div>` +
    `<div class="midi-banner-actions">` +
    `<button class="btn" data-action="midiPreview">Choose MIDI</button>` +
    `<button class="btn active" data-action="midiImport" ${preview.accepted ? '' : 'disabled'}>Import into pattern</button>` +
    `</div>`;
}

function startInlinePatternRename() {
  renamingPattern = true;
  const wrap = $('patternWrap');
  const current = (state.patterns || [])[state.pattern] || `Pattern ${state.pattern + 1}`;
  const input = document.createElement('input');
  input.className = 'pattern-rename'; input.value = current; input.maxLength = 128;
  input.setAttribute('aria-label', 'Pattern name');
  let finished = false;
  const finish = commit => {
    if (finished) return;
    finished = true;
    renamingPattern = false;
    const val = input.value.trim();
    input.remove();
    if ($('patternBtn')) $('patternBtn').hidden = false;
    if (commit && val && val !== current) emit('renamePattern', { name: val });
    renderTop();
  };
  input.addEventListener('keydown', e => { if (e.key === 'Enter') finish(true); if (e.key === 'Escape') finish(false); });
  input.addEventListener('blur', () => finish(true));
  if ($('patternBtn')) $('patternBtn').hidden = true;
  if (wrap && typeof wrap.insertBefore === 'function' && $('patNextBtn')) wrap.insertBefore(input, $('patNextBtn'));
  else wrap?.prepend?.(input);
  input.focus?.(); input.select?.();
}

/* ---------- Lane Headers, Ruler & Canvas Grid ---------- */
function renderHeaders() {
  const lanes = state.lanes || [];
  const anySolo = lanes.some(l => l.solo && !l.mute);
  $('laneHeaders').innerHTML = lanes.map(l => {
    const cls = 'lane-row' + (l.slot === selectedSlot ? ' selected' : '') + (l.mute ? ' muted' : '') + (!l.mute && anySolo && !l.solo ? ' silenced' : '');
    const badges = [
      l.target === 'midi' ? `<span class="badge">M${l.channel}</span>` : '',
      l.length !== 16 ? `<span class="badge">${l.length}</span>` : '',
      l.sampleMissing ? `<span class="badge warn" title="${esc(l.sampleError)}">!</span>` : '',
    ].join('');
    return `<div class="${cls}" style="--lane:${esc(l.color || '#D99B38')}" data-lane="${l.slot}" draggable="true">` +
      `<span class="drag-handle" title="Drag to reorder">⋮⋮</span>` +
      `<span class="chip"></span>` +
      `<span class="meter" aria-hidden="true"><span class="meter-fill" id="meter-${l.slot}"></span></span>` +
      `<button class="lane-name" data-lane="${l.slot}" aria-label="Select ${esc(l.name)}">${esc(l.name)}</button>` +
      badges +
      `<button class="ms m" data-action="mute" data-lane="${l.slot}" aria-pressed="${!!l.mute}" aria-label="Mute ${esc(l.name)}">M</button>` +
      `<button class="ms s" data-action="solo" data-lane="${l.slot}" aria-pressed="${!!l.solo}" aria-label="Solo ${esc(l.name)}">S</button>` +
      `<button class="lane-more" data-more="${l.slot}" aria-label="Lane tools for ${esc(l.name)}" title="Lane tools">⋯</button>` +
      `</div>`;
  }).join('') + `<div class="drop-zone"><button class="add-lane-btn" data-action="addLane">+ Add lane</button></div>`;
}

function drawRuler() {
  const canvas = $('rulerCanvas');
  if (!canvas || !canvas.getContext) return;
  const cols = maxCols();
  const cellW = getCellW(cols);
  const cssW = Math.max(($('gridScroll')?.clientWidth || 520) - 16, totalGridWidth(cols, cellW));
  const cssH = 30;
  const dpr = window.devicePixelRatio || 1;
  if (canvas.width !== Math.round(cssW * dpr) || canvas.height !== Math.round(cssH * dpr)) {
    canvas.width = Math.round(cssW * dpr); canvas.height = Math.round(cssH * dpr);
    canvas.style.width = cssW + 'px'; canvas.style.height = cssH + 'px';
  }
  const g = canvas.getContext('2d');
  g.setTransform(dpr, 0, 0, dpr, 0, 0);
  g.clearRect?.(0, 0, cssW, cssH);
  g.save?.();
  g.translate?.(-($('gridScroll')?.scrollLeft || 0) + 8, 0);
  g.font = '600 10px "JetBrains Mono", Consolas, monospace';
  g.textBaseline = 'middle';
  for (let i = 0; i < cols; i++) {
    const x = stepX(i, cellW);
    if (i % 4 === 0) {
      const bar = Math.floor(i / 16) + 1;
      const beat = Math.floor((i % 16) / 4) + 1;
      g.fillStyle = beat === 1 ? THEME.textMuted : THEME.textFaint;
      g.fillText?.(beat === 1 ? `BAR ${bar}` : `${bar}.${beat}`, x + 2, cssH / 2 + 1);
      g.fillStyle = THEME.border;
      g.fillRect(x, cssH - 5, 1, 5);
    } else {
      g.fillStyle = THEME.textFaint;
      g.globalAlpha = 0.45;
      g.fillRect(x + cellW / 2, cssH - 3, 1, 3);
      g.globalAlpha = 1;
    }
  }
  g.restore?.();
}

function drawGrid() {
  const canvas = $('gridCanvas');
  if (!canvas || !canvas.getContext) return;
  const lanes = state.lanes || [];
  const cols = maxCols();
  const cellW = getCellW(cols);
  const cssW = Math.max(($('gridScroll')?.clientWidth || 520) - 20, totalGridWidth(cols, cellW));
  const cssH = Math.max(ROW_H, lanes.length * ROW_H + 8);
  const dpr = window.devicePixelRatio || 1;
  if (canvas.width !== Math.round(cssW * dpr) || canvas.height !== Math.round(cssH * dpr)) {
    canvas.width = Math.round(cssW * dpr); canvas.height = Math.round(cssH * dpr);
    canvas.style.width = cssW + 'px'; canvas.style.height = cssH + 'px';
  }
  const g = canvas.getContext('2d');
  g.setTransform(dpr, 0, 0, dpr, 0, 0);
  g.fillStyle = THEME.bg; g.fillRect(0, 0, cssW, cssH);
  const anySolo = lanes.some(l => l.solo && !l.mute);
  const now = Date.now();

  for (let r = 0; r < lanes.length; r++) {
    const l = lanes[r], y = r * ROW_H + 4;
    const len = Number(l.length) || 16;
    const dimmed = l.mute || (anySolo && !l.solo);
    const isSel = l.slot === selectedSlot;

    if (isSel) {
      g.fillStyle = THEME.selectedRow;
      roundRectPath(g, 0, y - 4, cssW, CELL_H + 8, 8);
      g.fill?.();
    }

    const laneW = totalGridWidth(len, cellW) - 2;
    g.fillStyle = THEME.trough;
    roundRectPath(g, 2, y - 3, laneW, CELL_H + 6, 8);
    g.fill?.();
    if (isSel) {
      g.strokeStyle = THEME.border; g.lineWidth = 1;
      g.stroke?.();
    }

    const playStep = transport.playheads[l.slot];

    for (let i = 0; i < len; i++) {
      const x = stepX(i, cellW), st = (l.steps && l.steps[i]) || [];
      const on = !!st[0], vel = st[1] ?? 100, rat = st[10] ?? 1, prob = st[12] ?? 100;

      g.globalAlpha = dimmed ? 0.36 : 1.0;
      g.fillStyle = (Math.floor(i / 4) % 2) ? THEME.cellAlt : THEME.cell;
      roundRectPath(g, x, y, cellW, CELL_H, 5);
      g.fill?.();

      if (on) {
        const fh = Math.max(5, Math.round((vel / 127) * CELL_H));
        const fy = y + CELL_H - fh;

        g.save?.();
        roundRectPath(g, x, y, cellW, CELL_H, 5);
        g.clip?.();

        g.fillStyle = l.color || '#D99B38';
        g.globalAlpha = (dimmed ? 0.36 : 1.0) * (0.35 + 0.65 * prob / 100);
        g.fillRect(x, fy, cellW, fh);

        g.fillStyle = 'rgba(255,255,255,0.22)';
        g.fillRect(x, fy, cellW, 1.5);

        const fkey = `${l.slot}:${i}`;
        const fts = hitFlashes[fkey];
        if (fts) {
          const age = now - fts;
          if (age < 170) {
            g.fillStyle = '#FFFFFF';
            g.globalAlpha = (1 - age / 170) * 0.35;
            g.fillRect(x, fy, cellW, fh);
          } else {
            delete hitFlashes[fkey];
          }
        }

        if (prob < 100) {
          g.globalAlpha = 1;
          g.strokeStyle = 'rgba(12,13,16,0.55)'; g.lineWidth = 1.5;
          g.beginPath();
          for (let d = -CELL_H; d < cellW + CELL_H; d += 6) {
            g.moveTo(x + d, y + CELL_H); g.lineTo(x + d + fh, fy);
          }
          g.stroke();
        }
        if (rat > 1) {
          g.fillStyle = THEME.trough; g.globalAlpha = 0.88;
          for (let k = 1; k < rat; k++) g.fillRect(x + Math.round(k * cellW / rat) - 1, fy, 2, fh);
        }
        g.restore?.();
      }

      g.globalAlpha = 1;
      if (playStep === i) {
        g.save?.();
        roundRectPath(g, x, y, cellW, CELL_H, 5);
        g.clip?.();
        g.fillStyle = THEME.playTint; g.fillRect(x, y, cellW, CELL_H);
        g.fillStyle = THEME.playBar; g.fillRect(x, y, cellW, 2.5);
        g.restore?.();
      }
      if (hoverGrid && hoverGrid.slot === l.slot && hoverGrid.step === i) {
        g.strokeStyle = THEME.hover; g.lineWidth = 1.2;
        roundRectPath(g, x + 0.5, y + 0.5, cellW - 1, CELL_H - 1, 5);
        g.stroke?.();
      }
      if (focusCell.slot === l.slot && focusCell.step === i && document.activeElement === canvas) {
        g.strokeStyle = THEME.playBar; g.lineWidth = 1.5;
        roundRectPath(g, x + 1, y + 1, cellW - 2, CELL_H - 2, 4.5);
        g.stroke?.();
      }
    }
  }
}

/* ---------- Step Lane Editor ---------- */
function renderStepTabs() {
  const lane = currentLane();
  const f = STEP_FIELDS[activeTab];
  if ($('editorLaneDot')) $('editorLaneDot').style.background = lane?.color || '#D99B38';
  if ($('editorLaneName')) $('editorLaneName').textContent = lane?.name || '';
  if ($('axisTitle')) $('axisTitle').textContent = f.label;
  if ($('axisHelp')) $('axisHelp').textContent = f.help;
  if ($('axisScale')) $('axisScale').innerHTML = f.scale.map(s => `<span>${esc(s)}</span>`).join('');

  const primary = STEP_FIELDS.map((field, idx) => ({ field, idx })).filter(x => !x.field.midiOnly);
  const midiOnly = STEP_FIELDS.map((field, idx) => ({ field, idx })).filter(x => x.field.midiOnly);
  const mkTab = ({ field, idx }) => {
    const dim = field.midiOnly && lane?.target !== 'midi';
    return `<button class="tab ${dim ? 'midi-only' : ''}" role="tab" data-tab="${idx}" aria-selected="${idx === activeTab}" title="${esc(dim ? field.label + ' (MIDI lanes only)' : field.label)}">${esc(field.label)}</button>`;
  };
  $('stepTabs').innerHTML = primary.map(mkTab).join('') + `<span class="step-tab-sep"></span>` + midiOnly.map(mkTab).join('');
}

function drawBars() {
  const canvas = $('barsCanvas');
  if (!canvas || !canvas.getContext) return;
  const lane = currentLane();
  const cols = maxCols();
  const cellW = getCellW(cols);
  const cssW = Math.max(($('barsScroll')?.clientWidth || 520) - 20, totalGridWidth(cols, cellW));
  const cssH = 118;
  const dpr = window.devicePixelRatio || 1;
  if (canvas.width !== Math.round(cssW * dpr) || canvas.height !== Math.round(cssH * dpr)) {
    canvas.width = Math.round(cssW * dpr); canvas.height = Math.round(cssH * dpr);
    canvas.style.width = cssW + 'px'; canvas.style.height = cssH + 'px';
  }
  const g = canvas.getContext('2d');
  g.setTransform(dpr, 0, 0, dpr, 0, 0);
  g.fillStyle = THEME.bg; g.fillRect(0, 0, cssW, cssH);
  if (!lane) return;
  const f = STEP_FIELDS[activeTab], len = Number(lane.length) || 16;

  g.fillStyle = THEME.trough;
  roundRectPath(g, 2, 2, totalGridWidth(len, cellW) - 2, cssH - 4, 8);
  g.fill?.();

  g.strokeStyle = THEME.border; g.lineWidth = 1; g.globalAlpha = 0.55;
  for (const frac of (f.bipolar ? [0.5] : [0.25, 0.5, 0.75])) {
    const gy = Math.round(6 + (cssH - 12) * frac) + 0.5;
    g.beginPath(); g.moveTo(6, gy); g.lineTo(totalGridWidth(len, cellW) - 6, gy); g.stroke();
  }
  g.globalAlpha = 1;

  const playStep = transport.playheads[lane.slot];
  for (let i = 0; i < len; i++) {
    const x = stepX(i, cellW), st = (lane.steps && lane.steps[i]) || [];
    const on = !!st[0], val = st[f.idx] ?? f.def;
    g.fillStyle = (Math.floor(i / 4) % 2) ? THEME.cellAlt : THEME.cell;
    roundRectPath(g, x, 6, cellW, cssH - 12, 4);
    g.fill?.();

    g.fillStyle = lane.color || '#D99B38';
    g.globalAlpha = on ? 0.92 : 0.24;
    if (f.bipolar) {
      const mid = cssH / 2, half = (cssH - 14) / 2;
      const frac = Math.abs(val) / Math.max(Math.abs(f.min), Math.abs(f.max));
      const bh = Math.max(2, frac * half);
      roundRectPath(g, x + 2, val >= 0 ? mid - bh : mid, cellW - 4, bh, 3);
      g.fill?.();
    } else {
      const frac = (val - f.min) / (f.max - f.min || 1);
      const bh = Math.max(3, frac * (cssH - 14));
      roundRectPath(g, x + 2, cssH - 7 - bh, cellW - 4, bh, 3);
      g.fill?.();
    }
    g.globalAlpha = 1;

    if (playStep === i) {
      g.fillStyle = THEME.playTint;
      roundRectPath(g, x, 6, cellW, cssH - 12, 4);
      g.fill?.();
    }
    if (hoverBar && hoverBar.step === i) {
      g.strokeStyle = THEME.hover; g.lineWidth = 1.2;
      roundRectPath(g, x + 0.5, 6.5, cellW - 1, cssH - 13, 4);
      g.stroke?.();
    }
  }
}

/* ---------- Interactive Sample Waveform ---------- */
function drawWaveform(canvas, lane) {
  if (!canvas || !canvas.getContext) return;
  const dpr = window.devicePixelRatio || 1;
  const w = Math.max(180, canvas.clientWidth || 242), h = 60;
  canvas.width = Math.round(w * dpr); canvas.height = Math.round(h * dpr);
  const g = canvas.getContext('2d');
  g.setTransform(dpr, 0, 0, dpr, 0, 0);
  g.fillStyle = THEME.well; g.fillRect(0, 0, w, h);
  g.fillStyle = THEME.border; g.fillRect(0, h / 2, w, 1);

  const peaks = lane.peaks || [];
  if (peaks.length) {
    g.fillStyle = lane.color || '#D99B38';
    const bw = w / peaks.length;
    for (let i = 0; i < peaks.length; i++) {
      const idx = lane.reverse ? (peaks.length - 1 - i) : i;
      const bh = Math.max(2, peaks[idx] * (h - 12));
      g.fillRect(i * bw, (h - bh) / 2, Math.max(1, bw - 0.6), bh);
    }
  }
  const sx = clamp(lane.start || 0, 0, 0.99) * w, ex = clamp(lane.end ?? 1, 0.01, 1) * w;
  g.fillStyle = 'rgba(0,0,0,0.48)';
  if (sx > 0) g.fillRect(0, 0, sx, h);
  if (ex < w) g.fillRect(ex, 0, w - ex, h);

  g.fillStyle = THEME.text;
  g.fillRect(sx, 0, 1.5, h);
  g.fillRect(ex - 1.5, 0, 1.5, h);
  g.beginPath(); g.moveTo(sx, 0); g.lineTo(sx + 8, 0); g.lineTo(sx, 8); g.closePath?.(); g.fill?.();
  g.beginPath(); g.moveTo(ex, 0); g.lineTo(ex - 8, 0); g.lineTo(ex, 8); g.closePath?.(); g.fill?.();
}

function bindWaveformDrag(canvas, lane) {
  if (!canvas || typeof canvas.addEventListener !== 'function') return;
  let dragWhich = null;
  let started = false;
  let lastEmit = 0;

  const updateTrim = (e, gesture) => {
    const r = canvas.getBoundingClientRect?.() || { left: 0, width: 242 };
    const frac = clamp((e.clientX - r.left) / (r.width || 242), 0, 1);
    const val = dragWhich === 'start'
      ? +clamp(frac, 0, (lane.end ?? 1) - 0.01).toFixed(3)
      : +clamp(frac, (lane.start || 0) + 0.01, 1).toFixed(3);
    if (dragWhich === 'start') lane.start = val; else lane.end = val;
    drawWaveform(canvas, lane);
    if ($('waveTrimBadge')) $('waveTrimBadge').textContent = `${Math.round((lane.start || 0) * 100)}%-${Math.round((lane.end ?? 1) * 100)}%`;
    setHint(`Sample trim: ${Math.round((lane.start || 0) * 100)}% to ${Math.round((lane.end ?? 1) * 100)}%`);
    emit('setSampler', { lane: lane.slot, field: dragWhich, value: val, gesture });
  };

  canvas.addEventListener('pointermove', e => {
    const r = canvas.getBoundingClientRect?.() || { left: 0, width: 242 };
    const frac = clamp((e.clientX - r.left) / (r.width || 242), 0, 1);
    if (!dragWhich) {
      const nearS = Math.abs(frac - (lane.start || 0)) * (r.width || 242) < 12;
      const nearE = Math.abs(frac - (lane.end ?? 1)) * (r.width || 242) < 12;
      if (canvas.style) canvas.style.cursor = (nearS || nearE) ? 'ew-resize' : 'default';
      return;
    }
    const now = Date.now();
    if (!started) { started = true; lastEmit = now; updateTrim(e, 'start'); }
    else if (now - lastEmit >= 40) { lastEmit = now; updateTrim(e, 'move'); }
  });
  canvas.addEventListener('pointerdown', e => {
    e.preventDefault?.();
    const r = canvas.getBoundingClientRect?.() || { left: 0, width: 242 };
    const frac = clamp((e.clientX - r.left) / (r.width || 242), 0, 1);
    dragWhich = Math.abs(frac - (lane.start || 0)) <= Math.abs(frac - (lane.end ?? 1)) ? 'start' : 'end';
    started = true; lastEmit = Date.now(); activeLiveDrag++;
    canvas.setPointerCapture?.(e.pointerId);
    updateTrim(e, 'start');
  });
  const end = () => {
    if (!dragWhich) return;
    const field = dragWhich;
    const val = field === 'start' ? (lane.start || 0) : (lane.end ?? 1);
    dragWhich = null; started = false;
    activeLiveDrag = Math.max(0, activeLiveDrag - 1);
    emit('setSampler', { lane: lane.slot, field, value: val, gesture: 'end' });
  };
  canvas.addEventListener('pointerup', end);
  canvas.addEventListener('pointercancel', end);
}

/* ---------- Inspector ---------- */
function renderInspector() {
  if (editingInspector()) return;
  const l = currentLane();
  if (!l) {
    $('inspector').innerHTML = `<div class="card"><span class="label">No lane selected</span><p class="caption">Select or add a lane to edit its sound and routing.</p><button class="btn" data-action="addLane">+ Add lane</button></div>`;
    return;
  }
  const sampleLabel = l.sampleKind === 'builtin'
    ? `Built-in ${['kick', 'snare', 'closed hat', 'open hat'][l.sampleBuiltin] || 'sound'}`
    : (l.samplePath ? String(l.samplePath).split(/[\\/]/).pop() : 'No sample loaded');
  const busOpts = Array.from({ length: 16 }, (_, i) => `<option value="${i + 1}" ${l.output === i + 1 ? 'selected' : ''}>${i === 0 ? 'Main' : 'Out ' + (i + 1)}</option>`).join('');
  const chokeOpts = Array.from({ length: 17 }, (_, i) => `<option value="${i}" ${l.choke === i ? 'selected' : ''}>${i === 0 ? 'None' : 'Group ' + i}</option>`).join('');

  $('inspector').innerHTML =
    `<div class="lane-title" style="--lane:${esc(l.color || '#D99B38')}">` +
      `<button class="color-dot-btn" id="laneColorBtn" data-action="colorPop" title="Change lane color" aria-label="Change lane color"><span></span></button>` +
      `<input data-field="name" data-lane="${l.slot}" value="${esc(l.name)}" aria-label="Lane name">` +
      `<button class="btn ghost icon-only" data-action="audition" title="Audition lane" aria-label="Audition lane">` +
        `<svg viewBox="0 0 16 16" fill="none" stroke="currentColor" stroke-width="1.5" stroke-linecap="round" stroke-linejoin="round" aria-hidden="true"><path d="M3 6.5h2.5L9 3.5v9l-3.5-3H3v-3Z"/><path d="M11.5 5.5a3.5 3.5 0 0 1 0 5"/></svg>` +
      `</button>` +
    `</div>` +
    `<div class="segmented" role="radiogroup" aria-label="Lane target">` +
      `<button data-target="sample" aria-checked="${l.target === 'sample'}">Sample</button>` +
      `<button data-target="midi" aria-checked="${l.target === 'midi'}">MIDI Out</button>` +
    `</div>` +
    (l.target === 'sample'
      ? `<div class="card">` +
          `<div class="card-head"><span class="label">Sample · Drag handles to trim</span><button class="btn mini-btn ghost" data-action="chooseSample">Choose</button></div>` +
          `<div class="wave-wrap"><canvas id="waveCanvas" class="wave"></canvas></div>` +
          `<div class="wave-meta"><span class="caption" title="${esc(l.samplePath || sampleLabel)}">${esc(sampleLabel)}</span><span class="badge" id="waveTrimBadge">${Math.round((l.start || 0) * 100)}%-${Math.round((l.end ?? 1) * 100)}%</span></div>` +
          (l.sampleError || l.sampleMissing
            ? `<div class="sample-warn-box" role="status"><span class="sample-warn-msg">${esc(l.sampleError || 'Sample file is missing.')}</span><div class="tools-grid"><button class="btn mini-btn" data-action="retrySample" ${l.samplePath ? '' : 'disabled'}>Retry</button><button class="btn mini-btn" data-action="relinkFolder">Relink folder</button></div></div>`
            : '') +
        `</div>` +
        `<div class="card">` +
          `<div class="card-head"><span class="label">Envelope &amp; Playback</span><button class="btn mini-btn ${l.reverse ? 'active' : ''}" data-reverse="${l.reverse ? '0' : '1'}" aria-pressed="${!!l.reverse}">Reverse</button></div>` +
          `<div class="segmented" role="radiogroup" aria-label="Playback mode"><button data-playmode="oneshot" aria-checked="${(l.playMode || 'oneshot') === 'oneshot'}">One-Shot</button><button data-playmode="gate" aria-checked="${l.playMode === 'gate'}">Gate</button></div>` +
          `<div class="knobs cols-4" id="envKnobs"></div>` +
        `</div>` +
        `<div class="card">` +
          `<div class="card-head"><span class="label">Filter</span></div>` +
          `<div class="segmented" role="radiogroup" aria-label="Filter mode"><button data-filter="off" aria-checked="${(l.filterMode || 'off') === 'off'}">Off</button><button data-filter="lp" aria-checked="${l.filterMode === 'lp'}">Low-pass</button><button data-filter="hp" aria-checked="${l.filterMode === 'hp'}">High-pass</button></div>` +
          `<div class="knobs cols-2 ${(l.filterMode || 'off') === 'off' ? 'dimmed' : ''}" id="filterKnobs"></div>` +
        `</div>`
      : `<div class="card">` +
          `<div class="card-head"><span class="label">MIDI Output</span></div>` +
          `<div class="row2">` +
            `<label class="field"><span class="field-label">Channel</span><input class="num-input" data-field="channel" data-lane="${l.slot}" type="number" min="1" max="16" value="${l.channel}"></label>` +
            `<label class="field"><span class="field-label">Base note (${noteName(l.note)})</span><input class="num-input" data-field="note" data-lane="${l.slot}" type="number" min="0" max="127" value="${l.note}"></label>` +
          `</div>` +
          `<div class="row2">` +
            `<label class="field"><span class="field-label">Bend range</span><input class="num-input" data-field="bendRange" data-lane="${l.slot}" type="number" min="0" max="48" value="${l.bendRange ?? 2}"></label>` +
            `<label class="field"><span class="field-label">Mod A / B CC</span><div class="row2"><input class="num-input" data-field="modACc" data-lane="${l.slot}" type="number" min="0" max="127" value="${l.modACc ?? 74}"><input class="num-input" data-field="modBCc" data-lane="${l.slot}" type="number" min="0" max="127" value="${l.modBCc ?? 71}"></div></label>` +
          `</div>` +
        `</div>`) +
    `<div class="card">` +
      `<div class="card-head"><span class="label">Mix &amp; Routing</span></div>` +
      `<div class="knobs" id="mixKnobs"></div>` +
      `<div class="row2">` +
        `<label class="field"><span class="field-label">Steps (1-512)</span><input class="num-input" data-field="length" data-lane="${l.slot}" type="number" min="1" max="512" value="${l.length}"></label>` +
        (l.target === 'sample'
          ? `<label class="field"><span class="field-label">Output</span><select data-field="output" data-lane="${l.slot}">${busOpts}</select></label>`
          : `<div class="field"><span class="field-label">Route</span><div class="num-input" style="display:flex;align-items:center;color:var(--text-muted)">Ch ${l.channel} · ${noteName(l.note)}</div></div>`) +
      `</div>` +
      (l.target === 'sample' ? `<label class="field"><span class="field-label">Choke group</span><select data-field="choke" data-lane="${l.slot}">${chokeOpts}</select></label>` : '') +
      `<div class="row2">` +
        `<button class="btn mini-btn" data-action="midiDragLane">⋮⋮ Lane MIDI</button>` +
        `<button class="btn mini-btn" data-action="audioDragLane">⋮⋮ Lane WAV</button>` +
      `</div>` +
    `</div>`;

  const laneCol = l.color || '#D99B38';
  const emitLaneLive = (field, value, gesture) => emit('setLane', gesture ? { lane: l.slot, field, value, gesture } : { lane: l.slot, field, value });
  const emitSamplerLive = (field, value, gesture) => emit('setSampler', gesture ? { lane: l.slot, field, value, gesture } : { lane: l.slot, field, value });

  if (l.target === 'sample') {
    const wc = $('waveCanvas');
    drawWaveform(wc, l);
    bindWaveformDrag(wc, l);

    if (typeof document.createElementNS === 'function') {
      const envSlot = $('envKnobs');
      if (envSlot) {
        for (const k of [
          { label: 'Tune', min: -2400, max: 2400, step: 50, def: 0, value: l.tune ?? 0, bipolar: true, fmt: v => (v > 0 ? '+' : '') + (v / 100).toFixed(1), onChange: (v, g) => { l.tune = Math.round(v); emitSamplerLive('tune', l.tune, g); } },
          { label: 'Attack', min: 0, max: 500, step: 1, def: 1, value: l.attack ?? 1, fmt: v => Math.round(v) + 'ms', onChange: (v, g) => { l.attack = v; emitSamplerLive('attack', v, g); } },
          { label: 'Decay', min: 0, max: 5000, step: 10, def: 0, value: l.decay ?? 0, fmt: v => v === 0 ? 'Off' : Math.round(v) + 'ms', onChange: (v, g) => { l.decay = v; emitSamplerLive('decay', v, g); } },
          { label: 'Release', min: 0, max: 2000, step: 5, def: 1, value: l.release ?? 1, fmt: v => Math.round(v) + 'ms', onChange: (v, g) => { l.release = v; emitSamplerLive('release', v, g); } },
        ]) envSlot.appendChild(makeKnob({ ...k, color: laneCol }));
      }
      const fltSlot = $('filterKnobs');
      if (fltSlot) {
        for (const k of [
          { label: 'Cutoff', min: 20, max: 20000, def: 20000, log: true, value: l.filterCutoff ?? 20000, fmt: v => v >= 1000 ? (v / 1000).toFixed(1) + 'k' : Math.round(v) + 'Hz', onChange: (v, g) => { l.filterCutoff = Math.round(v); emitSamplerLive('filterCutoff', l.filterCutoff, g); } },
          { label: 'Res', min: 0, max: 0.95, step: 0.01, def: 0, value: l.filterResonance ?? 0, fmt: v => Math.round(v * 100) + '%', onChange: (v, g) => { l.filterResonance = v; emitSamplerLive('filterResonance', v, g); } },
        ]) fltSlot.appendChild(makeKnob({ ...k, color: laneCol }));
      }
    }
  }

  const mixSlot = $('mixKnobs');
  if (mixSlot && typeof document.createElementNS === 'function') {
    for (const k of [
      { label: 'Volume', min: 0, max: 10000, step: 100, def: 5000, value: l.volume ?? 5000, fmt: v => Math.round(v / 100) + '%', onChange: (v, g) => { l.volume = Math.round(v); emitLaneLive(LANE_FIELDS.volume, l.volume, g); } },
      { label: 'Pan', min: -10000, max: 10000, step: 200, def: 0, value: l.pan ?? 0, bipolar: true, fmt: v => Math.abs(v) < 100 ? 'C' : Math.round(Math.abs(v) / 100) + (v < 0 ? 'L' : 'R'), onChange: (v, g) => { l.pan = Math.round(v); emitLaneLive(LANE_FIELDS.pan, l.pan, g); } },
      { label: 'Swing', min: 0, max: 10000, step: 100, def: 10000, value: l.swing ?? 10000, fmt: v => Math.round(v / 100) + '%', onChange: (v, g) => { l.swing = Math.round(v); emitLaneLive(LANE_FIELDS.swing, l.swing, g); } },
    ]) mixSlot.appendChild(makeKnob({ ...k, color: laneCol }));
  }
}

/* ---------- Sample Browser ---------- */
function browserEntries() {
  const entries = state.browserListing?.entries || [];
  const q = browserFilter.trim().toLowerCase();
  return q ? entries.filter(e => String(e.name || '').toLowerCase().includes(q)) : entries;
}
function renderLayoutClasses() {
  if ($('browserPane')) $('browserPane').hidden = !browserOpen;
  if ($('mainLayout')) {
    $('mainLayout').className = 'workspace' + (browserOpen ? ' browser-open' : '') + (inspectorOpen ? '' : ' inspector-closed');
  }
  if ($('browserToggleBtn')) {
    $('browserToggleBtn').setAttribute('aria-pressed', String(browserOpen));
    $('browserToggleBtn').className = 'btn ghost' + (browserOpen ? ' active' : '');
  }
  if ($('browserAutoSwapBtn')) {
    $('browserAutoSwapBtn').setAttribute('aria-pressed', String(browserAutoSwap));
    $('browserAutoSwapBtn').className = 'btn mini-btn' + (browserAutoSwap ? ' active' : '');
  }
}
function renderBrowser() {
  renderLayoutClasses();
  const activePath = state.browserListing?.path || '';
  if ($('browserBreadcrumb')) $('browserBreadcrumb').textContent = activePath || 'Select or add a folder';
  if ($('browserRoots')) {
    const roots = state.browserFolders || [];
    $('browserRoots').innerHTML = roots.length
      ? roots.map(r => `<div class="browser-root"><button class="root-btn ${activePath === r.path ? 'active' : ''}" data-root="${esc(r.path)}" title="${esc(r.path)}">${esc(r.name || r.path)}</button><button class="root-rm" data-rm-root="${esc(r.path)}" aria-label="Remove folder">×</button></div>`).join('')
      : `<div class="caption">Add a drum folder to browse and hot-swap samples.</div>`;
  }
  if ($('browserUpBtn')) $('browserUpBtn').disabled = !state.browserListing?.parent;
  if ($('browserList')) {
    const entries = browserEntries();
    const loadedPath = currentLane()?.samplePath || '';
    const laneCol = currentLane()?.color || '#D99B38';
    $('browserList').innerHTML = entries.map((e, idx) =>
      `<div class="browser-item ${idx === browserCursor ? 'selected' : ''} ${!e.isDir && e.path === loadedPath ? 'loaded' : ''}" style="--lane-accent:${esc(laneCol)}" data-bidx="${idx}" data-bpath="${esc(e.path)}" data-bdir="${e.isDir ? '1' : '0'}" role="option">` +
      `<span class="item-name">${e.isDir ? '▸ ' : ''}${esc(e.name)}</span>` +
      (!e.isDir ? `<button class="item-add" data-add-sample="${esc(e.path)}" title="Add as new lane">+</button>` : '') +
      `</div>`
    ).join('');
    $('browserList').querySelector?.('.browser-item.selected')?.scrollIntoView?.({ block: 'nearest' });
  }
}

function renderAll() {
  if (!currentLane()) selectedSlot = state.lanes?.[0]?.slot ?? 0;
  renderTop(); renderMidiPreviewBanner(); renderBrowser(); renderHeaders(); drawRuler(); drawGrid(); renderStepTabs(); drawBars(); renderInspector();
}

/* ---------- 60fps Meter & Hit-Flash Loop ---------- */
function tickVisuals() {
  rafScheduled = false;
  let keepGoing = false;
  const lanes = state.lanes || [];
  for (const l of lanes) {
    const target = meterTargets[l.slot] || 0;
    const cur = meterDisplay[l.slot] || 0;
    const next = target > cur ? target : cur * 0.86;
    meterDisplay[l.slot] = next < 0.008 ? 0 : next;
    meterTargets[l.slot] = 0;
    if (meterDisplay[l.slot] > 0) keepGoing = true;
    const el = $('meter-' + l.slot);
    if (el) {
      el.style.height = Math.round(meterDisplay[l.slot] * 100) + '%';
      el.style.background = meterDisplay[l.slot] > 0.92 ? THEME.danger : THEME.green;
    }
  }
  const hasFlashes = Object.keys(hitFlashes).length > 0;
  if (hasFlashes) { drawGrid(); keepGoing = true; }
  if (keepGoing && typeof requestAnimationFrame === 'function') {
    rafScheduled = true;
    requestAnimationFrame(tickVisuals);
  }
}
function ensureVisualLoop() {
  if (!rafScheduled && typeof requestAnimationFrame === 'function') {
    rafScheduled = true;
    requestAnimationFrame(tickVisuals);
  }
}

/* ---------- Popovers: Pattern Bank, Groove, Kit/File, Lane Tools, Swatches ---------- */
function showPatternPopover() {
  if (openMenu && openMenuTrigger === $('patternBtn')) { closeMenu(); return; }
  closeMenu();
  const r = $('patternWrap')?.getBoundingClientRect?.() || $('patternBtn')?.getBoundingClientRect?.() || { left: 100, bottom: 44 };
  const pop = document.createElement('div');
  pop.className = 'bank-pop';
  activeBank = Math.floor((state.pattern || 0) / 16);
  const nonEmpty = new Set(state.nonEmptyPatterns || []);
  const pending = transport.playing ? transport.pendingPattern : state.pendingPattern;
  const renderPop = () => {
    const names = state.patterns || ['Pattern 1'];
    const base = activeBank * 16;
    const chOpts = Array.from({ length: 17 }, (_, i) => `<option value="${i}" ${(state.patternMidiChannel || 0) === i ? 'selected' : ''}>${i === 0 ? 'Keyswitch: Off' : 'Keyswitch Ch ' + i}</option>`).join('');
    pop.innerHTML =
      `<div class="card-head"><span class="label">256-Pattern Bank</span><span class="badge">Bank ${activeBank + 1}</span></div>` +
      `<div class="bank-tabs">${Array.from({ length: 16 }, (_, b) => `<button class="bank-tab ${b === activeBank ? 'active' : ''}" data-bank="${b}">B${b + 1}</button>`).join('')}</div>` +
      `<div class="bank-grid">${Array.from({ length: 16 }, (_, i) => {
        const p = base + i;
        const pname = names[p] || `Pattern ${p + 1}`;
        return `<button class="pat-cell ${p === state.pattern ? 'active' : ''} ${p === pending && p !== state.pattern ? 'queued' : ''} ${nonEmpty.has(p) ? 'has-data' : ''}" data-pat="${p}" title="${esc(pname)}">` +
          `<span class="pc-num">${String(p + 1).padStart(2, '0')}</span><span class="pc-name">${esc(pname)}</span></button>`;
      }).join('')}</div>` +
      `<div class="tools-grid"><button class="btn mini-btn" data-pop="rename">Rename</button><button class="btn mini-btn" data-pop="clone">Clone</button><button class="btn mini-btn danger" data-pop="clear">Clear</button></div>` +
      `<select id="patMidiCh">${chOpts}</select>`;
  };
  renderPop();
  pop.addEventListener('click', e => {
    const b = e.target.closest?.('[data-bank],[data-pat],[data-pop]');
    if (!b) return;
    if (b.dataset.bank !== undefined) { activeBank = Number(b.dataset.bank); renderPop(); return; }
    if (b.dataset.pat !== undefined) { closeMenu(); emit('pattern', { value: Number(b.dataset.pat) }); return; }
    const act = b.dataset.pop; closeMenu();
    if (act === 'rename') startInlinePatternRename();
    else if (act === 'clone') emit('clonePattern');
    else if (act === 'clear') emit('clearPattern');
  });
  pop.addEventListener('change', e => {
    if (e.target.id === 'patMidiCh') emit('setPatternMidiChannel', { value: Number(e.target.value) });
  });
  document.body.appendChild(pop);
  pop.style.left = Math.min(r.left, (window.innerWidth || 1000) - 336) + 'px';
  pop.style.top = (r.bottom + 6) + 'px';
  openMenu = pop;
  openMenuTrigger = $('patternBtn');
}

function showGroovePopover() {
  if (openMenu && openMenuTrigger === $('grooveBtn')) { closeMenu(); return; }
  closeMenu();
  const r = $('grooveBtn')?.getBoundingClientRect?.() || { left: 400, bottom: 44 };
  const pop = document.createElement('div');
  pop.className = 'popover groove-pop';
  const drawGroove = () => {
    pop.innerHTML =
      `<div class="card-head"><span class="label">Groove &amp; Chance</span></div>` +
      `<div class="groove-row"><span class="groove-label">Truncate swung notes</span><button class="btn mini-btn ${state.truncate ? 'active' : ''}" id="truncateBtn" aria-pressed="${!!state.truncate}">${state.truncate ? 'On' : 'Off'}</button></div>` +
      `<div class="groove-row"><span class="groove-label">Probability seed</span><button class="btn mini-btn" id="seedBtn">#${state.seed ?? 0} ↻</button></div>`;
  };
  drawGroove();
  pop.addEventListener('click', e => {
    if (e.target.closest?.('#truncateBtn')) {
      state.truncate = !state.truncate;
      emit('setRack', { field: RACK_FIELDS.truncate, value: state.truncate ? 1 : 0 });
      drawGroove();
    } else if (e.target.closest?.('#seedBtn')) {
      emit('rerollSeed');
    }
  });
  document.body.appendChild(pop);
  pop.style.left = Math.min(r.left, (window.innerWidth || 1000) - 238) + 'px';
  pop.style.top = (r.bottom + 6) + 'px';
  openMenu = pop;
  openMenuTrigger = $('grooveBtn');
}

function showLaneMenu(slot, x, y, trigger = null) {
  showMenu(x, y, [
    { label: 'Audition', action: () => emit('audition', { lane: slot }) },
    { label: 'Duplicate lane', action: () => emit('duplicateLane', { lane: slot }) },
    { label: 'Drag lane MIDI', action: () => emit('midiDragStart', { lane: slot }) },
    { label: 'Drag lane WAV', action: () => emit('audioDragStart', { lane: slot, includeTail: audioDragTail }) },
    '-',
    { label: 'Fill every 2', action: () => emit('fillLane', { lane: slot, every: 2 }) },
    { label: 'Fill every 3', action: () => emit('fillLane', { lane: slot, every: 3 }) },
    { label: 'Fill every 4', action: () => emit('fillLane', { lane: slot, every: 4 }) },
    { label: 'Fill every 8', action: () => emit('fillLane', { lane: slot, every: 8 }) },
    { label: 'Rotate left', action: () => emit('rotateLane', { lane: slot, direction: -1 }) },
    { label: 'Rotate right', action: () => emit('rotateLane', { lane: slot, direction: 1 }) },
    { label: 'Invert', action: () => emit('invertLane', { lane: slot }) },
    { label: 'Clear steps', action: () => emit('clearLane', { lane: slot }) },
    '-',
    { label: 'Delete lane', danger: true, action: () => emit('removeLane', { lane: slot }) },
  ], { trigger });
}

/* ---------- Top Bar Events ---------- */
$('patternBtn')?.addEventListener('click', showPatternPopover);
$('patPrevBtn')?.addEventListener('click', () => emit('pattern', { value: ((state.pattern || 0) + 255) % 256 }));
$('patNextBtn')?.addEventListener('click', () => emit('pattern', { value: ((state.pattern || 0) + 1) % 256 }));
$('grooveBtn')?.addEventListener('click', showGroovePopover);
$('fileMenuBtn')?.addEventListener('click', () => {
  if (openMenu && openMenuTrigger === $('fileMenuBtn')) { closeMenu(); return; }
  const r = $('fileMenuBtn')?.getBoundingClientRect?.() || { right: 880, bottom: 44 };
  showMenu(r.right, r.bottom + 6, [
    { label: 'Save kit (.furrowkit)', action: () => emit('kitSave') },
    { label: 'Load kit (.furrowkit)', action: () => emit('kitLoad') },
    '-',
    { label: 'Export pattern as MIDI...', action: () => emit('midiExport') },
    { label: 'Import MIDI file...', action: () => emit('midiPreview') },
    '-',
    { label: 'Relink missing samples...', action: () => emit('relinkFolder') },
  ], { alignRight: true, trigger: $('fileMenuBtn') });
});

$('standalonePlayBtn')?.addEventListener('click', e => {
  e?.target?.blur?.();
  emit('standaloneTransport', { playing: !transport.playing });
});
$('fullscreenBtn')?.addEventListener('click', e => {
  e?.target?.blur?.();
  emit('toggleFullscreen');
});
$('bpmInput')?.addEventListener('change', e => emit('standaloneTransport', { bpm: clamp(Number(e.target.value) || 120, 20, 300) }));
$('bpmWrap')?.addEventListener('pointerdown', e => {
  if (e.target === $('bpmInput')) return;
  e.preventDefault();
  let startY = e.clientY, startBpm = Math.round(transport.bpm || state.bpm || 120);
  const mv = ev => {
    const next = clamp(Math.round(startBpm + (startY - ev.clientY) * 0.4), 20, 300);
    if ($('bpmInput')) $('bpmInput').value = String(next);
    emit('standaloneTransport', { bpm: next });
  };
  const up = () => { document.removeEventListener('pointermove', mv); document.removeEventListener('pointerup', up); };
  document.addEventListener('pointermove', mv); document.addEventListener('pointerup', up);
});
$('dragMidiBtn')?.addEventListener('pointerdown', e => { if (e.button === 0) emit('midiDragStart', { lane: -1 }); });
$('dragWavBtn')?.addEventListener('pointerdown', e => { if (e.button === 0) emit('audioDragStart', { lane: -1, includeTail: audioDragTail }); });
$('wavTailBtn')?.addEventListener('click', () => { audioDragTail = !audioDragTail; renderTop(); });
$('browserToggleBtn')?.addEventListener('click', () => {
  browserOpen = !browserOpen;
  if (browserOpen && (window.innerWidth || 1100) < 920) inspectorOpen = false;
  if (browserOpen && !state.browserListing && state.browserFolders?.length)
    emit('browserList', { path: state.browserFolders[0].path });
  renderTop(); renderBrowser(); drawRuler(); drawGrid(); drawBars();
});
$('inspectorToggleBtn')?.addEventListener('click', () => {
  inspectorOpen = !inspectorOpen;
  renderTop(); renderLayoutClasses(); drawRuler(); drawGrid(); drawBars();
});
$('editorToggleBtn')?.addEventListener('click', () => {
  editorOpen = !editorOpen;
  renderTop();
});
$('zoomPills')?.addEventListener('click', e => {
  const b = e.target.closest?.('[data-zoom]'); if (!b) return;
  zoomMode = b.dataset.zoom;
  $('zoomPills').querySelectorAll?.('button').forEach(btn => btn.classList.toggle('active', btn === b));
  drawRuler(); drawGrid(); drawBars();
});
document.querySelector?.('.topbar')?.addEventListener('click', e => {
  const b = e.target.closest?.('[data-action]');
  if (b && !b.disabled) emit(b.dataset.action);
});
$('midiPreviewBar')?.addEventListener('click', e => {
  const b = e.target.closest?.('[data-action]');
  if (!b || b.disabled) return;
  if (b.dataset.action === 'midiImport' && !state.midiPreview?.accepted) return;
  emit(b.dataset.action);
});

/* ---------- Sample Browser Events ---------- */
$('browserAutoSwapBtn')?.addEventListener('click', () => { browserAutoSwap = !browserAutoSwap; renderBrowser(); });
$('browserAddFolderBtn')?.addEventListener('click', () => emit('browserAddFolder'));
$('browserUpBtn')?.addEventListener('click', () => { if (state.browserListing?.parent) { browserCursor = -1; emit('browserList', { path: state.browserListing.parent }); } });
$('browserFilterInput')?.addEventListener('input', e => { browserFilter = e.target.value || ''; browserCursor = -1; renderBrowser(); });
$('browserRoots')?.addEventListener('click', e => {
  const rm = e.target.closest?.('[data-rm-root]');
  if (rm) { emit('browserRemoveFolder', { path: rm.dataset.rmRoot }); return; }
  const b = e.target.closest?.('[data-root]');
  if (b) { browserCursor = -1; emit('browserList', { path: b.dataset.root }); }
});
$('browserList')?.addEventListener('click', e => {
  const add = e.target.closest?.('[data-add-sample]');
  if (add) { emit('addSampleFiles', { paths: [add.dataset.addSample] }); return; }
  const item = e.target.closest?.('[data-bidx]');
  if (!item) return;
  browserCursor = Number(item.dataset.bidx);
  if (item.dataset.bdir === '1') { browserCursor = -1; emit('browserList', { path: item.dataset.bpath }); return; }
  if (browserAutoSwap) emit('browserHotSwap', { lane: selectedSlot, path: item.dataset.bpath });
  else emit('browserPreview', { path: item.dataset.bpath });
  renderBrowser();
});
$('browserList')?.addEventListener('dblclick', e => {
  const item = e.target.closest?.('[data-bpath]');
  if (item && item.dataset.bdir !== '1') emit('browserHotSwap', { lane: selectedSlot, path: item.dataset.bpath });
});
$('browserList')?.addEventListener('keydown', e => {
  const entries = browserEntries();
  if (!entries.length) return;
  if (e.key === 'ArrowDown' || e.key === 'ArrowUp') {
    e.preventDefault();
    browserCursor = clamp(browserCursor + (e.key === 'ArrowDown' ? 1 : -1), 0, entries.length - 1);
    const cur = entries[browserCursor];
    if (cur && !cur.isDir) {
      if (browserAutoSwap) emit('browserHotSwap', { lane: selectedSlot, path: cur.path });
      else emit('browserPreview', { path: cur.path });
    }
    renderBrowser();
  } else if (e.key === 'Enter' && browserCursor >= 0 && entries[browserCursor]) {
    e.preventDefault();
    const cur = entries[browserCursor];
    if (cur.isDir) { browserCursor = -1; emit('browserList', { path: cur.path }); }
    else emit('browserHotSwap', { lane: selectedSlot, path: cur.path });
  } else if (e.key === 'Escape') {
    emit('browserStopPreview');
  }
});

/* ---------- Lane Header Events ---------- */
$('laneHeaders')?.addEventListener('click', e => {
  const more = e.target.closest?.('[data-more]');
  if (more) {
    const slot = Number(more.dataset.more);
    selectedSlot = slot;
    emit('selectLane', { lane: slot });
    renderHeaders(); drawGrid(); renderStepTabs(); drawBars(); renderInspector();
    const r = more.getBoundingClientRect?.() || { left: 200, bottom: 120 };
    showLaneMenu(slot, r.left, r.bottom + 4, more);
    return;
  }
  const b = e.target.closest?.('[data-action],[data-lane]');
  if (!b) return;
  if (b.dataset.action === 'addLane') { emit('addLane', { note: 36 + ((state.lanes?.length || 0) % 48) }); return; }
  const slot = Number(b.dataset.lane);
  selectedSlot = slot;
  const l = currentLane();
  if (!l) return;
  if (b.dataset.action === 'mute' || b.dataset.action === 'solo') {
    emit('setLane', { lane: slot, field: LANE_FIELDS[b.dataset.action], value: l[b.dataset.action] ? 0 : 1 });
    return;
  }
  emit('selectLane', { lane: slot });
  renderHeaders(); drawGrid(); renderStepTabs(); drawBars(); renderInspector();
});
$('laneHeaders')?.addEventListener('dblclick', e => {
  const b = e.target.closest?.('.lane-name');
  if (b) emit('audition', { lane: Number(b.dataset.lane) });
});
$('laneHeaders')?.addEventListener('contextmenu', e => {
  const b = e.target.closest?.('.lane-row');
  if (!b) return;
  e.preventDefault();
  const slot = Number(b.dataset.lane);
  selectedSlot = slot;
  emit('selectLane', { lane: slot });
  renderHeaders(); drawGrid(); renderStepTabs(); drawBars(); renderInspector();
  showLaneMenu(slot, e.clientX, e.clientY);
});

$('laneHeaders')?.addEventListener('dragstart', e => {
  const row = e.target.closest?.('.lane-row'); if (!row) return;
  dragLaneSlot = Number(row.dataset.lane);
});
$('laneHeaders')?.addEventListener('dragover', e => {
  const row = e.target.closest?.('.lane-row');
  if (dragLaneSlot < 0 || !row) return;
  e.preventDefault(); row.classList.add('drag-over');
});
$('laneHeaders')?.addEventListener('dragleave', e => e.target.closest?.('.lane-row')?.classList.remove('drag-over'));
$('laneHeaders')?.addEventListener('drop', e => {
  const row = e.target.closest?.('.lane-row');
  if (dragLaneSlot < 0 || !row) return;
  e.preventDefault(); row.classList.remove('drag-over');
  const targetSlot = Number(row.dataset.lane);
  const src = dragLaneSlot; dragLaneSlot = -1;
  if (src === targetSlot) return;
  const slots = (state.lanes || []).map(l => l.slot);
  const from = slots.indexOf(src), to = slots.indexOf(targetSlot);
  if (from < 0 || to < 0) return;
  slots.splice(from, 1); slots.splice(to, 0, src);
  emit('reorderLanes', { order: slots });
});

/* Bidirectional scroll sync across headers, grid, ruler, and bars */
let syncingScroll = false;
$('gridScroll')?.addEventListener('scroll', () => {
  if (syncingScroll) return;
  syncingScroll = true;
  if ($('laneHeaders')) $('laneHeaders').scrollTop = $('gridScroll').scrollTop;
  if ($('barsScroll')) $('barsScroll').scrollLeft = $('gridScroll').scrollLeft;
  drawRuler();
  syncingScroll = false;
});
$('laneHeaders')?.addEventListener('scroll', () => {
  if (syncingScroll) return;
  syncingScroll = true;
  if ($('gridScroll')) $('gridScroll').scrollTop = $('laneHeaders').scrollTop;
  syncingScroll = false;
});
$('barsScroll')?.addEventListener('scroll', () => {
  if (syncingScroll) return;
  syncingScroll = true;
  if ($('gridScroll')) $('gridScroll').scrollLeft = $('barsScroll').scrollLeft;
  drawRuler();
  syncingScroll = false;
});

/* ---------- Grid Canvas Interactions ---------- */
function gridHit(e) {
  const canvas = $('gridCanvas'), r = canvas.getBoundingClientRect?.() || { left: 0, top: 0 };
  const x = e.clientX - r.left, y = e.clientY - r.top;
  const row = Math.floor(y / ROW_H);
  const lane = (state.lanes || [])[row];
  if (!lane) return null;
  const step = xToStep(x, Number(lane.length) || 16, getCellW(maxCols()));
  if (step < 0) return null;
  return { lane, step };
}
$('gridCanvas')?.addEventListener('contextmenu', e => e.preventDefault());
$('gridCanvas')?.addEventListener('pointerleave', () => { hoverGrid = null; drawGrid(); });
$('gridCanvas')?.addEventListener('pointerdown', e => {
  const hit = gridHit(e); if (!hit) return;
  e.preventDefault(); $('gridCanvas').focus?.();
  const prev = selectedSlot;
  selectedSlot = hit.lane.slot; focusCell = { slot: hit.lane.slot, step: hit.step };
  emit('selectLane', { lane: hit.lane.slot });
  if (prev !== hit.lane.slot) { renderHeaders(); renderStepTabs(); renderInspector(); }

  const st = (hit.lane.steps && hit.lane.steps[hit.step]) || [0, 100, 64, 0, 0, 0, 64, 64, 0, 5000, 1, 0, 100];
  if (e.altKey) {
    const nextProb = PROB_CYCLE[(PROB_CYCLE.indexOf(st[12] ?? 100) + 1) % PROB_CYCLE.length];
    emit('setStepsBatch', { edits: [{ lane: hit.lane.slot, step: hit.step, field: 0, value: 1 }, { lane: hit.lane.slot, step: hit.step, field: 12, value: nextProb }] });
    setHint(`${hit.lane.name} · Step ${hit.step + 1} · Probability ${nextProb}%`);
    return;
  }
  if (e.shiftKey) {
    gridDrag = { mode: 'vel', lane: hit.lane, step: hit.step, startY: e.clientY, startVel: st[1] ?? 100, val: st[1] ?? 100, started: false, lastEmit: 0 };
    st[0] = 1; drawGrid(); drawBars();
  } else {
    const paintVal = e.button === 2 ? 0 : (st[0] ? 0 : 1);
    gridDrag = { mode: 'paint', lane: hit.lane, val: paintVal, map: new Map([[hit.step, paintVal]]) };
    st[0] = paintVal; drawGrid(); drawBars();
  }
  $('gridCanvas').setPointerCapture?.(e.pointerId);
});
$('gridCanvas')?.addEventListener('pointermove', e => {
  if (gridDrag) {
    if (gridDrag.mode === 'vel') {
      const next = clamp(Math.round(gridDrag.startVel + (gridDrag.startY - e.clientY) * 1.1), 1, 127);
      if (next === gridDrag.val) return;
      gridDrag.val = next;
      if (gridDrag.lane.steps?.[gridDrag.step]) gridDrag.lane.steps[gridDrag.step][1] = next;
      setHint(`${gridDrag.lane.name} · Step ${gridDrag.step + 1} · Velocity ${next}`);
      drawGrid(); drawBars();
      const now = Date.now();
      const edits = [{ lane: gridDrag.lane.slot, step: gridDrag.step, field: 0, value: 1 }, { lane: gridDrag.lane.slot, step: gridDrag.step, field: 1, value: next }];
      if (!gridDrag.started) { gridDrag.started = true; gridDrag.lastEmit = now; emit('setStepsBatch', { edits, gesture: 'start' }); }
      else if (now - gridDrag.lastEmit >= 45) { gridDrag.lastEmit = now; emit('setStepsBatch', { edits, gesture: 'move' }); }
      return;
    }
    const hit = gridHit(e);
    if (!hit || hit.lane.slot !== gridDrag.lane.slot) return;
    gridDrag.map.set(hit.step, gridDrag.val);
    if (hit.lane.steps?.[hit.step]) hit.lane.steps[hit.step][0] = gridDrag.val;
    drawGrid(); drawBars();
    return;
  }
  const hit = gridHit(e);
  const nextH = hit ? { slot: hit.lane.slot, step: hit.step } : null;
  if (nextH?.slot !== hoverGrid?.slot || nextH?.step !== hoverGrid?.step) {
    hoverGrid = nextH;
    drawGrid();
    if (hit) {
      const st = (hit.lane.steps && hit.lane.steps[hit.step]) || [];
      setHint(`${hit.lane.name} · Step ${hit.step + 1}${st[0] ? ` · Vel ${st[1] ?? 100} · Prob ${st[12] ?? 100}%${(st[10] ?? 1) > 1 ? ' · Ratchet x' + st[10] : ''}` : ' · Off'}`);
    }
  }
});
const endGridDrag = () => {
  if (!gridDrag) return;
  const d = gridDrag; gridDrag = null;
  if (d.mode === 'vel') {
    const edits = [{ lane: d.lane.slot, step: d.step, field: 0, value: 1 }, { lane: d.lane.slot, step: d.step, field: 1, value: d.val }];
    emit('setStepsBatch', d.started ? { edits, gesture: 'end' } : { edits });
  } else {
    const edits = [...d.map.entries()].map(([step, value]) => ({ lane: d.lane.slot, step, field: 0, value }));
    emit(edits.length === 1 ? 'setStep' : 'setStepsBatch', edits.length === 1 ? edits[0] : { edits });
  }
};
$('gridCanvas')?.addEventListener('pointerup', endGridDrag);
$('gridCanvas')?.addEventListener('pointercancel', endGridDrag);
$('gridCanvas')?.addEventListener('keydown', e => {
  const lanes = state.lanes || []; if (!lanes.length) return;
  const li = Math.max(0, lanes.findIndex(l => l.slot === focusCell.slot));
  const lane = lanes[li], len = Number(lane.length) || 16;
  if (e.key === 'Enter' || e.key === ' ') {
    e.preventDefault();
    const on = lane.steps?.[focusCell.step]?.[0] ? 0 : 1;
    emit('setStep', { lane: lane.slot, step: focusCell.step, field: 0, value: on });
  } else if (e.shiftKey && (e.key === 'ArrowUp' || e.key === 'ArrowDown')) {
    e.preventDefault();
    const cur = lane.steps?.[focusCell.step]?.[1] ?? 100;
    const next = clamp(cur + (e.key === 'ArrowUp' ? 8 : -8), 1, 127);
    emit('setStepsBatch', { edits: [{ lane: lane.slot, step: focusCell.step, field: 0, value: 1 }, { lane: lane.slot, step: focusCell.step, field: 1, value: next }] });
  } else if (e.key === 'ArrowRight') { e.preventDefault(); focusCell.step = (focusCell.step + 1) % len; drawGrid(); }
  else if (e.key === 'ArrowLeft') { e.preventDefault(); focusCell.step = (focusCell.step - 1 + len) % len; drawGrid(); }
  else if (e.key === 'ArrowDown' && li + 1 < lanes.length) { e.preventDefault(); focusCell.slot = lanes[li + 1].slot; selectedSlot = focusCell.slot; renderAll(); }
  else if (e.key === 'ArrowUp' && li > 0) { e.preventDefault(); focusCell.slot = lanes[li - 1].slot; selectedSlot = focusCell.slot; renderAll(); }
});

/* ---------- Step Lane Editor Interactions ---------- */
$('stepTabs')?.addEventListener('click', e => {
  const b = e.target.closest?.('[data-tab]'); if (!b) return;
  activeTab = Number(b.dataset.tab); renderStepTabs(); drawBars();
});
function sampleBar(e) {
  const lane = currentLane(); if (!lane) return;
  const canvas = $('barsCanvas'), r = canvas.getBoundingClientRect?.() || { left: 0, top: 0, height: 118 };
  const step = xToStep(e.clientX - r.left, Number(lane.length) || 16, getCellW(maxCols()));
  if (step < 0) return;
  const f = STEP_FIELDS[activeTab];
  const frac = clamp(1 - (e.clientY - r.top - 6) / ((r.height || 118) - 12), 0, 1);
  const val = Math.round(clamp(f.min + frac * (f.max - f.min), f.min, f.max));
  if (barDrag.map.get(step) === val) return;
  barDrag.map.set(step, val);
  if (lane.steps?.[step]) lane.steps[step][f.idx] = val;
  setHint(`${lane.name} · Step ${step + 1} · ${f.label}: ${f.fmt(val)}`);
  drawBars(); drawGrid();
  const now = Date.now();
  const edits = [{ lane: lane.slot, step, field: f.idx, value: val }];
  if (!barDrag.started) { barDrag.started = true; barDrag.lastEmit = now; emit('setStepsBatch', { edits, gesture: 'start' }); }
  else if (now - barDrag.lastEmit >= 45) { barDrag.lastEmit = now; emit('setStepsBatch', { edits, gesture: 'move' }); }
}
$('barsCanvas')?.addEventListener('pointerdown', e => {
  const lane = currentLane(); if (!lane) return;
  e.preventDefault(); barDrag = { lane, field: STEP_FIELDS[activeTab].idx, map: new Map(), started: false, lastEmit: 0 };
  $('barsCanvas').setPointerCapture?.(e.pointerId);
  sampleBar(e);
});
$('barsCanvas')?.addEventListener('pointermove', e => {
  if (barDrag) { sampleBar(e); return; }
  const lane = currentLane(); if (!lane) return;
  const r = $('barsCanvas').getBoundingClientRect?.() || { left: 0 };
  const step = xToStep(e.clientX - r.left, Number(lane.length) || 16, getCellW(maxCols()));
  hoverBar = step >= 0 ? { step } : null;
  drawBars();
  if (step >= 0) {
    const f = STEP_FIELDS[activeTab];
    const val = (lane.steps && lane.steps[step]?.[f.idx]) ?? f.def;
    setHint(`${lane.name} · Step ${step + 1} · ${f.label}: ${f.fmt(val)} (double-click to reset)`);
  }
});
$('barsCanvas')?.addEventListener('pointerleave', () => { hoverBar = null; drawBars(); });
const endBarDrag = () => {
  if (!barDrag) return;
  const d = barDrag; barDrag = null;
  const edits = [...d.map.entries()].map(([step, value]) => ({ lane: d.lane.slot, step, field: d.field, value }));
  if (!edits.length) return;
  if (d.started) emit('setStepsBatch', { edits, gesture: 'end' });
  else emit(edits.length === 1 ? 'setStep' : 'setStepsBatch', edits.length === 1 ? edits[0] : { edits });
};
$('barsCanvas')?.addEventListener('pointerup', endBarDrag);
$('barsCanvas')?.addEventListener('pointercancel', endBarDrag);
$('barsCanvas')?.addEventListener('dblclick', e => {
  const lane = currentLane(); if (!lane) return;
  const r = $('barsCanvas').getBoundingClientRect?.() || { left: 0 };
  const step = xToStep(e.clientX - r.left, Number(lane.length) || 16, getCellW(maxCols()));
  if (step < 0) return;
  const f = STEP_FIELDS[activeTab];
  emit('setStep', { lane: lane.slot, step, field: f.idx, value: f.def });
  setHint(`${lane.name} · Step ${step + 1} · ${f.label} reset to ${f.fmt(f.def)}`);
});

/* ---------- Inspector Events ---------- */
$('inspector')?.addEventListener('change', e => {
  const field = e.target.dataset?.field; if (!field) return;
  const lane = Number(e.target.dataset.lane);
  const value = (e.target.type === 'number' || e.target.tagName === 'SELECT') ? Number(e.target.value) : e.target.value;
  if (field === 'length') emit('setLength', { lane, value });
  else if (field === 'name') emit('renameLane', { lane, name: value });
  else if (field === 'color') emit('colorLane', { lane, color: value });
  else if (field in LANE_FIELDS) emit('setLane', { lane, field: LANE_FIELDS[field], value });
  else emit('setSampler', { lane, field, value });
});
$('inspector')?.addEventListener('click', e => {
  const el = e.target.closest?.('[data-action],[data-target],[data-playmode],[data-reverse],[data-filter],[data-color]');
  if (!el || el.disabled) return;
  if (el.dataset.action === 'colorPop') {
    const l = currentLane(); if (!l) return;
    if (openMenu && openMenuTrigger === el) { closeMenu(); return; }
    closeMenu();
    const r = el.getBoundingClientRect?.() || { left: 820, bottom: 90 };
    const pop = document.createElement('div');
    pop.className = 'popover swatch-pop align-right';
    pop.innerHTML = PALETTE.map(c => `<button class="swatch" data-c="${c}" style="--c:${c}" aria-checked="${(l.color || '').toUpperCase() === c}"></button>`).join('') +
      `<input class="color-pick" id="customColorPick" type="color" value="${esc(l.color || '#D99B38')}" aria-label="Custom color">`;
    pop.addEventListener('click', ev => {
      const sb = ev.target.closest?.('[data-c]'); if (!sb) return;
      emit('colorLane', { lane: selectedSlot, color: sb.dataset.c });
      closeMenu();
    });
    pop.addEventListener('change', ev => {
      if (ev.target.id === 'customColorPick') { emit('colorLane', { lane: selectedSlot, color: ev.target.value }); closeMenu(); }
    });
    document.body.appendChild(pop);
    pop.style.left = Math.min(r.left, (window.innerWidth || 1000) - 210) + 'px';
    pop.style.top = (r.bottom + 6) + 'px';
    openMenu = pop; openMenuTrigger = el;
    return;
  }
  if (el.dataset.target) { emit('setSampler', { lane: selectedSlot, field: 'target', value: el.dataset.target }); return; }
  if (el.dataset.playmode) { emit('setSampler', { lane: selectedSlot, field: 'playMode', value: el.dataset.playmode }); return; }
  if (el.dataset.reverse !== undefined) { emit('setSampler', { lane: selectedSlot, field: 'reverse', value: el.dataset.reverse === '1' }); return; }
  if (el.dataset.filter) { emit('setSampler', { lane: selectedSlot, field: 'filterMode', value: el.dataset.filter }); return; }
  if (el.dataset.color) { emit('colorLane', { lane: selectedSlot, color: el.dataset.color }); return; }
  const action = el.dataset.action;
  if (action === 'addLane') { emit('addLane', { note: 60 }); return; }
  if (action === 'midiDragLane') { emit('midiDragStart', { lane: selectedSlot }); return; }
  if (action === 'audioDragLane') { emit('audioDragStart', { lane: selectedSlot, includeTail: audioDragTail }); return; }
  if (['midiPreview', 'midiImport', 'panic', 'relinkFolder'].includes(action)) { emit(action); return; }
  if (action === 'retrySample') { const path = currentLane()?.samplePath; if (path) emit('retrySample', { lane: selectedSlot, path }); return; }
  emit(action, { lane: selectedSlot });
});

/* ---------- Global Shortcuts & Popover Dismissal ---------- */
document.addEventListener('pointerdown', e => {
  if (openMenu && !openMenu.contains?.(e.target) && (!openMenuTrigger || !openMenuTrigger.contains?.(e.target))) {
    closeMenu();
  }
});
document.addEventListener('keydown', e => {
  if (e.key === 'F11' && state.standalone) {
    e.preventDefault(); emit('toggleFullscreen'); return;
  }
  if (['INPUT', 'TEXTAREA', 'SELECT'].includes(document.activeElement?.tagName)) return;
  if (e.key === 'Escape') { closeMenu(); return; }
  if (e.key === ' ' && state.standalone && document.activeElement !== $('gridCanvas')) {
    e.preventDefault(); emit('standaloneTransport', { playing: !transport.playing }); return;
  }
  if (!e.ctrlKey && !e.metaKey && !e.altKey) {
    if (e.key.toLowerCase() === 'b') { browserOpen = !browserOpen; if (browserOpen && (window.innerWidth || 1100) < 920) inspectorOpen = false; renderTop(); renderBrowser(); drawRuler(); drawGrid(); drawBars(); return; }
    if (e.key.toLowerCase() === 'i') { inspectorOpen = !inspectorOpen; renderTop(); renderLayoutClasses(); drawRuler(); drawGrid(); drawBars(); return; }
  }
  if ((e.ctrlKey || e.metaKey) && e.key.toLowerCase() === 'z') { e.preventDefault(); emit(e.shiftKey ? 'redo' : 'undo'); }
  else if ((e.ctrlKey || e.metaKey) && e.key.toLowerCase() === 'y') { e.preventDefault(); emit('redo'); }
});
window.addEventListener?.('resize', () => {
  if (browserOpen && inspectorOpen && (window.innerWidth || 1100) < 920) {
    inspectorOpen = false;
    renderTop(); renderLayoutClasses();
  }
  drawRuler(); drawGrid(); drawBars();
});

/* ---------- Native JUCE Bridge Subscriptions ---------- */
window.__furrowState = s => {
  state = s || state;
  if (!currentLane()) selectedSlot = state.lanes?.[0]?.slot ?? 0;
  renderAll();
};
window.__furrowTransport = t => {
  if (!t) return;
  const map = {}, mMap = {};
  for (const h of (t.playheads || [])) map[h.slot] = h.step;
  for (const m of (t.meters || [])) mMap[m.slot] = m.peak;
  transport = { ...t, playheads: map, meters: mMap };

  const now = Date.now();
  for (const l of (state.lanes || [])) {
    const p = clamp(mMap[l.slot] || 0, 0, 1);
    if (p > 0) meterTargets[l.slot] = Math.max(meterTargets[l.slot] || 0, p);
    const el = $('meter-' + l.slot);
    if (el && typeof requestAnimationFrame !== 'function') {
      el.style.height = Math.round(p * 100) + '%';
      el.style.background = p > 0.92 ? THEME.danger : THEME.green;
    }
    const step = map[l.slot];
    if (t.playing && step !== undefined && step !== prevPlayheads[l.slot] && l.steps?.[step]?.[0]) {
      hitFlashes[`${l.slot}:${step}`] = now;
    }
  }
  prevPlayheads = map;
  ensureVisualLoop();
  renderTop();
  drawGrid();
  drawBars();
};
window.__JUCE__?.backend?.addEventListener('state', window.__furrowState);
window.__JUCE__?.backend?.addEventListener('transport', window.__furrowTransport);
renderAll();
emit('ready');
