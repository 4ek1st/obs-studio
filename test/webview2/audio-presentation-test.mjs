import test from "node:test";
import assert from "node:assert/strict";
import { readFile } from "node:fs/promises";
import vm from "node:vm";

const app = await readFile(new URL("../../frontend/webview2/ui/app.js", import.meta.url), "utf8");
function element() {
  const properties = new Map();
  return { hidden: false, style: { setProperty(name, value) { properties.set(name, value); } }, properties };
}
function draw(state) {
  const bars = [0, 1].map(() => ({ peak: element(), hold: element(), magnitude: element(), input: element() }));
  const entry = { bars, meters: { querySelectorAll() { return bars.map(bar => bar.peak); } } };
  const source = app.slice(app.indexOf("function updateLevels("), app.indexOf("function renderTransitions("));
  vm.runInNewContext(source + '\nupdateLevels({fixture: levels});', {
    audioElements: new Map([["fixture", entry]]), rendersPanel: () => true, levels: state,
  });
  return bars;
}
test("the Web mixer draws distinct native peak, held peak, magnitude and input states", () => {
  const bars = draw({ minimum: -60, clipping: false, idle: false, channels: [
    { peak: -18, peakHold: -6, magnitude: -24, inputColor: "#abcdef" },
    { peak: -42, peakHold: -30, magnitude: -48, inputColor: "#123456" },
  ] });
  assert.equal(bars[0].peak.properties.get("--peak-empty"), "30%");
  assert.equal(bars[1].peak.properties.get("--peak-empty"), "70%");
  assert.equal(bars[0].hold.properties.get("--meter-position"), "90%");
  assert.equal(bars[0].magnitude.properties.get("--meter-position"), "60%");
  assert.equal(bars[1].input.properties.get("background-color"), "#123456");
});
test("native clipping holds the bar full and idle clears the input indicator", () => {
  const bars = draw({ minimum: -60, clipping: true, idle: true, channels: [
    { peak: -30, peakHold: -3, magnitude: -36, inputColor: "#ffffff" },
  ] });
  assert.equal(bars[0].peak.properties.get("--peak-empty"), "0%");
  assert.equal(bars[0].input.hidden, true);
});

test("bursty native meter messages draw only the newest level in one browser frame", () => {
  const bar = { peak: element(), hold: element(), magnitude: element(), input: element() };
  const frames = [];
  const source = app.slice(app.indexOf("function updateLevels("), app.indexOf("function renderTransitions("));
  vm.runInNewContext(source + `
    scheduleLevels({fixture:{minimum:-60,channels:[{peak:-18}]}});
    scheduleLevels({fixture:{minimum:-60,channels:[{peak:-42}]}});
  `, {
    audioElements: new Map([["fixture", { bars: [bar] }]]),
    rendersPanel: () => true,
    requestAnimationFrame: callback => { frames.push(callback); return frames.length; },
  });
  assert.equal(frames.length, 1);
  frames[0]();
  assert.equal(bar.peak.properties.get("--peak-empty"), "70%");
});

function faderInput(type, overrides = {}, focused = true) {
  const handlers = new Map(), calls = [];
  const volume = { disabled: false, addEventListener(name, callback) { handlers.set(name, callback); } };
  const start = app.indexOf("function installNativeFaderInput(");
  const source = app.slice(start, app.indexOf("function renderMixer(", start));
  vm.runInNewContext(source + '\ninstallNativeFaderInput(volume,"fixture",(command,args)=>calls.push({command,args}));', {
    document: { activeElement: focused ? volume : null }, volume, calls,
  });
  let prevented = false;
  handlers.get(type)({ key: "PageUp", deltaY: -100, deltaMode: 0, wheelDeltaY: 120,
    ctrlKey: false, shiftKey: false, altKey: false, metaKey: false,
    preventDefault() { prevented = true; }, ...overrides });
  return { calls: JSON.parse(JSON.stringify(calls)), prevented };
}
test("fader PageUp and native-suppressed Up use real Qt key handling, not browser steps", () => {
  for (const key of ["PageUp", "PageDown", "ArrowLeft", "ArrowUp", "Home", "End"])
    assert.deepEqual(faderInput("keydown", { key }).calls, [{ command: "audio.key", args: { uuid: "fixture", key, control: false, shift: false } }]);
  assert.equal(faderInput("keydown", { key: "z", ctrlKey: true }).prevented, false);
});
test("focused wheel preserves native wheel delta and modifiers; unfocused wheel scrolls", () => {
  assert.deepEqual(faderInput("wheel", { ctrlKey: true, shiftKey: true }).calls,
    [{ command: "audio.wheel", args: { uuid: "fixture", value: 120, control: true, shift: true } }]);
  assert.equal(faderInput("wheel", {}, false).prevented, false);
});
