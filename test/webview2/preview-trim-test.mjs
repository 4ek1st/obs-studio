import test from "node:test";
import assert from "node:assert/strict";
import vm from "node:vm";
import {readFile} from "node:fs/promises";
import {clampPreviewTrims, previewTrimLimit} from "../../frontend/webview2/ui/preview-trim.mjs";

const app=await readFile(new URL("../../frontend/webview2/ui/app.js",import.meta.url),"utf8");
const syncCode=app.slice(app.indexOf("function previewTrimCapacity()"),app.indexOf("function savePreviewTrim()"));

test("side grips consume only blank preview width", () => {
  const limit=previewTrimLimit(2048, 940, 16/9, 1635, 10);
  assert.equal(limit, 390);
  assert.deepEqual(clampPreviewTrims(0, 200, limit, "right"), {left:0,right:200});
  assert.deepEqual(clampPreviewTrims(0, 600, limit, "right"), {left:0,right:limit});
  assert.deepEqual(clampPreviewTrims(170, 300, limit, "right"), {left:170,right:limit-170});
});

test("resizing or a zoomed canvas clamps both sides before the image scales", () => {
  assert.equal(previewTrimLimit(1500, 940, 16/9, 1635, 10), 0);
  const clamped=clampPreviewTrims(170, 200, 120);
  assert.equal(clamped.left+clamped.right, 120);
  assert.ok(clamped.left>0 && clamped.right>0);
  assert.deepEqual(clampPreviewTrims(40, 80, 0), {left:0,right:0});
});

test("a temporary narrow window does not replace the user's saved side widths", () => {
  const handles=new Map(),area={style:{setProperty(){}}};
  const context=vm.createContext({innerWidth:1600,viewportWidth:1600,panelMode:"",previewTrimLimit,clampPreviewTrims,
    state:{previewControls:{enabled:true,canvasWidth:1920,canvasHeight:1080,scale:.58,pixelRatio:1,hostWidth:1600}},
    scheduleBounds(){},document:{querySelector(){return area;}},
    $(id){
      if(id==="preview")return {get clientWidth(){return context.viewportWidth-context.trimState().applied.left-context.trimState().applied.right;},clientHeight:350};
      if(!handles.has(id))handles.set(id,{setAttribute(){}});
      return handles.get(id);
    }});
  vm.runInContext(`let previewTrim={left:100,right:128};let appliedPreviewTrim={left:0,right:0};
    ${syncCode}
    globalThis.trimState=()=>({requested:{...previewTrim},applied:{...appliedPreviewTrim}});`,context);
  const trim=()=>JSON.parse(JSON.stringify(context.trimState()));
  context.syncPreviewTrim();
  assert.deepEqual(trim(),{requested:{left:100,right:128},applied:{left:100,right:128}});
  context.innerWidth=context.viewportWidth=context.state.previewControls.hostWidth=900;
  context.syncPreviewTrim();
  assert.deepEqual(trim(),{requested:{left:100,right:128},applied:{left:0,right:0}});
  context.innerWidth=context.viewportWidth=context.state.previewControls.hostWidth=1600;
  context.syncPreviewTrim();
  assert.deepEqual(trim(),{requested:{left:100,right:128},applied:{left:100,right:128}});
});
