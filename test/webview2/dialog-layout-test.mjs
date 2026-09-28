import assert from "node:assert/strict";
import { readFile } from "node:fs/promises";
import vm from "node:vm";

// Exercise the production renderer at the observed Qt/WebView viewport mismatch.
class Element {
  style = { setProperty(name, value) { this[name] = value; } };
  dataset = {};
  classList = { add() {}, toggle() {} };
  children = [];
  setAttribute() {}
  append(child) { this.children.push(child); }
  querySelector() { return null; }
  matches() { return false; }
  remove() {}
}
const root = new Element(), error = new Element();
const events = new Map(), requests = [];
let render;
const sandbox = {
  innerWidth: 1066.6666666667, innerHeight: 700,
  document: {
    documentElement: new Element(), activeElement: null,
    querySelector: selector => selector === "#dialog" ? root : error,
    createElement: () => new Element(),
    addEventListener: (name, callback) => events.set(name, callback),
  },
  addEventListener: (name, callback) => events.set(name, callback),
  createBridge: () => ({ subscribe: (_name, callback) => { render = callback; }, request: (name, args) => {
    requests.push({ name, args }); return Promise.resolve({});
  } }),
  setTimeout, clearTimeout,
};
sandbox.window = sandbox;
vm.runInNewContext((await readFile(new URL("../../frontend/webview2/ui/dialog.js", import.meta.url), "utf8")).replace(/^import .*?;\s*/, ""), sandbox);
render({ width: 1280, height: 840, nodes: [{ id: "scroll", type: "scrollArea", enabled: true,
  rect: { x: 960, y: 500, width: 200, height: 300 }, clip: { x: 960, y: 500, width: 200, height: 300 } }] });
assert.equal(root.style.width, "1280px");
assert.equal(root.style.height, "840px");
const values = root.style.transform.match(/[\d.]+/g).map(Number);
assert.ok(Math.abs(values[0] - 5 / 6) < 1e-6 && Math.abs(values[1] - 5 / 6) < 1e-6, "Qt coordinates scale to CSS viewport");
let prevented = false;
events.get("wheel")({ clientX: 850, clientY: 500, deltaX: 0, deltaY: 120, deltaMode: 0,
  preventDefault() { prevented = true; } });
assert.ok(prevented && requests.at(-1).name === "dialog.wheel" && requests.at(-1).args.id === "scroll", "wheel hit test uses inverse viewport scale");
sandbox.innerWidth = 640; sandbox.innerHeight = 420;
events.get("resize")();
assert.equal(root.style.transform, "scale(0.5, 0.5)", "resize reapplies geometry without awaiting native polling");
render({ width: 1280, height: 840, nodes: [{ id: "panel", type: "panel", enabled: true,
  background: "#252833", frameWidth: 12, decoration: "data:image/png;base64,YQ==",
  rect: { x: 0, y: 0, width: 200, height: 100 }, clip: { x: 0, y: 0, width: 200, height: 100 } }] });
const panel = root.children.at(-1);
assert.equal(panel.style.border, "none", "QSS layout insets do not become a solid CSS border");
assert.match(panel.style.backgroundImage || "", /YQ==/, "native panel decoration supplies the actual thin border and rounded corners");
render({ width: 1280, height: 840, nodes: [{ id: "panel", type: "panel", enabled: true,
  decoration: "data:image/png;base64,YQ==", decorationRect: { x: 40, y: 140, width: 400, height: 220 },
  rect: { x: -40, y: -140, width: 1200, height: 3600 }, clip: { x: 0, y: 0, width: 400, height: 220 } }] });
assert.equal(panel.style.backgroundPosition, "40px 140px", "clipped decoration keeps its original widget-local origin");
assert.equal(panel.style.backgroundSize, "400px 220px", "visible pixels are not stretched across the full scroll page");
console.log("PASS: dialog viewport scaling, inverse wheel targeting, and resize");
