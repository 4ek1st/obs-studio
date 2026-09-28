import test from "node:test";
import assert from "node:assert/strict";
import {readFile} from "node:fs/promises";
import vm from "node:vm";

const app=await readFile(new URL("../../frontend/webview2/ui/app.js",import.meta.url),"utf8");
const scheduler=app.slice(app.indexOf("function scheduleBounds()"),app.indexOf('$("original").addEventListener'));
function setup(){
  let width=1200;
  const frames=[],requests=[],settle=[];
  const context=vm.createContext({panelMode:"",boundsFrame:0,connected:true,boundsPending:false,boundsDirty:false,
    boundsSignature:"",boundsSignatures:new Map(),innerWidth:1200,innerHeight:800,state:{studioMode:true},
    document:{querySelectorAll:()=>[]},
    $:id=>({hidden:true,open:false,getBoundingClientRect:()=>({x:id==="program"?width/2+100:0,y:20,width:width/2-100,height:650})}),
    requestAnimationFrame:cb=>(frames.push(cb),frames.length),
    request:(command,args)=>{requests.push({command,args});return new Promise(resolve=>settle.push(resolve));}});
  vm.runInContext(scheduler,context);
  return {context,frames,requests,settle,resize(w){width=w;context.innerWidth=w;context.scheduleBounds();},
    tick(){const f=frames.shift();if(f)f();}};
}
test("preview and program resize in one frame without waiting between surfaces",()=>{
  const f=setup();f.resize(1200);f.tick();
  assert.equal(f.requests.length,1);
  assert.equal(f.requests[0].command,"preview.layout");
  assert.deepEqual(Array.from(f.requests[0].args.surfaces,x=>x.target),["preview","program"]);
});
test("resize backpressure keeps the latest dimensions instead of a queue of obsolete frames",async()=>{
  const f=setup();f.resize(1200);f.tick();
  for(let width=1190;width>=700;width-=10){f.resize(width);f.tick();}
  assert.equal(f.requests.length,1,"only one native request can be in flight");
  f.settle.shift()({});await new Promise(resolve=>setImmediate(resolve));f.tick();
  assert.equal(f.requests.length,2);
  assert.equal(f.requests[1].args.viewportWidth,700);
  assert.equal(f.requests[1].args.surfaces[0].width,250);
});
