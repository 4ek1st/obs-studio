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
  return { calls, slider, fire(name, data = {}) {
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
test("a pixel-sized T-bar handle maps back to the parked native slider hit area",()=>{
  const f=fixture();
  f.slider._nativeGeometry={first:.012,last:.988,height:500};
  f.slider._visualGeometry={thumbWidth:24,left:0,right:0,scale:1};
  f.fire("pointerdown",{clientX:22});
  assert.ok(Math.abs(f.calls[0].args.x-.012)<1e-10);
  f.fire("pointermove",{clientX:110});
  assert.ok(Math.abs(f.calls[1].args.x-.5)<1e-10);
});
test("T-bar presentation uses intrinsic Qt sizes instead of stretching the parked widget",()=>{
  const start=app.indexOf("function applyTBarGeometry(");
  const code=app.slice(start,app.indexOf("function installNativeTBarInput(",start));
  const slider={style:{setProperty(k,v){this[k]=v;}},getBoundingClientRect:()=>({width:200})};
  const geometry={width:1000,height:500,preferredHeight:40,thumbWidth:.024,thumbHeight:.08,
    thumbWidthPixels:24,thumbHeightPixels:40,first:.012,last:.988,minimum:0,maximum:1023,devicePixelRatio:1};
  vm.runInNewContext(code+"\napplyTBarGeometry(slider,geometry);",{slider,geometry,window:{devicePixelRatio:1}});
  assert.equal(slider.style.height,"40px");
  assert.equal(slider.style["--tbar-thumb-width"],"24px");
  assert.equal(slider.style["--tbar-thumb-height"],"40px");
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
