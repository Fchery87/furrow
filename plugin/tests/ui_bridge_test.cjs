const assert = require('node:assert/strict');
const fs = require('node:fs');
const vm = require('node:vm');
const path = require('node:path');

const html = fs.readFileSync(path.join(__dirname, '../ui/app/index.html'), 'utf8');
assert.doesNotMatch(html.replaceAll('http://www.w3.org/2000/svg', ''), /https?:\/\//i, 'shipping UI must contain zero external network URLs');
assert.doesNotMatch(html, /\bprompt\s*\(/, 'shipping UI must not call window.prompt');

const events = [], elements = {}, subscriptions = {}, docListeners = {};
function makeElement(id) {
  return {
    id, innerHTML: '', textContent: '', value: '', className: '', hidden: false, disabled: false,
    dataset: {}, style: {}, listeners: {},
    addEventListener(type, fn) { this.listeners[type] = fn; },
    setAttribute() {}, getAttribute() {}, appendChild() {}, append() {}, prepend() {}, remove() {},
    focus() {}, select() {}, contains(t) { return t?.parent === this; },
    getBoundingClientRect() { return { left: 0, top: 0, bottom: 40, width: 600, height: 128 }; },
    getContext() {
      return {
        setTransform() {}, fillRect() {}, strokeRect() {}, beginPath() {}, moveTo() {}, lineTo() {}, stroke() {},
        fillStyle: '', strokeStyle: '', lineWidth: 1, globalAlpha: 1,
      };
    },
  };
}
function getEl(id) { return elements[id] ||= makeElement(id); }
const document = {
  activeElement: null,
  body: makeElement('body'),
  getElementById: getEl,
  querySelector: () => getEl('topbar'),
  createElement: tag => makeElement(tag),
  addEventListener(type, fn) { docListeners[type] = fn; },
};
const window = {
  devicePixelRatio: 1, innerWidth: 1100, innerHeight: 700,
  __JUCE__: {
    backend: {
      emitEvent: (name, value) => events.push({ name, value: JSON.parse(JSON.stringify(value)) }),
      addEventListener: (name, fn) => { subscriptions[name] = fn; },
    },
  },
};

vm.runInNewContext(html.match(/<script>([\s\S]*?)<\/script>/)[1], { window, document, console });
assert.equal(typeof subscriptions.state, 'function', 'native state subscription');
assert.equal(typeof subscriptions.transport, 'function', 'native transport subscription');
assert.deepEqual(events.shift(), { name: 'edit', value: { action: 'ready' } }, 'ready handshake on boot');

const lane = {
  slot: 0, name: 'Kick', color: '#E2B24A', length: 16, target: 'sample', note: 36, mute: false, solo: false,
  volume: 5000, pan: 0, bendRange: 2, swing: 10000, modACc: 74, modBCc: 71, channel: 1, output: 1,
  start: 0, end: 1, tune: 0, attack: 1, decay: 0, release: 1, choke: 0,
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
assert.match(getEl('inspector').innerHTML, /&lt;ok&gt;/);
assert.match(getEl('stepTabs').innerHTML, /Release/);
assert.match(getEl('stepTabs').innerHTML, /Probability/);

subscriptions.transport({ playing: true, bpm: 128, beat: 1.5, pattern: 0, pendingPattern: 1, playheads: [{ slot: 0, step: 6 }] });
assert.equal(getEl('bpmText').textContent, '128 BPM');
assert.match(getEl('queuedBadge').textContent, /Fill/);

function click(id, dataset) {
  const target = { dataset, disabled: false, closest() { return this; } };
  getEl(id).listeners.click({ target });
}
function change(field, value, type = 'number') {
  getEl('inspector').listeners.change({ target: { dataset: { field, lane: '0' }, type, value, tagName: 'INPUT' } });
}
function expect(value) { assert.deepEqual(events.pop(), { name: 'edit', value }); }

click('laneHeaders', { lane: '0', action: 'mute' }); expect({ action: 'setLane', lane: 0, field: 5, value: 1 });
click('laneHeaders', { lane: '0', action: 'solo' }); expect({ action: 'setLane', lane: 0, field: 6, value: 1 });
click('inspector', { fill: '4' }); expect({ action: 'fillLane', lane: 0, every: 4 });
click('inspector', { rotate: '-1' }); expect({ action: 'rotateLane', lane: 0, direction: -1 });
click('inspector', { action: 'invertLane' }); expect({ action: 'invertLane', lane: 0 });
click('inspector', { action: 'clearLane' }); expect({ action: 'clearLane', lane: 0 });
click('inspector', { action: 'removeLane' }); expect({ action: 'removeLane', lane: 0 });
click('inspector', { action: 'retrySample' }); expect({ action: 'retrySample', lane: 0, path: '/samples/kick.wav' });
click('inspector', { action: 'midiPreview' }); expect({ action: 'midiPreview' });
click('inspector', { action: 'midiImport' }); expect({ action: 'midiImport' });
change('length', '32'); expect({ action: 'setLength', lane: 0, value: 32 });
change('name', 'Sub kick', 'text'); expect({ action: 'renameLane', lane: 0, name: 'Sub kick' });
getEl('truncateBtn').listeners.click(); expect({ action: 'setRack', field: 5, value: 1 });
getEl('seedBtn').listeners.click(); expect({ action: 'rerollSeed' });
docListeners.keydown({ ctrlKey: true, key: 'z', preventDefault() {} }); expect({ action: 'undo' });
docListeners.keydown({ ctrlKey: true, shiftKey: true, key: 'z', preventDefault() {} }); expect({ action: 'redo' });

const before = getEl('inspector').innerHTML;
document.activeElement = { tagName: 'INPUT', parent: getEl('inspector') };
subscriptions.state({ ...snapshot, lanes: [{ ...lane, name: 'Changed' }] });
assert.equal(getEl('inspector').innerHTML, before, 'state updates must not clobber focused inspector inputs');
document.activeElement = null;
subscriptions.state({ ...snapshot, lanes: [{ ...lane, name: 'Changed' }] });
assert.match(getEl('inspector').innerHTML, /Changed/);

console.log('Wave 2 UI bridge behavior regressions passed');
