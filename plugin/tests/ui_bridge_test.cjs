const assert = require('node:assert/strict');
const fs = require('node:fs');
const vm = require('node:vm');
const path = require('node:path');

const appDir = path.join(__dirname, '../ui/app');
const html = fs.readFileSync(path.join(appDir, 'index.html'), 'utf8');
const css = fs.readFileSync(path.join(appDir, 'styles.css'), 'utf8');
const js = fs.readFileSync(path.join(appDir, 'app.js'), 'utf8');
const favicon = fs.readFileSync(path.join(appDir, 'favicon.svg'), 'utf8');

for (const [name, content] of [['index.html', html], ['styles.css', css], ['app.js', js], ['favicon.svg', favicon]]) {
  assert.doesNotMatch(content.replaceAll('http://www.w3.org/2000/svg', ''), /https?:\/\//i, `${name} must contain zero external network URLs`);
  assert.doesNotMatch(content, /\bprompt\s*\(/, `${name} must not call window.prompt`);
}
assert.match(html, /href="favicon\.svg"/, 'index.html links local favicon.svg');
assert.match(html, /href="styles\.css"/, 'index.html links local styles.css');
assert.match(html, /src="app\.js"/, 'index.html loads local app.js');

const events = [], elements = {}, subscriptions = {}, docListeners = {}, bodyChildren = [];
function makeElement(id, tagName = 'DIV') {
  const el = {
    id, tagName, innerHTML: '', textContent: '', value: '', className: '', hidden: false, disabled: false,
    scrollTop: 0, scrollLeft: 0, clientWidth: 640,
    dataset: {}, style: {}, attrs: {}, listeners: {}, children: [],
    addEventListener(type, fn) { (this.listeners[type] ||= []).push(fn); },
    removeEventListener(type, fn) {
      if (!this.listeners[type]) return;
      this.listeners[type] = this.listeners[type].filter(f => f !== fn);
    },
    emit(type, ev = {}) { for (const fn of (this.listeners[type] || [])) fn(ev); },
    setAttribute(k, v) { this.attrs[k] = v; },
    getAttribute(k) { return this.attrs[k]; },
    appendChild(c) { c.parent = this; this.children.push(c); if (this.id === 'body') bodyChildren.push(c); return c; },
    append(...cs) { cs.forEach(c => this.appendChild(c)); },
    prepend(c) { c.parent = this; this.children.unshift(c); },
    insertBefore(c) { c.parent = this; this.children.push(c); },
    remove() {
      const idx = bodyChildren.indexOf(this);
      if (idx >= 0) bodyChildren.splice(idx, 1);
    },
    focus() {}, select() {}, blur() {}, setPointerCapture() {}, scrollIntoView() {},
    contains(t) { return t === this || t?.parent === this || this.children.some(c => c.contains?.(t)); },
    querySelector() { return null; },
    querySelectorAll() { return []; },
    getBoundingClientRect() { return { left: 0, right: 240, top: 0, bottom: 40, width: 240, height: 118 }; },
    getContext() {
      return {
        setTransform() {}, clearRect() {}, fillRect() {}, strokeRect() {}, beginPath() {}, closePath() {},
        moveTo() {}, lineTo() {}, rect() {}, roundRect() {}, fill() {}, stroke() {}, save() {}, restore() {},
        clip() {}, translate() {}, fillText() {},
        fillStyle: '', strokeStyle: '', lineWidth: 1, globalAlpha: 1, font: '', textBaseline: '',
      };
    },
  };
  return el;
}
function getEl(id) { return elements[id] ||= makeElement(id); }
const document = {
  activeElement: null,
  body: makeElement('body', 'BODY'),
  getElementById: getEl,
  querySelector: () => getEl('topbar'),
  createElement: tag => makeElement('', tag.toUpperCase()),
  createElementNS: (_ns, tag) => makeElement('', tag.toUpperCase()),
  addEventListener(type, fn) { (docListeners[type] ||= []).push(fn); },
  removeEventListener(type, fn) {
    if (!docListeners[type]) return;
    docListeners[type] = docListeners[type].filter(f => f !== fn);
  },
};
const window = {
  devicePixelRatio: 1, innerWidth: 1100, innerHeight: 700,
  addEventListener() {},
  __JUCE__: {
    backend: {
      emitEvent: (name, value) => events.push({ name, value: JSON.parse(JSON.stringify(value)) }),
      addEventListener: (name, fn) => { subscriptions[name] = fn; },
    },
  },
};

vm.runInNewContext(js, { window, document, console, Date });
assert.equal(typeof subscriptions.state, 'function', 'native state subscription');
assert.equal(typeof subscriptions.transport, 'function', 'native transport subscription');
assert.deepEqual(events.shift(), { name: 'edit', value: { action: 'ready' } }, 'ready handshake on boot');

const lane = {
  slot: 0, name: 'Kick', color: '#D99B38', length: 16, target: 'sample', note: 36, mute: false, solo: false,
  volume: 5000, pan: 0, bendRange: 2, swing: 10000, modACc: 74, modBCc: 71, channel: 1, output: 1,
  start: 0, end: 1, tune: 0, attack: 1, decay: 0, release: 1, choke: 0,
  playMode: 'oneshot', reverse: false, filterMode: 'lp', filterCutoff: 2000, filterResonance: 0.2,
  sampleKind: 'file', samplePath: '/samples/kick.wav', sampleMissing: false, sampleError: '',
  peaks: [0.2, 0.9, 0.4],
  steps: Array.from({ length: 16 }, (_, i) => [i === 0 ? 1 : 0, 100, 64, 0, 0, 0, 64, 64, 0, 5000, 1, 0, 100, 0, 0, 0]),
};
const snapshot = {
  revision: 2, pattern: 0, pendingPattern: -1, patterns: ['Main', 'Fill'], playing: false, bpm: 120,
  globalSwing: 2500, truncate: false, seed: 42, canUndo: true, canRedo: false, error: '',
  lanes: [lane],
  midiPreview: { accepted: true, lanes: 1, lengthSteps: 16, diagnostics: [{ message: '<ok>' }] },
};
subscriptions.state(snapshot);
assert.match(getEl('laneHeaders').innerHTML, /Kick/);
assert.match(getEl('midiPreviewBar').innerHTML, /&lt;ok&gt;/);
assert.match(getEl('stepTabs').innerHTML, /Release/);
assert.match(getEl('stepTabs').innerHTML, /Probability/);

subscriptions.transport({ playing: true, bpm: 128, beat: 1.5, pattern: 0, pendingPattern: 1, playheads: [{ slot: 0, step: 6 }] });
assert.equal(getEl('bpmText').textContent, '128 BPM');
assert.match(getEl('queuedBadge').textContent, /Fill/);

function click(id, dataset) {
  const target = {
    dataset,
    disabled: false,
    closest(sel) {
      const attrs = [...sel.matchAll(/\[data-([a-zA-Z0-9-]+)\]/g)].map(m => m[1].replace(/-([a-z])/g, (_, c) => c.toUpperCase()));
      return attrs.some(a => dataset[a] !== undefined) ? this : null;
    },
  };
  getEl(id).emit('click', { target });
}
function change(field, value, type = 'number') {
  getEl('inspector').emit('change', { target: { dataset: { field, lane: '0' }, type, value, tagName: 'INPUT' } });
}
function expect(value) { assert.deepEqual(events.pop(), { name: 'edit', value }); }

// Mute / Solo / Inspector toggles & MIDI preview banner actions
click('laneHeaders', { lane: '0', action: 'mute' }); expect({ action: 'setLane', lane: 0, field: 5, value: 1 });
click('laneHeaders', { lane: '0', action: 'solo' }); expect({ action: 'setLane', lane: 0, field: 6, value: 1 });
click('midiPreviewBar', { action: 'midiPreview' }); expect({ action: 'midiPreview' });
click('midiPreviewBar', { action: 'midiImport' }); expect({ action: 'midiImport' });
click('inspector', { action: 'retrySample' }); expect({ action: 'retrySample', lane: 0, path: '/samples/kick.wav' });
click('inspector', { playmode: 'gate' }); expect({ action: 'setSampler', lane: 0, field: 'playMode', value: 'gate' });
click('inspector', { reverse: '1' }); expect({ action: 'setSampler', lane: 0, field: 'reverse', value: true });
click('inspector', { filter: 'lp' }); expect({ action: 'setSampler', lane: 0, field: 'filterMode', value: 'lp' });
click('inspector', { action: 'midiDragLane' }); expect({ action: 'midiDragStart', lane: 0 });
click('inspector', { action: 'audioDragLane' }); expect({ action: 'audioDragStart', lane: 0, includeTail: false });

// Lane tools menu (via lane '...' button) covers Fill 3/4, Rotate, Invert, Clear, Duplicate, Delete
click('laneHeaders', { more: '0' });
const laneMenu = bodyChildren[bodyChildren.length - 1];
assert.ok(laneMenu && laneMenu.className.includes('menu'), 'lane tools menu opens');
const findMenuBtn = label => laneMenu.children.find(c => c.textContent === label);
findMenuBtn('Fill every 3').emit('click'); expect({ action: 'fillLane', lane: 0, every: 3 });

click('laneHeaders', { more: '0' });
bodyChildren[bodyChildren.length - 1].children.find(c => c.textContent === 'Rotate left').emit('click');
expect({ action: 'rotateLane', lane: 0, direction: -1 });

click('laneHeaders', { more: '0' });
bodyChildren[bodyChildren.length - 1].children.find(c => c.textContent === 'Duplicate lane').emit('click');
expect({ action: 'duplicateLane', lane: 0 });

// Groove popover covers Truncate and Seed reroll
getEl('grooveBtn').emit('click');
const groovePop = bodyChildren[bodyChildren.length - 1];
groovePop.emit('click', { target: { closest: sel => sel === '#truncateBtn' ? {} : null } });
expect({ action: 'setRack', field: 5, value: 1 });
groovePop.emit('click', { target: { closest: sel => sel === '#seedBtn' ? {} : null } });
expect({ action: 'rerollSeed' });

// WAV Tail + Drag WAV
getEl('wavTailBtn').emit('click');
getEl('dragWavBtn').emit('pointerdown', { button: 0 });
expect({ action: 'audioDragStart', lane: -1, includeTail: true });

// Sample browser toggle, preview, auto-swap
subscriptions.state({
  ...snapshot,
  browserFolders: [{ name: 'Drums', path: '/drums' }],
  browserListing: { path: '/drums', parent: '/', entries: [{ name: 'kick.wav', path: '/drums/kick.wav', isDir: false }] },
});
getEl('browserToggleBtn').emit('click');
assert.equal(getEl('browserPane').hidden, false);
getEl('browserList').emit('keydown', { key: 'ArrowDown', preventDefault() {} }); expect({ action: 'browserPreview', path: '/drums/kick.wav' });
getEl('browserAutoSwapBtn').emit('click');
getEl('browserList').emit('keydown', { key: 'ArrowUp', preventDefault() {} }); expect({ action: 'browserHotSwap', lane: 0, path: '/drums/kick.wav' });

// Standalone transport, fullscreen toggle + peak meter
subscriptions.state({ ...snapshot, standalone: true });
assert.equal(getEl('fullscreenBtn').hidden, false, 'fullscreen button visible in standalone');
getEl('fullscreenBtn').emit('click'); expect({ action: 'toggleFullscreen' });
for (const fn of docListeners.keydown) fn({ key: 'F11', preventDefault() {} });
expect({ action: 'toggleFullscreen' });
getEl('standalonePlayBtn').emit('click'); expect({ action: 'standaloneTransport', playing: false });
subscriptions.transport({ playing: true, bpm: 128, beat: 1.5, pattern: 0, pendingPattern: 1, playheads: [{ slot: 0, step: 6 }], meters: [{ slot: 0, peak: 0.75 }] });
assert.equal(getEl('meter-0').style.height, '75%');

// Inspector fields + bidirectional scroll sync + live waveform trim gesture
change('length', '32'); expect({ action: 'setLength', lane: 0, value: 32 });
change('name', 'Sub kick', 'text'); expect({ action: 'renameLane', lane: 0, name: 'Sub kick' });

getEl('laneHeaders').scrollTop = 88;
getEl('laneHeaders').emit('scroll');
assert.equal(getEl('gridScroll').scrollTop, 88, 'scrolling lane headers syncs gridScroll');

// Live logarithmic filter Cutoff knob drag emits start/end gestures
const filterSlot = getEl('filterKnobs');
const cutoffDial = filterSlot.children[0].children[0];
cutoffDial.emit('pointerdown', { clientY: 100, pointerId: 1, preventDefault() {} });
cutoffDial.emit('pointermove', { clientY: 60, shiftKey: false });
const startEvt = events.pop();
assert.equal(startEvt.value.action, 'setSampler');
assert.equal(startEvt.value.field, 'filterCutoff');
assert.equal(startEvt.value.gesture, 'start');
assert.ok(startEvt.value.value > 2000 && startEvt.value.value < 20000, 'log cutoff moves smoothly in mid-band');
cutoffDial.emit('pointerup', {});
const endEvt = events.pop();
assert.equal(endEvt.value.action, 'setSampler');
assert.equal(endEvt.value.gesture, 'end');

// Undo / Redo shortcuts
for (const fn of docListeners.keydown) fn({ ctrlKey: true, key: 'z', preventDefault() {} });
expect({ action: 'undo' });
for (const fn of docListeners.keydown) fn({ ctrlKey: true, shiftKey: true, key: 'z', preventDefault() {} });
expect({ action: 'redo' });

// State updates must not clobber focused inspector inputs
const before = getEl('inspector').innerHTML;
document.activeElement = { tagName: 'INPUT', parent: getEl('inspector') };
subscriptions.state({ ...snapshot, lanes: [{ ...lane, name: 'Changed' }] });
assert.equal(getEl('inspector').innerHTML, before, 'state updates must not clobber focused inspector inputs');
document.activeElement = null;
subscriptions.state({ ...snapshot, lanes: [{ ...lane, name: 'Changed' }] });
assert.match(getEl('inspector').innerHTML, /Changed/);

console.log('Graphite UI bridge behavior regressions passed');
