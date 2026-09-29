import test from "node:test";
import assert from "node:assert/strict";
import { readFile } from "node:fs/promises";
import { execFileSync } from "node:child_process";
import vm from "node:vm";

const app = process.env.WEBVIEW_WORKSPACE_BASELINE
  ? execFileSync("git", ["show", "57e27eeb:frontend/webview2/ui/app.js"], { cwd:new URL("../..",import.meta.url), encoding:"utf8" })
  : await readFile(new URL("../../frontend/webview2/ui/app.js",import.meta.url),"utf8");
const css = await readFile(new URL("../../frontend/webview2/ui/style.css",import.meta.url),"utf8");

// Execute the actual production list renderer and its registered callbacks.
// This small DOM double records requests; it has no replacement key/drop logic.
function fixture(sources=true) {
  const calls=[],elements=new Map();
  const document={activeElement:null,createElement:tag=>new Element(tag),createTextNode:text=>({textContent:text})};
  class Element {
    constructor(tag="div") { this.tagName=tag.toUpperCase();this.children=[];this.dataset={};this.listeners={};this.className="";this.style={setProperty(name,value){this[name]=value;}};this.scrollTop=0;this.clientHeight=100;this.clientWidth=300;this.rect={top:0,left:0,width:300,height:20};this.classList={add(){},remove(){},toggle(){}}; }
    append(...nodes){for(const node of nodes){node.parentElement=this;this.children.push(node);}}
    replaceChildren(...nodes){this.children=[];this.append(...nodes);}
    set textContent(text){this.text=text;this.children=[];} get textContent(){return this.text??"";}
    setAttribute(name,value){this[name]=value;}
    addEventListener(name,callback){this.listeners[name]=callback;}
    matches(selector){return selector.startsWith(".")?this.className.split(" ").includes(selector.slice(1)):this.tagName.toLowerCase()===selector;}
    closest(selector){return selector.split(",").some(s=>this.matches(s.trim()))?this:this.parentElement?.closest(selector)??null;}
    querySelector(selector){for(const child of this.children){if(child.matches?.(selector))return child;const nested=child.querySelector?.(selector);if(nested)return nested;}return null;}
    contains(node){return node===this||this.children.some(child=>child.contains?.(node));}
    focus(){document.activeElement=this;} select(){}
    getBoundingClientRect(){return this.rect;}
    get previousElementSibling(){const a=this.parentElement.children;return a[a.indexOf(this)-1];}
    get nextElementSibling(){const a=this.parentElement.children;return a[a.indexOf(this)+1];}
    click(){return this.listeners.click?.(event(this));}
    fire(name,values={}){return this.listeners[name]?.(event(this,values));}
  }
  function event(target,values={}){return {target,key:"",code:"",ctrlKey:false,metaKey:false,shiftKey:false,preventDefault(){},stopPropagation(){},...values};}
  const sourceRows=[{id:"1",uuid:"a",owner:"s",name:"A",selected:true},{id:"2",uuid:"b",owner:"s",name:"B",group:true,collapsed:true},{id:"3",uuid:"c",owner:"s",name:"C"}];
  const scenes=[{uuid:"s",name:"Scene A"},{uuid:"s2",name:"Scene B"},{uuid:"s3",name:"Scene C"}];
  const state={currentScene:"s",sources:sourceRows,scenes,workspace:{}};
  for(const id of ["sources","scenes","rename-dialog","rename-title","rename-value"])elements.set(id,new Element());
  const button=(_text,callback)=>{const node=new Element("button");node.addEventListener("click",callback);return node;};
  const context={document,state,dragged:null,renameTarget:null,tr:(_key,fallback)=>fallback,keyOf:row=>[row.owner??"",row.id,row.uuid].join("/"),$:(id)=>elements.get(id),
    sourceArgs:row=>({...row,scene:state.currentScene}),request:async(command,args)=>{calls.push([command,args]);return {};},closeMenus(){},scheduleBounds(){},
    icon:()=>new Element("svg"),iconButton:(_symbol,text,callback)=>button(text,callback),button,actionsByName:new Map(),
    invokeName:name=>calls.push(["invoke",name]),showContext:()=>calls.push(["custom.menu"])};
  vm.createContext(context);
  const start=app.includes("function listDropPosition")?app.indexOf("function listDropPosition"):app.indexOf("async function selectSource");
  vm.runInContext(app.slice(start,app.indexOf("function renderMixer")),context);
  const list=elements.get(sources?"sources":"scenes"),rows=sources?sourceRows:scenes;
  context.renderRows(list,rows,sources);
  list.children.forEach((node,i)=>node.rect={left:0,top:i*20,width:300,height:20});
  return {calls,list,rows,state,context,document,event};
}
test("Shift and Ctrl+Shift row navigation preserve range/additive modifiers",async()=>{
  const f=fixture();await f.list.children[0].fire("keydown",{key:"ArrowDown",shiftKey:true,ctrlKey:true});
  assert.equal(f.calls[0][0],"source.select");assert.equal(f.calls[0][1].range,true);assert.equal(f.calls[0][1].additive,true);
});
test("Ctrl navigation moves native focus without replacing source selection",async()=>{
  const f=fixture();await f.list.children[0].fire("keydown",{key:"ArrowDown",ctrlKey:true});assert.equal(f.calls[0][1].focusOnly,true);
});
test("Ctrl+A uses the native source selection endpoint",async()=>{
  const f=fixture();await f.list.children[0].fire("keydown",{key:"ф",code:"KeyA",ctrlKey:true});assert.equal(f.calls[0][0],"source.selectAll");
});
test("group double click expands instead of opening source properties",async()=>{
  const f=fixture();await f.list.children[1].fire("dblclick");assert.equal(f.calls[0][0],"source.expand");assert.equal(f.calls[0][1].value,true);
});
test("source drop reads row coordinates even when the target is a child icon",async()=>{
  const f=fixture();f.context.dragged={row:f.rows[0],sources:true,scene:"s"};
  await f.list.children[2].fire("drop",{clientY:58,clientX:10,offsetY:0,target:f.list.children[2].children[0]});
  assert.equal(f.calls[0][1].position,"after");
});
test("dropping on the center of a group uses native OnItem without Alt",async()=>{
  const f=fixture();f.context.dragged={row:f.rows[0],sources:true,scene:"s"};await f.list.children[1].fire("drop",{clientY:30,clientX:20,offsetY:10});assert.equal(f.calls[0][1].position,"inside");
});
test("scene drag keeps explicit before/after insertion intent",async()=>{
  const f=fixture(false);f.context.dragged={row:f.rows[0],sources:false,scene:"s"};await f.list.children[2].fire("drop",{clientY:42,clientX:20,offsetY:2});assert.equal(f.calls.at(-1)[1].position,"before");
});
test("source context invokes the complete original native menu",async()=>{
  const f=fixture();await f.list.children[0].fire("contextmenu");assert.equal(f.calls[0][0],"native.command");assert.equal(f.calls[0][1].id,"source.context");
});
test("scene double click reaches the original Studio Mode controller",async()=>{
  const f=fixture(false);await f.list.children[0].fire("dblclick");assert.equal(f.calls[0][0],"scene.activate");
});
test("source pointer enter and leave reach the native preview hover path",async()=>{
  const f=fixture();await f.list.children[0].fire("mouseenter");await f.list.children[0].fire("mouseleave");assert.deepEqual(f.calls.map(call=>[call[0],call[1].value]),[["source.hover",true],["source.hover",false]]);
});
test("F2 creates an inline editor whose Enter invokes native rename",async()=>{
  const f=fixture();await f.list.children[0].fire("keydown",{key:"F2"});const editor=f.list.querySelector(".row-name-editor");assert.ok(editor);editor.value="Renamed";await editor.fire("keydown",{key:"Enter"});assert.equal(f.calls[0][0],"source.rename");assert.equal(f.calls[0][1].name,"Renamed");
});
test("unchanged Enter and Escape explicitly finish the original native name transaction",async()=>{
  for(const [key,save] of [["Enter",true],["Escape",false]]){
    const f=fixture();await f.list.children[0].fire("keydown",{key:"F2"});
    const editor=f.list.querySelector(".row-name-editor");assert.ok(editor);
    await editor.fire("keydown",{key});
    assert.equal(f.calls[0][0],"source.rename");assert.equal(f.calls[0][1].save,save);
    assert.equal(f.calls[0][1].name,f.rows[0].name);
  }
});
test("a scene switch cancels inline rename without mutating the new scene",async()=>{
  const f=fixture();await f.list.children[0].fire("keydown",{key:"F2"});const editor=f.list.querySelector(".row-name-editor");assert.ok(editor);editor.value="Old scene edit";f.state.currentScene="other";f.context.renderRows(f.list,f.rows,true);assert.equal(f.list.querySelector(".row-name-editor"),null);assert.equal(f.calls.length,0);
});
test("native lifecycle completion closes the HTML editor without a second rename",async()=>{
  const f=fixture();await f.list.children[0].fire("keydown",{key:"F2"});
  f.list.querySelector(".row-name-editor").value="Uncommitted text";
  f.context.finishNativeRename();
  assert.equal(f.list.querySelector(".row-name-editor"),null);assert.equal(f.calls.length,0);
});
test("native source context keeps item identity separate from command identity",async()=>{
  const f=fixture();await f.list.children[0].fire("contextmenu");assert.equal(f.calls[0][1].id,"source.context");assert.equal(f.calls[0][1].item,"1");
});
test("grid arrows follow rendered columns and Home/End reach boundaries",async()=>{
  const f=fixture(false);f.state.workspace.actionSceneGridMode=true;f.list.children.forEach((entry,i)=>entry.rect={left:(i%2)*100,top:Math.floor(i/2)*24,width:100,height:24});
  await f.list.children[0].fire("keydown",{key:"ArrowDown"});assert.equal(f.calls[0][1].uuid,"s3");
  await f.list.children[2].fire("keydown",{key:"Home"});assert.equal(f.calls[1][1].uuid,"s");
});

test("source color is an alpha overlay and does not replace selected or hovered row backgrounds",()=>{
  const f=fixture();
  f.rows[0].color="#ff444454";
  f.context.renderRows(f.list,f.rows,true);
  const row=f.list.children[0];
  assert.equal(row["aria-selected"],"true");
  assert.equal(row.style.backgroundColor,undefined,"native selection owns the base background");
  assert.equal(row.style["--source-color"],"#ff444454","retain the native preset's 33 percent alpha");
  assert.match(css,/#sources\s*>\s*\.row\s*\{[^}]*background-image:\s*linear-gradient\(var\(--source-color,\s*transparent\),\s*var\(--source-color,\s*transparent\)\)/);
  f.rows[0].selected=false;
  f.context.renderRows(f.list,f.rows,true);
  assert.equal(f.list.children[0]["aria-selected"],"false");
  assert.equal(f.list.children[0].style["--source-color"],"#ff444454");
});
