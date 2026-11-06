const assert = require('node:assert/strict');
const fs = require('node:fs');
const vm = require('node:vm');
const path = require('node:path');
const html = fs.readFileSync(path.join(__dirname, '../ui/app/index.html'), 'utf8');
const events = [], elements = {}, subscriptions = {};
function element(id) {
  return elements[id] ||= { id, innerHTML: '', value: '', dataset: {}, listeners: {},
    addEventListener(type, fn) { this.listeners[type] = fn; },
    contains(target) { return target?.parent === this; } };
}
const document = { activeElement: null, getElementById: element, querySelector: () => element('top') };
const window = { __JUCE__: { backend: {
  emitEvent: (name, value) => events.push({name, value: JSON.parse(JSON.stringify(value))}),
  addEventListener: (name, fn) => { subscriptions[name] = fn; }
} } };
vm.runInNewContext(html.match(/<script>([\s\S]*?)<\/script>/)[1], { window, document, prompt: () => 'Renamed', console });
assert.equal(typeof subscriptions.state, 'function', 'native state subscription');
const lane = {slot: 0, name: 'Drums', color: '#ffffff', length: 2, target: 'midi', note: 36, mute: false, solo: true,
  volume: 100, pan: 0, swing: 0, channel: 1, output: 1, start: 0, end: 1, tune: 0, attack: 0, decay: 0, release: 0,
  samplePath: '/samples/kick.wav', steps: [[0, 100], [1, 90]]};
let snapshot = {pattern: 0, patterns: ['Pattern 1'], lanes: [lane], midiPreview: {accepted: true, lanes: 1, lengthSteps: 4, diagnostics: [{message: '<warning>'}]}};
subscriptions.state(snapshot);
assert.match(element('rack').innerHTML, /Drums/);
assert.match(element('inspector').innerHTML, /&lt;warning&gt;/);
function click(id, dataset) {
  const target = {dataset, disabled: false, closest() { return this; }};
  element(id).listeners.click({target});
}
function change(field, value, type = 'number') {
  element('inspector').listeners.change({target: {dataset: {field, lane: '0'}, type, value}});
}
function expect(value) { assert.deepEqual(events.pop(), {name: 'edit', value}); }
click('rack', {lane: '0', step: '0'});
expect({action: 'setStep', lane: 0, step: 0, field: 0, value: 1});
click('rack', {lane: '0', action: 'mute'});
expect({action: 'setLane', lane: 0, field: 5, value: 1});
click('rack', {lane: '0', action: 'solo'});
expect({action: 'setLane', lane: 0, field: 6, value: 0});
click('rack', {lane: '0'});
expect({action: 'selectLane', lane: 0});
change('length', '32'); expect({action: 'setLength', lane: 0, value: 32});
change('pan', '-200'); expect({action: 'setLane', lane: 0, field: 8, value: -200});
change('attack', '0.2'); expect({action: 'setSampler', lane: 0, field: 'attack', value: 0.2});
change('target', 'sample', 'select-one'); expect({action: 'setSampler', lane: 0, field: 'target', value: 'sample'});
change('name', 'Kick', 'text'); expect({action: 'renameLane', lane: 0, name: 'Kick'});
click('inspector', {action: 'retrySample'}); expect({action: 'retrySample', lane: 0, path: '/samples/kick.wav'});
click('inspector', {action: 'midiPreview'}); expect({action: 'midiPreview'});
click('inspector', {action: 'midiImport'}); expect({action: 'midiImport'});
click('inspector', {action: 'clearLane'}); expect({action: 'clearLane', lane: 0});
element('pattern').listeners.change({target: {value: '2'}}); expect({action: 'pattern', value: 2});
click('top', {action: 'kitSave'}); expect({action: 'kitSave'});
const before = element('inspector').innerHTML;
document.activeElement = {tagName: 'INPUT', parent: element('inspector')};
subscriptions.state({...snapshot, lanes: [{...lane, name: 'Changed'}]});
assert.equal(element('inspector').innerHTML, before, 'polling must not destroy focused inputs');
document.activeElement = null;
subscriptions.state({...snapshot, lanes: [{...lane, name: 'Changed'}]});
assert.match(element('inspector').innerHTML, /Changed/);
subscriptions.state({...snapshot, midiPreview: null});
const count = events.length;
click('inspector', {action: 'midiImport'});
assert.equal(events.length, count, 'accept requires a compatible preview');
subscriptions.state({...snapshot, lanes: []});
assert.match(element('inspector').innerHTML, /Add lane/);
click('inspector', {action: 'addLane'}); expect({action: 'addLane', note: 60});
console.log('UI bridge behavior regressions passed');
