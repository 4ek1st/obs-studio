import assert from "node:assert/strict";
import { readFile } from "node:fs/promises";
import vm from "node:vm";

// Exercise the production control's pointer/keyboard contract with the same
// non-1:1 CSS scale as WebView2. Visual placement is also tested in the Qt fixture.
class Element {
  style = { setProperty(name, value) { this[name] = value; } };
  dataset = {};
  attributes = {};
  classList = { add() {}, toggle() {} };
  children = [];
  listeners = {};
  clientHeight = 300;
  clientWidth = 14;
  setAttribute(name, value) { this.attributes[name] = String(value); }
  append(...children) { this.children.push(...children); }
  get firstElementChild() { return this.children[0]; }
  addEventListener(name, callback) { this.listeners[name] = callback; }
  getBoundingClientRect() { return { top: 50, left: 20, width: 7, height: 150 }; }
  focus() {}
  setPointerCapture() {}
}
const sandbox = {
  innerWidth: 640, innerHeight: 420,
  document: { querySelector: () => new Element(), createElement: () => new Element(),
    documentElement: new Element(), addEventListener() {} },
  addEventListener() {}, setTimeout, clearTimeout,
  installExternalDrop() {},
  createBridge: () => ({ subscribe() {}, request: () => Promise.resolve({}) }),
};
vm.createContext(sandbox);
vm.runInContext((await readFile(new URL("../../frontend/webview2/ui/dialog.js", import.meta.url), "utf8")).replace(/^import .*?;\s*/gm, ""), sandbox);
const values = []; let finishes = 0;
const bar = sandbox.createScrollbar(value => values.push(value), () => finishes++);
sandbox.updateScrollbar(bar, { minimum: 0, maximum: 600, page: 300, step: 10, value: 0 }, true);
assert.match(bar.firstElementChild.style.cssText, /top:0px;height:100px/,
  "at the top, one visible page of three gives a top-aligned one-third thumb");
const event = extra => ({ preventDefault() {}, stopPropagation() {}, button: 0, pointerId: 1, ...extra });
bar.listeners.keydown(event({ key: "PageDown" }));
assert.equal(values.at(-1), 300, "PageDown moves a native page, not ten slider steps");
bar.listeners.keydown(event({ key: "Home" }));
bar.listeners.pointerdown(event({ target: bar.firstElementChild, clientY: 60 }));
bar.listeners.pointermove(event({ clientY: 110 }));
assert.equal(values.at(-1), 300, "50 CSS pixels on a 50% scaled track moves half of the native range");
bar.listeners.pointerup(event({}));
bar.listeners.keydown(event({ key: "End" }));
assert.equal(values.at(-1), 600);
assert.match(bar.firstElementChild.style.cssText, /top:200px;height:100px/);
bar.listeners.keydown(event({ key: "ArrowDown" }));
assert.equal(values.at(-1), 600, "scrolling clamps at the end");
bar.listeners.pointerdown(event({ target: bar, clientY: 51 }));
assert.equal(values.at(-1), 300, "click above the thumb moves one page up");
assert.equal(bar.attributes["aria-valuenow"], "300");
assert.ok(finishes >= 4, "pointer and keyboard interactions finish their Qt transaction");
sandbox.updateScrollbar(bar, { minimum: 0, maximum: 600, page: 300, step: 10, value: 300, enabled: false }, true);
const count = values.length;
bar.listeners.keydown(event({ key: "Home" }));
assert.equal(values.length, count, "disabled scrollbar cannot modify native state");
const nativeStyle = { value: 0, grooveRect: { x: 0, y: 10, width: 14, height: 280 },
  thumbRect: { x: 1, y: 10, width: 12, height: 40 },
  normal: "data:image/png;base64,YQ==", hover: "data:image/png;base64,Yg==", pressed: "data:image/png;base64,Yw==" };
sandbox.updateScrollbar(bar, { minimum: 0, maximum: 600, page: 300, step: 10, value: 0, nativeStyle }, true);
assert.equal(bar.firstElementChild.style.height, "40px", "QSS handle minimum overrides the synthetic proportional size");
assert.equal(bar.firstElementChild.style.top, "10px", "native groove margins are retained");
assert.match(bar.style["--scrollbar-native"] || "", /YQ==/, "normal native QStyle image is displayed without replacing its colors");
bar.listeners.pointerdown(event({ target: bar.firstElementChild, clientY: 60 }));
bar.listeners.pointermove(event({ clientY: 120 }));
assert.equal(values.at(-1), 300, "native groove travel controls scaled drag mapping");
bar.listeners.pointerup(event({}));
sandbox.updateScrollbar(bar, { minimum: 0, maximum: 600, page: 300, step: 10, value: 0,
  nativeStyle: { ...nativeStyle, reversed: true, thumbRect: { x: 1, y: 250, width: 12, height: 40 } } }, true);
bar.listeners.pointerdown(event({ target: bar.firstElementChild, clientY: 185 }));
bar.listeners.pointermove(event({ clientY: 125 }));
assert.equal(values.at(-1), 300, "reversed native appearance preserves drag direction");
bar.listeners.pointerup(event({}));
console.log("PASS: proportional scrollbar, top/bottom, page keys, scaled pointer drag, track click and disabled state");
