import test from "node:test";
import assert from "node:assert/strict";
import vm from "node:vm";
import {readFile} from "node:fs/promises";

const source=await readFile(new URL("../../frontend/webview2/ui/app.js",import.meta.url),"utf8");
const production=source.slice(source.indexOf("function renderPreview("),source.indexOf("function render(next)"));
function setup(){
  const nodes=new Map();
  const make=()=>({dataset:{},style:{setProperty(name,value){this[name]=value;}},classList:{toggle(){}},children:[],firstElementChild:{style:{}},
    clientWidth:200,clientHeight:200,scrollWidth:400,scrollHeight:400,
    replaceChildren(...items){this.children=items;},matches(){return false;}});
  const $=id=>{if(!nodes.has(id))nodes.set(id,make());return nodes.get(id);};
  const context=vm.createContext({$,panelMode:"",innerWidth:800,innerHeight:600,clean:value=>value.replace(/&/g,""),
    document:{createElement:make},actionEntry:()=>({enabled:true})});
  vm.runInContext(production,context);
  return {$,render:value=>context.renderPreview(value)};
}
test("preview scale uses current native choices, resolutions and manual placeholder",()=>{
  const {$,render}=setup();
  render({previewControls:{enabled:true,index:1,percent:"100%",options:[{index:0,text:"Fit"},{index:1,text:"Canvas 1920x1080"}]}});
  assert.equal($("preview-scaling").children.length,2);
  assert.equal($("preview-scaling").children[1].textContent,"Canvas 1920x1080");
  assert.equal($("preview-scaling").value,"1");
  render({previewControls:{enabled:true,index:-1,placeholder:"Manual 1200x675",options:[{index:0,text:"Fit"}]}});
  assert.equal($("preview-scaling").children[1].disabled,true);
  assert.equal($("preview-scaling").value,"-1");
});
test("native disabled state exposes Enable Preview and removes scrollbar hit targets",()=>{
  const {$,render}=setup();
  render({previewControls:{enabled:false,enableText:"&Enable Preview"}});
  assert.equal($("preview-disabled").hidden,false);
  assert.equal($("enable-preview").textContent,"Enable Preview");
  assert.equal($("preview-scroll-x").hidden,true);
  render({previewControls:{enabled:true}});
  assert.equal($("preview-disabled").hidden,true);
});
test("native canvas ranges map proportionally to real scroll offsets",()=>{
  const {$,render}=setup();
  render({previewControls:{enabled:true,previewXScrollBar:{min:-100,max:100,value:50,page:200},
    previewYScrollBar:{min:-100,max:100,value:-50,page:200}}});
  assert.equal($("preview-scroll-x").firstElementChild.style.width,"400px");
  assert.equal($("preview-scroll-x").scrollLeft,150);
  assert.equal($("preview-scroll-y").scrollTop,50);
});
test("scrollbar thickness uses the native widget size and current CSS viewport scale",()=>{
  const {$,render}=setup();
  render({previewControls:{enabled:true,hostWidth:1000,hostHeight:750,
    previewXScrollBar:{extent:20,min:0,max:0,value:0,page:200},
    previewYScrollBar:{extent:25,min:0,max:0,value:0,page:200}}});
  assert.equal($("preview-grid").style["--preview-scroll-x"],"16px");
  assert.equal($("preview-grid").style["--preview-scroll-y"],"20px");
  assert.equal($("preview-scroll-x").hidden,false);
  assert.equal($("preview-scroll-y").hidden,false);
});
