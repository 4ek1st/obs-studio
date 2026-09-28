import test from "node:test";
import assert from "node:assert/strict";
import { readFile } from "node:fs/promises";
import vm from "node:vm";

const app = await readFile(new URL("../../frontend/webview2/ui/app.js", import.meta.url), "utf8");
function fixture() {
  const handlers = new Map(), calls = [];
  const slider = { disabled: false, value: 0, addEventListener(name, callback) { handlers.set(name, callback); },
    getBoundingClientRect: () => ({ left: 10, top: 10, width: 200, height: 30 }),
    focus() {}, blur() {}, setPointerCapture() {}, hasPointerCapture: () => false, releasePointerCapture() {} };
  const start = app.indexOf("function installNativeTBarInput(");
  const source = app.slice(start, app.indexOf("function renderControls(", start));
  vm.runInNewContext(source + '\ninstallNativeTBarInput(slider,(command,args)=>calls.push({command,args}));', {
    slider, calls, document: { activeElement: slider },
  });
  return { calls, fire(name, data = {}) {
    let prevented = false;
    handlers.get(name)({ clientX: 160, clientY: 25, button: 0, pointerId: 7,
      key: "PageUp", deltaY: 100, wheelDeltaY: -120, ctrlKey: false, shiftKey: false,
      preventDefault() { prevented = true; }, ...data });
    return prevented;
  } };
}
test("T-bar track press reaches native hit testing and never sets an absolute value", () => {
  const f = fixture();
  assert.equal(f.fire("pointerdown"), true);
  assert.equal(f.calls[0].command, "studio.tbar.input");
  assert.equal(f.calls[0].args.kind, "press");
  assert.equal(f.calls[0].args.x, 0.75);
  assert.equal("value" in f.calls[0].args, false);
});
test("T-bar cancellation releases exactly one native gesture", () => {
  const f = fixture(); f.fire("pointerdown"); f.fire("pointermove"); f.fire("pointercancel"); f.fire("lostpointercapture");
  assert.deepEqual(f.calls.map(call => call.args.kind), ["press", "move", "release"]);
});
test("T-bar keyboard and wheel use Qt without synthesizing sliderReleased", () => {
  const f = fixture(); f.fire("keydown"); f.fire("wheel");
  assert.deepEqual(f.calls.map(call => call.args.kind), ["key", "wheel"]);
  assert.equal(f.calls[0].args.key, "PageUp");
  assert.equal(f.calls[1].args.value, -120);
});
