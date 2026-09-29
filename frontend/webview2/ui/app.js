import { createBridge, createPresentation } from "./bridge.mjs";
import { installExternalDrop } from "./external-drop.mjs";

const $ = id => document.getElementById(id);
const clean = text => String(text ?? "").replace(/&&/g, "\u0000").replace(/&/g, "").replace(/\u0000/g, "&");
const keyOf = row => [row.owner ?? "", row.id, row.uuid].join("/");
let bridge, state, renameTarget, dragged;
let present;
let menuSignature = "", boundsFrame = 0, connected = false;
let boundsPending = false, boundsDirty = false, boundsSignature = "";
const audioElements = new Map(), actionsByName = new Map();
const defaultPanelOrder = ["scenesDock","sourcesDock","mixerDock","transitionsDock","controlsDock"];
let panelMode = "";
const nativeCommands = new Set(["action.invoke", "control.click", "native.command", "source.visibility", "source.lock", "source.expand", "source.rename", "source.move", "source.selectAll", "scene.rename", "scene.move", "scene.activate"]);
const iconPaths = {
  plus:"M8 2v12M2 8h12", trash:"M3 4h10M6 2h4M4 4l1 10h6l1-10M7 6v6M9 6v6",
  up:"M3 10l5-5 5 5", down:"M3 6l5 5 5-5", gear:"M6 2h4l.5 2 2 .5 1 3-1 3-2 .5-.5 2H6l-.5-2-2-.5-1-3 1-3 2-.5zM10 8a2 2 0 1 1-4 0 2 2 0 0 1 4 0",
  eye:"M1 8s2.5-4 7-4 7 4 7 4-2.5 4-7 4-7-4-7-4M10 8a2 2 0 1 1-4 0 2 2 0 0 1 4 0",
  lock:"M4 7h8v7H4zM5 7V5a3 3 0 0 1 6 0v2", volume:"M2 6h3l4-3v10l-4-3H2zM11 5c3 1 3 5 0 6",
  muted:"M2 6h3l4-3v10l-4-3H2zM11 6l4 4M15 6l-4 4", filter:"M2 3h12L9 8v5l-2 1V8z",
  folder:"M1 4h5l2 2h7v7H1zM1 4V2h5l2 2h6v2", image:"M2 2h12v12H2zM3 11l3-4 3 3 2-2 3 4M11 5h.1",
  copy:"M6 5h8v9H6zM2 11V2h8", dots:"M8 3h.1M8 8h.1M8 13h.1", monitor:"M2 2h12v9H2zM5 14h6M8 11v3",
  headphones:"M2 9V7a6 6 0 0 1 12 0v2M2 8h3v6H2zM11 8h3v6h-3z", layoutVertical:"M2 2h4v12H2zM10 2h4v12h-4z", layoutHorizontal:"M2 2h12v4H2zM2 10h12v4H2z"
};
function icon(name) {
  const svg = document.createElementNS("http://www.w3.org/2000/svg", "svg");
  svg.setAttribute("viewBox", "0 0 16 16"); svg.setAttribute("aria-hidden", "true");
  const path = document.createElementNS(svg.namespaceURI, "path");
  path.setAttribute("d", iconPaths[name] || iconPaths.gear); path.setAttribute("fill", "none");
  path.setAttribute("stroke", "currentColor"); path.setAttribute("stroke-width", "1.4");
  path.setAttribute("stroke-linecap", "round"); path.setAttribute("stroke-linejoin", "round");
  svg.append(path); return svg;
}
function button(text, callback, enabled=true) {
  const element = document.createElement("button"); element.type="button"; element.textContent=clean(text);
  element.disabled=!enabled; element.addEventListener("click", callback); return element;
}
function iconButton(name, title, callback, enabled=true) {
  const element=button("",callback,enabled); element.append(icon(name));
  element.title=clean(title); element.setAttribute("aria-label", clean(title)); return element;
}
function showError(error) { $("error").querySelector("span").textContent=error.message||String(error); $("error").hidden=false; scheduleBounds(); }
async function request(command,args={}) {
  try {
    if(nativeCommands.has(command)) args={...args,context:state?.context};
    const result=await bridge.request(command,args); $("error").hidden=true; return result;
  } catch(error) { showError(error); return undefined; }
}
function sourceArgs(source) { return {scene:state.currentScene,id:source.id,uuid:source.uuid,owner:source.owner}; }
function invokeName(name) {
  const action=actionsByName.get(name); if(action) return request("action.invoke",{id:action.id});
}
function closeMenus() {
  document.querySelectorAll("details[open]").forEach(menu=>menu.open=false);
  $("context-menu").hidden=true; scheduleBounds();
}
function indexActions(items) {
  for(const item of items||[]) { if(item.name) actionsByName.set(item.name,item); if(item.children) indexActions(item.children); }
}
function prepareMenu(menu,content,id) {
  let generation=0;
  menu.addEventListener("toggle",async()=>{
    const current=++generation; if(!menu.open)return;
    const entries=await request("menu.prepare",{id});
    if(current!==generation||!menu.open||!Array.isArray(entries))return;
    indexActions(entries); content.replaceChildren(...menuItems(entries).childNodes); scheduleBounds();
  });
}
function menuItems(items) {
  const content=document.createElement("div");
  for(const item of items||[]) {
    if(item.separator){content.append(document.createElement("hr"));continue;}
    if(item.children) {
      const nested=document.createElement("details"); nested.className="submenu";
      if(item.enabled===false)nested.setAttribute("aria-disabled","true");
      const title=document.createElement("summary");title.textContent=clean(item.text);
      const children=menuItems(item.children);nested.append(title,children);
      if(item.id)prepareMenu(nested,children,item.id);content.append(nested);continue;
    }
    const entry=button("",()=>{closeMenus(); if(item.callback)item.callback();else request("action.invoke",{id:item.id});},item.enabled!==false);
    const name=document.createElement("span");name.textContent=(item.checked?"✓ ":"")+clean(item.text);
    const shortcut=document.createElement("span");shortcut.className="shortcut";shortcut.textContent=item.shortcut||"";
    entry.setAttribute("aria-label",clean(item.text));entry.append(name,shortcut);content.append(entry);
  }
  return content;
}
function showContext(event,items) {
  event?.preventDefault();closeMenus();const menu=$("context-menu");
  menu.replaceChildren(...menuItems(items).childNodes);menu.hidden=false;
  const x=event?.clientX??20,y=event?.clientY??100;
  menu.style.left=Math.max(4,Math.min(x,innerWidth-menu.offsetWidth-6))+"px";
  menu.style.top=Math.max(32,Math.min(y,innerHeight-menu.offsetHeight-6))+"px";scheduleBounds();
}
function renderMenus(menus) {
  indexActions(menus);const signature=JSON.stringify(menus);
  if(signature===menuSignature||document.querySelector(".menu[open]"))return;
  menuSignature=signature;const fragment=document.createDocumentFragment();
  for(const item of menus||[]) {
    if(!item.children)continue;
    const menu=document.createElement("details");menu.className="menu";
    const title=document.createElement("summary");title.textContent=clean(item.text);
    const list=menuItems(item.children);list.className="menu-popover";menu.append(title,list);prepareMenu(menu,list,item.id);
    menu.addEventListener("toggle",()=>{if(menu.open){$("context-menu").hidden=true;document.querySelectorAll(".menu[open]").forEach(other=>{if(other!==menu)other.open=false;});}scheduleBounds();});
    fragment.append(menu);
  }
  $("menus").replaceChildren(fragment);
}
function toolbarSymbol(name) {
  if(/Add/.test(name))return"plus";if(/Remove/.test(name))return"trash";
  if(/Up|Top/.test(name))return"up";if(/Down|Bottom/.test(name))return"down";
  if(/Filter/.test(name))return"filter";return"gear";
}
function renderToolbar(id,items) {
  const target=$(id),signature=JSON.stringify(items||[]);indexActions(items);
  if(target.dataset.signature===signature)return;target.dataset.signature=signature;
  const nodes=[];
  for(const action of items||[]) {
    if(action.separator){const separator=document.createElement("span");separator.className="divider";nodes.push(separator);continue;}
    const element=iconButton(toolbarSymbol(action.name||""),action.text,()=>request("action.invoke",{id:action.id}),action.enabled);
    element.dataset.action=action.name||action.id;nodes.push(element);
  }
  if(id==="source-toolbar")nodes.push(iconButton("folder","Группировать выделенные источники",()=>request("native.command",{id:"source.group"}),state.sources.some(x=>x.selected)));
  target.replaceChildren(...nodes);
}
function listDropPosition(rect,event,group=false,grid=false) {
  const fraction=grid?(event.clientX-rect.left)/rect.width:(event.clientY-rect.top)/rect.height;
  if(group&&fraction>.2&&fraction<.8)return "inside";
  return fraction>.5?"after":"before";
}
function listNavigationTarget(entries,index,key,grid=false,viewportHeight=0) {
  if(!entries.length)return index;
  if(key==="Home")return 0;if(key==="End")return entries.length-1;
  const rects=entries.map(entry=>entry.getBoundingClientRect()),current=rects[index];
  if(!current)return index;
  if(key==="PageUp"||key==="PageDown"){
    const target=current.top+(key==="PageUp"?-1:1)*Math.max(current.height,viewportHeight);
    return rects.reduce((best,rect,i)=>Math.abs(rect.top-target)<Math.abs(rects[best].top-target)?i:best,index);
  }
  if(grid&&(key==="ArrowUp"||key==="ArrowDown")){
    const direction=key==="ArrowUp"?-1:1,candidates=rects.map((rect,i)=>({rect,i})).filter(({rect})=>(rect.top-current.top)*direction>1);
    candidates.sort((a,b)=>Math.abs(a.rect.top-current.top)-Math.abs(b.rect.top-current.top)||Math.abs(a.rect.left-current.left)-Math.abs(b.rect.left-current.left));
    return candidates[0]?.i??index;
  }
  const step=(key==="ArrowUp"||key==="ArrowLeft")?-1:(key==="ArrowDown"||key==="ArrowRight")?1:0;
  return Math.max(0,Math.min(entries.length-1,index+step));
}
async function selectSource(row,event) {
  return request("source.select",{...sourceArgs(row),additive:!!(event?.ctrlKey||event?.metaKey),range:!!event?.shiftKey,focusOnly:!!event?.focusOnly});
}
async function rename(kind,row) {
  closeMenus();
  if(kind==="scene"&&row.uuid!==state.currentScene)await request("scene.select",{uuid:row.uuid});
  const list=$(kind==="source"?"sources":"scenes"),identity=kind==="source"?keyOf(row):row.uuid;
  const entry=Array.from(list.children).find(node=>node.dataset.id===identity),name=entry?.querySelector(".name");
  if(!name||entry.querySelector("input"))return;
  const editor=document.createElement("input");editor.className="row-name-editor";editor.value=row.name;editor.dataset.scene=state.currentScene;editor.dataset.identity=identity;editor.setAttribute("aria-label","Переименовать");
  name.replaceChildren(editor);entry.draggable=false;let finished=false;
  const finish=async(save,notify=true)=>{if(finished)return;finished=true;entry.draggable=true;name.textContent=row.name;
    if(kind==="source"&&notify)await request("source.rename",{...sourceArgs(row),name:editor.value,save});
    else if(save&&editor.value!==row.name)await request("scene.rename",{uuid:row.uuid,name:editor.value});
    list.dataset.signature="";renderRows(list,kind==="source"?state.sources:state.scenes,kind==="source");
    Array.from(list.children).find(node=>node.dataset.id===identity)?.focus({preventScroll:true});};
  // A native model reset already finalized its pending editor before publishing
  // this replacement state. Do not send an obsolete item/scene back to OBS.
  editor.cancelRename=()=>finish(false,false);
  editor.addEventListener("keydown",event=>{event.stopPropagation();if(event.key==="Enter"&&!event.isComposing){event.preventDefault();finish(true);}else if(event.key==="Escape"){event.preventDefault();finish(false);}});
  editor.addEventListener("blur",()=>finish(true));editor.focus();editor.select();
}
function actionEntry(name) { return actionsByName.get(name)||null; }
function finishNativeRename() {
  for(const id of ["sources","scenes"])$(id).querySelector(".row-name-editor")?.cancelRename();
}
function rowContext(event,row,sources) {
  event.preventDefault();closeMenus();
  request("native.command",sources?{...sourceArgs(row),item:row.id,id:"source.context"}:{id:"scene.context"});
}
function renderRows(element,rows,sources=false) {
  const editor=element.querySelector(".row-name-editor");
  if(editor){if(editor.dataset.scene!==state.currentScene||!rows.some(row=>(sources?keyOf(row):row.uuid)===editor.dataset.identity))editor.cancelRename();return;}
  const signature=JSON.stringify([rows,state.currentScene]);
  if(element.dataset.signature===signature)return;element.dataset.signature=signature;
  const focused=element.contains(document.activeElement)?document.activeElement.closest(".row")?.dataset.id:null;
  const scroll=element.scrollTop,nodes=[];
  for(const row of rows) {
    const identity=sources?keyOf(row):row.uuid;const entry=document.createElement("div");entry.className="row";
    const tabRow=sources?(rows.find(value=>value.selected)??rows[0]):(rows.find(value=>value.uuid===state.currentScene)??rows[0]);
    entry.tabIndex=row===tabRow?0:-1;
    entry.dataset.id=identity;entry.setAttribute("role","option");entry.setAttribute("aria-selected",String(sources?row.selected:row.uuid===state.currentScene));
    entry.draggable=true;entry.style.paddingLeft=(5+(row.depth||0)*16)+"px";
    if(sources&&row.group){const expand=button(row.collapsed?"▸":"▾",e=>{e.stopPropagation();request("source.expand",{...sourceArgs(row),value:!!row.collapsed});});expand.className="expand";expand.setAttribute("aria-label",row.collapsed?"Развернуть группу":"Свернуть группу");entry.append(expand);}
    const type=document.createElement("span");type.className="type-icon";
    if(sources&&row.icon?.startsWith("data:image/png;base64,")){const image=document.createElement("img");image.src=row.icon;image.width=image.height=16;image.alt="";type.append(image);}else type.append(icon(row.group?"folder":sources?"image":"monitor"));
    const name=document.createElement("span");name.className="name";name.textContent=row.name;if(sources)entry.append(type);entry.append(name);
    if(sources&&/^#[0-9a-f]{8}$/i.test(row.color||""))entry.style.setProperty("--source-color",row.color);
    if(sources) {
      const visible=iconButton("eye",row.visible?"Скрыть источник":"Показать источник",e=>{e.stopPropagation();request("source.visibility",{...sourceArgs(row),value:!row.visible});});
      visible.className="flag"+(row.visible?"":" off");visible.setAttribute("aria-pressed",String(row.visible));
      const locked=iconButton("lock",row.locked?"Разблокировать источник":"Заблокировать источник",e=>{e.stopPropagation();request("source.lock",{...sourceArgs(row),value:!row.locked});});
      locked.className="flag"+(row.locked?"":" off");locked.setAttribute("aria-pressed",String(row.locked));entry.append(visible,locked);
    }
    const select=event=>sources?selectSource(row,event):request("scene.select",{uuid:row.uuid});
    entry.addEventListener("click",event=>{if(!event.target.closest("button,input"))select(event);});
    entry.addEventListener("dblclick",async event=>{if(event.target.closest("button,input"))return;if(sources){if(row.group)request("source.expand",{...sourceArgs(row),value:!!row.collapsed});else request("source.properties",{uuid:row.uuid});}else{if(state.currentScene!==row.uuid)await select({});request("scene.activate",{uuid:row.uuid});}});
    if(sources){entry.addEventListener("mouseenter",()=>request("source.hover",{...sourceArgs(row),value:true}));entry.addEventListener("mouseleave",()=>request("source.hover",{...sourceArgs(row),value:false}));}
    entry.addEventListener("keydown",async event=>{
      if(event.target!==entry)return;
      if(sources&&(event.ctrlKey||event.metaKey)&&!event.shiftKey&&event.code==="KeyA"){event.preventDefault();request("source.selectAll",{scene:state.currentScene});}
      else if(event.key==="Enter"||event.key===" "){event.preventDefault();select(event);}
      else if(event.key==="F2"){event.preventDefault();const selected=sources?state.sources.find(value=>value.selected):state.scenes.find(value=>value.uuid===state.currentScene);if(selected)rename(sources?"source":"scene",selected);}
      else if(event.key==="Delete"){event.preventDefault();invokeName(sources?"actionRemoveSource":"actionRemoveScene");}
      else if(["ArrowUp","ArrowDown","ArrowLeft","ArrowRight","Home","End","PageUp","PageDown"].includes(event.key)){
        if(sources&&(event.key==="ArrowLeft"||event.key==="ArrowRight"))return;
        event.preventDefault();const entries=Array.from(element.children),index=entries.indexOf(entry),grid=!sources&&!!state.workspace?.actionSceneGridMode;
        const next=listNavigationTarget(entries,index,event.key,grid,element.clientHeight),target=rows[next];entries[next]?.focus();
        if(target){if(sources)selectSource(target,{ctrlKey:event.ctrlKey,metaKey:event.metaKey,shiftKey:event.shiftKey,focusOnly:(event.ctrlKey||event.metaKey)&&!event.shiftKey});else if(!event.ctrlKey&&!event.metaKey)request("scene.select",{uuid:target.uuid});}
      }
    });
    entry.addEventListener("contextmenu",async event=>{
      event.preventDefault();const position={preventDefault(){},clientX:event.clientX,clientY:event.clientY};
      if(!(sources?row.selected:row.uuid===state.currentScene))await select({});
      rowContext(position,row,sources);
    });
    entry.addEventListener("dragstart",event=>{dragged={row,sources,scene:state.currentScene};event.dataTransfer.effectAllowed="move";event.dataTransfer.setData("text/plain",identity);});
    entry.addEventListener("dragend",()=>{dragged=null;for(const node of element.children)node.classList.remove("drag-over");});
    entry.addEventListener("dragover",event=>{if(dragged?.sources===sources){event.preventDefault();entry.classList.add("drag-over");}});
    entry.addEventListener("dragleave",()=>entry.classList.remove("drag-over"));
    entry.addEventListener("drop",async event=>{
      event.preventDefault();event.stopPropagation();entry.classList.remove("drag-over");if(!dragged||dragged.sources!==sources)return;
      const from=dragged;dragged=null;
      const position=listDropPosition(entry.getBoundingClientRect(),event,sources&&row.group,!sources&&!!state.workspace?.actionSceneGridMode);
      if(sources){if(from.scene!==state.currentScene)return;if(!state.sources.find(s=>keyOf(s)===keyOf(from.row))?.selected)await selectSource(from.row,{});request("source.move",{...sourceArgs(from.row),target:sourceArgs(row),position});}
      else{await request("scene.select",{uuid:from.row.uuid});request("scene.move",{uuid:from.row.uuid,target:row.uuid,position});}
    });
    nodes.push(entry);
  }
  if(!rows.length){
    const empty=document.createElement("div");empty.className="empty";empty.append(document.createTextNode(sources?"Добавьте источник: захват экрана, игру, камеру, изображение или текст.":"Создайте первую сцену."));
    empty.append(document.createElement("br"),button(sources?"+ Добавить источник":"+ Добавить сцену",()=>invokeName(sources?"actionAddSource":"actionAddScene")));nodes.push(empty);
  }
  element.replaceChildren(...nodes);element.scrollTop=scroll;
  if(focused)Array.from(element.children).find(x=>x.dataset.id===focused)?.focus({preventScroll:true});
}
function installNativeFaderInput(volume,uuid,dispatch) {
  const keys=new Set(["ArrowLeft","ArrowRight","ArrowUp","ArrowDown","PageUp","PageDown","Home","End"]);
  volume.addEventListener("keydown",event=>{
    if(volume.disabled||event.defaultPrevented||event.isComposing||event.altKey||event.metaKey||!keys.has(event.key))return;
    event.preventDefault();dispatch("audio.key",{uuid,key:event.key,control:!!event.ctrlKey,shift:!!event.shiftKey});
  });
  volume.addEventListener("wheel",event=>{
    if(volume.disabled||document.activeElement!==volume||event.altKey||event.metaKey)return;
    // Chromium preserves the Windows wheel angle in wheelDeltaY. Keep partial
    // wheel ticks and let the original QSlider accumulate and accelerate them.
    const raw=Number.isFinite(event.wheelDeltaY)?event.wheelDeltaY:
      -event.deltaY*(event.deltaMode===1?40:event.deltaMode===2?120:1.2);
    const value=Math.round(raw);if(!value)return;
    event.preventDefault();dispatch("audio.wheel",{uuid,value,control:!!event.ctrlKey,shift:!!event.shiftKey});
  },{passive:false});
}
function applyNativeMixerMetrics(container, metrics, vertical) {
  // Qt and WebView can use different logical pixel scales on the same monitor.
  // Convert native dimensions once, including text, without reducing OBS's
  // configured font size or inventing a minimum from the main-window font.
  const nativeDpr=Number(metrics?.devicePixelRatio),browserDpr=window.devicePixelRatio||1;
  const scale=vertical&&nativeDpr>0&&Number.isFinite(nativeDpr)?nativeDpr/browserDpr:1;
  container.style.setProperty("--channel-native-scale",String(scale));
  if(!metrics)return;
  for(const [key,variable]of [["minimumHeight","channel-min-height"],["meterMinimumHeight","meter-min-height"],
    ["bodyMinimumHeight","meter-body-min-height"],["bottomPadding","channel-bottom-padding"],
    ["categoryHeight","channel-category-height"],["nameHeight","channel-name-height"],
    ["dbHeight","channel-db-height"],["buttonsHeight","channel-buttons-height"],["buttonWidth","channel-button-width"],
    ["categoryFontSize","channel-category-font"],["nameFontSize","channel-name-font"],
    ["dbFontSize","channel-db-font"],["meterFontSize","channel-meter-font"]]){
    const value=Number(metrics[key]);
    if(Number.isFinite(value)&&value>=0)container.style.setProperty("--"+variable,value+"px");
  }
  if(metrics.bodyMinimumHeight>=metrics.meterMinimumHeight)
    container.style.setProperty("--meter-frame-padding",((metrics.bodyMinimumHeight-metrics.meterMinimumHeight)/2)+"px");
}
function renderMixer(channels) {
  const present=new Set(), vertical=!!state.workspace?.verticalMixer;
  let cursor=$("mixer").firstElementChild;
  for(const channel of (channels||[]).filter(c=>c.visible!==false)) {
    present.add(channel.uuid);let entry=audioElements.get(channel.uuid);
    if(!entry) {
      const container=document.createElement("section");container.className="audio-channel";container.dataset.uuid=channel.uuid;
      const context=()=>request("native.command",{id:"audio.context",uuid:channel.uuid});
      const title=document.createElement("div");title.className="audio-title";
      const name=button("",context);name.className="audio-name";
      const nameText=document.createElement("span"),chevron=document.createElement("span");chevron.className="audio-chevron";chevron.textContent="▾";name.append(nameText,chevron);
      const category=document.createElement("span");category.className="audio-category";
      const db=document.createElement("span");db.className="audio-db";title.append(category,name,db);
      const controls=document.createElement("div");controls.className="audio-controls";
      const body=document.createElement("div");body.className="audio-body";
      const fader=document.createElement("div");fader.className="audio-fader";
      const ticks=document.createElement("div");ticks.className="fader-ticks";ticks.setAttribute("aria-hidden","true");
      const volume=document.createElement("input");volume.type="range";volume.min="0";volume.max="1";volume.step=String(1/4096);fader.append(ticks,volume);
      const meterFrame=document.createElement("div");meterFrame.className="meter-frame";meterFrame.setAttribute("aria-hidden","true");
      const meters=document.createElement("div");meters.className="meters";
      const scale=document.createElement("div");scale.className="meter-scale";
      for(let value=0;value>=-60;value-=6){const label=document.createElement("span");label.textContent=value;label.style.setProperty("--tick",String(-value/60));scale.append(label);}
      meterFrame.append(meters,scale);body.append(fader,meterFrame);
      const buttons=document.createElement("div");buttons.className="audio-buttons";
      let pending,flight=false,pointerEditing=false,inputFlight=0;
      const volumeBusy=()=>pointerEditing||flight||pending!==undefined||inputFlight>0;
      const endVolumeEdit=()=>{
        pointerEditing=false;
        const current=state?.audio.find(item=>item.uuid===channel.uuid);
        if(current&&!volumeBusy()){volume.value=current.volume;fader.style.setProperty("--volume",Number(volume.value)*100+"%");}
      };
      volume.addEventListener("pointerdown",event=>{if(event.button===0){pointerEditing=true;volume.setPointerCapture(event.pointerId);}});
      for(const event of ["pointerup","pointercancel","lostpointercapture"])volume.addEventListener(event,endVolumeEdit);
      const sendVolume=async()=>{if(flight)return;flight=true;while(pending!==undefined){const value=pending;pending=undefined;await request("audio.volume",{uuid:channel.uuid,value});}flight=false;};
      volume.addEventListener("input",()=>{fader.style.setProperty("--volume",Number(volume.value)*100+"%");pending=Number(volume.value);sendVolume();});
      installNativeFaderInput(volume,channel.uuid,async(command,args)=>{
        ++inputFlight;try{await request(command,args);}finally{--inputFlight;endVolumeEdit();}
      });
      const mute=iconButton("volume","Заглушить",()=>{const current=state.audio.find(c=>c.uuid===channel.uuid);if(current)request("audio.mute",{uuid:channel.uuid,value:!current.muted});});mute.className="audio-mute";
      const monitor=iconButton("headphones","Включить мониторинг",()=>{const current=state.audio.find(c=>c.uuid===channel.uuid);if(current)request("audio.monitor",{uuid:channel.uuid,value:current.monitoring?0:2});});monitor.className="audio-monitor";
      buttons.append(mute,monitor);controls.append(body,buttons);container.append(title,controls);
      container.addEventListener("contextmenu",event=>{event.preventDefault();context();});
      entry={container,name,nameText,category,db,volume,volumeBusy,mute,monitor,meters,bars:[],fader,ticks};audioElements.set(channel.uuid,entry);
    }
    entry.nameText.textContent=channel.name;entry.name.title=channel.name;entry.name.setAttribute("aria-label","Действия канала: "+channel.name);
    entry.category.textContent=channel.category|| (channel.global?"Глобальный":channel.pinned?"Закреплён":channel.active===false?"Неактивен":"Активен");
    entry.db.textContent=channel.dbText|| (Number.isFinite(channel.db)&&channel.db>-96?channel.db.toFixed(1)+" dB":"-inf dB");entry.volume.setAttribute("aria-label",channel.name+" — громкость");
    entry.volume.setAttribute("aria-orientation",vertical?"vertical":"horizontal");entry.volume.setAttribute("aria-valuetext",entry.db.textContent);
    if(!entry.volumeBusy())entry.volume.value=channel.volume;
    entry.fader.style.setProperty("--volume",Number(entry.volume.value)*100+"%");
    entry.volume.disabled=!channel.enabled||channel.volumeEnabled===false;
    entry.mute.disabled=!channel.enabled||channel.muteEnabled===false;entry.monitor.disabled=!channel.enabled||channel.monitorEnabled===false||channel.monitoringAvailable===false;
    entry.mute.classList.toggle("muted",channel.muted);entry.monitor.classList.toggle("monitored",!!channel.monitoring);
    for(const [control,prefix,fallback,pressed] of [[entry.mute,"mute",channel.muted?"muted":"volume",channel.muted],[entry.monitor,"monitor","headphones",!!channel.monitoring]]){
      control.title=channel[prefix+"Tooltip"]||(prefix==="mute"?(pressed?"Включить звук":"Заглушить"):(pressed?"Отключить мониторинг":"Включить мониторинг"));
      control.setAttribute("aria-label",control.title+": "+channel.name);control.setAttribute("aria-pressed",String(pressed));
      const image=channel[prefix+"Icon"],signature=image||fallback;
      if(control.dataset.icon!==signature){control.dataset.icon=signature;if(image?.startsWith("data:image/png;base64,")){const img=document.createElement("img");img.src=image;img.alt="";control.replaceChildren(img);}else control.replaceChildren(icon(fallback));}
    }
    const count=Math.max(1,Math.min(8,channel.channels||2));
    if(entry.meters.childElementCount!==count){
      entry.meters.replaceChildren();entry.bars=[];
      for(let n=0;n<count;n++){
        const bar=document.createElement("div");bar.className="meter";
        const track=document.createElement("div");track.className="meter-track";
        const peak=document.createElement("span");peak.className="meter-peak";
        const hold=document.createElement("span");hold.className="meter-mark meter-hold";
        const magnitude=document.createElement("span");magnitude.className="meter-mark meter-magnitude";
        const input=document.createElement("span");input.className="meter-input";input.hidden=true;
        track.append(peak,hold,magnitude);bar.append(track,input);entry.meters.append(bar);entry.bars.push({peak,hold,magnitude,input});
      }
    }
    entry.meters.style.setProperty("--channels",String(count));
    entry.meters.style.setProperty("--meter-thickness",Math.max(3,Math.min(6,channel.meterThickness||4))+"px");
    entry.container.style.setProperty("--channel-width",Math.max(70,Math.min(110,channel.preferredWidth||88))+"px");
    applyNativeMixerMetrics(entry.container,channel.verticalMetrics,vertical);
    for(const [key,variable] of [["categoryColor","category-text"],["categoryBackground","category-bg"]])if(/^#[0-9a-f]{6}$/i.test(channel[key]||""))entry.container.style.setProperty("--"+variable,channel[key]);
    entry.minimum=channel.meterMinimum??-60;entry.warning=channel.meterWarning??-20;entry.error=channel.meterError??-9;
    for(const [name,level]of [["warning",entry.warning],["error",entry.error]])entry.container.style.setProperty("--meter-"+name,(100*(1-level/entry.minimum))+"%");
    const magnitudeColor=channel.meterColors?.magnitudeColor;
    if(/^#[0-9a-f]{6}$/i.test(magnitudeColor||""))entry.container.style.setProperty("--meter-magnitude",magnitudeColor);
    const disabledColors=channel.meterDisabledColors??(channel.muted||channel.active===false||(channel.unassigned&&!channel.monitoring));
    for(const [variable,key]of [["meter-bg-green","backgroundNominalColor"],["meter-bg-yellow","backgroundWarningColor"],["meter-bg-red","backgroundErrorColor"],["meter-green","foregroundNominalColor"],["meter-yellow","foregroundWarningColor"],["meter-red","foregroundErrorColor"]]){
      const color=channel.meterColors?.[key+(disabledColors?"Disabled":"")];if(/^#[0-9a-f]{6}$/i.test(color||""))entry.container.style.setProperty("--"+variable,color);
    }
    entry.container.classList.toggle("audio-inactive",channel.active===false);
    const ticks=channel.faderTicks||[],signature=JSON.stringify(ticks);
    if(entry.ticks.dataset.signature!==signature){entry.ticks.dataset.signature=signature;entry.ticks.replaceChildren(...ticks.map(value=>{const tick=document.createElement("span");tick.style.setProperty("--fader-tick",String(value));return tick;}));}
    // Keep pointer capture/focus during volume changes. Move only when native
    // group/order actually changed; an append on every snapshot ends a drag.
    if(entry.container===cursor)cursor=cursor.nextElementSibling;
    else $("mixer").insertBefore(entry.container,cursor);
  }
  for(const [uuid,entry]of audioElements)if(!present.has(uuid)){entry.container.remove();audioElements.delete(uuid);}
  $("mixer").querySelector(".empty")?.remove();
  if(!present.size){const empty=document.createElement("div");empty.className="empty";empty.textContent="Аудиоисточники появятся здесь после добавления."; $("mixer").append(empty);}
  const footer=state.mixerToolbar||{},hidden=footer.hidden;
  mixerHidden.textContent=hidden?.text||"Скрыто: "+(channels||[]).filter(channel=>channel.hidden).length;mixerHidden.disabled=hidden?.enabled!==true;mixerHidden.title=hidden?.tooltip||"Показать скрытые каналы";mixerHidden.setAttribute("aria-pressed",String(!!hidden?.checked));
  mixerLayout.title=vertical?"Горизонтальная компоновка":"Вертикальная компоновка";mixerLayout.setAttribute("aria-label",mixerLayout.title);mixerLayout.replaceChildren(icon(vertical?"layoutHorizontal":"layoutVertical"));
  $("mixer-menu").textContent=(footer.optionsText||"Параметры")+" ▾";
}
function updateLevels(levels) {
  if(!rendersPanel("mixerDock"))return;
  for(const [uuid,state]of Object.entries(levels||{})){
    const entry=audioElements.get(uuid);if(!entry)continue;
    const values=Array.isArray(state)?state.map(peak=>({peak})):state.channels||[];
    const position=db=>Math.max(0,Math.min(100,100*(1-(db??-100)/(state.minimum??entry.minimum??-60))));
    entry.bars.forEach((bar,index)=>{
      const value=values[index]||{},peak=state.clipping?100:position(value.peak);
      bar.peak.style.setProperty("--peak-empty",(100-peak)+"%");
      bar.peak.style.setProperty("background",state.clipping?"var(--meter-red)":"");
      for(const [mark,db]of [[bar.hold,value.peakHold],[bar.magnitude,value.magnitude]]){
        mark.hidden=position(db)<=0;mark.style.setProperty("--meter-position",position(db)+"%");
      }
      bar.hold.style.setProperty("background-color",value.peakHold>=(entry.error??-9)?"var(--meter-red)":value.peakHold>=(entry.warning??-20)?"var(--meter-yellow)":"var(--meter-green)");
      bar.input.hidden=!!state.idle||!/^#[0-9a-f]{6}$/i.test(value.inputColor||"");
      if(!bar.input.hidden)bar.input.style.setProperty("background-color",value.inputColor);
    });
  }
}
let pendingLevels,levelsFrame=0;
function scheduleLevels(levels) {
  pendingLevels=levels;
  if(levelsFrame)return;
  levelsFrame=requestAnimationFrame(()=>{
    levelsFrame=0;
    const latest=pendingLevels;pendingLevels=undefined;
    updateLevels(latest);
  });
}
function renderTransitions(next) {
  if(rendersPanel("transitionsDock")){
  const select=$("transition-type"),signature=JSON.stringify(next.transitions||[]);
  if(select.dataset.signature!==signature){select.dataset.signature=signature;select.replaceChildren(...(next.transitions||[]).map(value=>{const option=document.createElement("option");option.value=value.uuid;option.textContent=value.name;return option;}));}
  select.value=next.currentTransition||"";select.disabled=next.transitionEnabled===false;
  if(document.activeElement!==$("transition-duration"))$("transition-duration").value=next.transitionDuration??300;
  $("transition-duration-label").hidden=!!next.transitionFixed;
  const target=$("transition-toolbar"),buttons=next.transitionControls||[];const sig=JSON.stringify(buttons);
  if(target.dataset.signature!==sig){target.dataset.signature=sig;target.replaceChildren(...buttons.map(c=>iconButton(toolbarSymbol(c.name||c.id),c.text,()=>request("control.click",{id:c.id}),c.enabled)));}
  }
  if(panelMode)return;
  $("program-column").hidden=$("studio-controls").hidden=!next.studioMode;
  $("studio-transition").disabled=next.studioTransitionEnabled===false;
  $("tbar").disabled=next.tbarEnabled===false;
  $("preview-mode").textContent=next.studioMode?"Предпросмотр":"";
  $("program-title").textContent=next.programName||"";
  const quick=next.quickTransitions||[],quickSignature=JSON.stringify([quick,next.addQuickTransition,next.addQuickTransitionEnabled]);
  if($("quick-transitions").dataset.signature!==quickSignature){
    $("quick-transitions").dataset.signature=quickSignature;
    const nodes=quick.map(c=>{
      const row=document.createElement("div");row.className="quick-row";
      const action=button(c.text,()=>request("control.click",{id:c.id}),c.enabled);action.title=clean(c.text);
      row.append(action,iconButton("dots","Настроить "+clean(c.text),()=>request("native.command",{id:"studio.quick.options",button:c.id}),c.enabled));
      return row;
    });
    if(next.addQuickTransition)nodes.push(button("+ Быстрый переход",()=>request("control.click",{id:next.addQuickTransition}),next.addQuickTransitionEnabled!==false));
    $("quick-transitions").replaceChildren(...nodes);
  }
  const tbar=$("tbar"),geometry=next.tbarGeometry;
  if(geometry)applyTBarGeometry(tbar,geometry);
  // The original TBarReleased can snap to zero while the Web input still has
  // focus. Every gesture is native, so focus must not suppress native updates.
  tbar.value=next.tbar??0;
}
function applyTBarGeometry(slider,geometry){
  slider._nativeGeometry=geometry;
  slider.min=geometry.minimum;slider.max=geometry.maximum;
  const scale=(geometry.devicePixelRatio||1)/(window.devicePixelRatio||1);
  const height=(geometry.preferredHeight||geometry.height)*scale;
  const nativeWidth=geometry.width||slider.getBoundingClientRect().width;
  const thumbWidth=(geometry.thumbWidthPixels||geometry.thumbWidth*nativeWidth)*scale;
  const thumbHeight=Math.min(height,(geometry.thumbHeightPixels||geometry.thumbHeight*geometry.height)*scale);
  const left=Math.max(0,(Math.min(geometry.first,geometry.last)-geometry.thumbWidth/2)*nativeWidth*scale);
  const right=Math.max(0,(1-Math.max(geometry.first,geometry.last)-geometry.thumbWidth/2)*nativeWidth*scale);
  slider._visualGeometry={thumbWidth,height,scale,left,right};
  slider.style.height=height+"px";
  slider.style.setProperty("--tbar-thumb-width",thumbWidth+"px");
  slider.style.setProperty("--tbar-thumb-height",thumbHeight+"px");
  slider.style.paddingLeft=left+"px";slider.style.paddingRight=right+"px";
  slider.dir=geometry.first>geometry.last?"rtl":"ltr";
}
function installNativeTBarInput(slider,dispatch) {
  const keys=new Set(["ArrowLeft","ArrowRight","ArrowUp","ArrowDown","PageUp","PageDown","Home","End"]);
  let pointer=null,button=0,last={x:0,y:0.5};
  const point=event=>{
    const rect=slider.getBoundingClientRect();
    let x=(event.clientX-rect.left)/Math.max(1,rect.width),y=(event.clientY-rect.top)/Math.max(1,rect.height);
    const native=slider._nativeGeometry,visual=slider._visualGeometry;
    if(native&&visual){
      const first=(visual.left+visual.thumbWidth/2)/rect.width,last=1-(visual.right+visual.thumbWidth/2)/rect.width;
      if(last>first)x=Math.min(native.first,native.last)+(x-first)/(last-first)*Math.abs(native.last-native.first);
      y=.5+(y-.5)*rect.height/(Math.max(1,native.height)*visual.scale);
    }
    return {x:Math.max(-1,Math.min(2,x)),y:Math.max(-1,Math.min(2,y))};
  };
  const send=(kind,event)=>dispatch("studio.tbar.input",{kind,...last,button,control:!!event.ctrlKey,shift:!!event.shiftKey});
  slider.addEventListener("pointerdown",event=>{
    if(slider.disabled||pointer!==null||(event.button!==0&&event.button!==1))return;
    event.preventDefault();slider.focus();pointer=event.pointerId;button=event.button;last=point(event);
    slider.setPointerCapture(pointer);send("press",event);
  });
  slider.addEventListener("pointermove",event=>{
    if(event.pointerId!==pointer)return;
    event.preventDefault();last=point(event);send("move",event);
  });
  const release=event=>{
    if(event.pointerId!==pointer)return;
    event.preventDefault();const captured=pointer;pointer=null;
    if(event.type==="pointerup")last=point(event);
    send("release",event);if(slider.hasPointerCapture(captured))slider.releasePointerCapture(captured);slider.blur();
  };
  for(const name of ["pointerup","pointercancel","lostpointercapture"])slider.addEventListener(name,release);
  slider.addEventListener("keydown",event=>{
    if(slider.disabled||event.defaultPrevented||event.isComposing||event.altKey||event.metaKey||!keys.has(event.key))return;
    event.preventDefault();dispatch("studio.tbar.input",{kind:"key",key:event.key,control:!!event.ctrlKey,shift:!!event.shiftKey});
  });
  slider.addEventListener("wheel",event=>{
    if(slider.disabled||document.activeElement!==slider||event.altKey||event.metaKey)return;
    const value=Math.round(Number.isFinite(event.wheelDeltaY)?event.wheelDeltaY:-event.deltaY*(event.deltaMode===1?40:event.deltaMode===2?120:1.2));
    if(!value)return;event.preventDefault();dispatch("studio.tbar.input",{kind:"wheel",value,control:!!event.ctrlKey,shift:!!event.shiftKey});
  },{passive:false});
}
function renderControls(controls) {
  const signature=JSON.stringify(controls||[]);if($("controls").dataset.signature===signature)return;
  $("controls").dataset.signature=signature;const focus=document.activeElement?.dataset.control;
  const rows=new Map();
  for(const control of controls||[]){
    const group=control.group||control.id;
    if(!rows.has(group)){const row=document.createElement("div");row.className="control-row";rows.set(group,row);}
    const b=button(control.iconOnly?"":control.text,()=>request("control.click",{id:control.id}),control.enabled);
    b.dataset.control=control.id;b.className="native-control";b.title=clean(control.tooltip||control.text);b.setAttribute("aria-label",clean(control.text));
    b.classList.toggle("active",!!control.active);b.classList.toggle("icon-only",!!control.iconOnly);
    if(control.checkable||control.active)b.setAttribute("aria-pressed",String(control.active||control.checked));
    if(control.icon?.startsWith("data:image/png;base64,")){const img=document.createElement("img");img.src=control.icon;img.alt="";b.prepend(img);}
    if(control.menu){const arrow=document.createElement("span");arrow.className="control-menu-arrow";arrow.textContent="▾";b.append(arrow);b.setAttribute("aria-haspopup","menu");}
    for(const [field,variable]of [["background","control-background"],["foreground","control-foreground"]])if(/^#[0-9a-f]{6}$/i.test(control[field]||""))b.style.setProperty("--"+variable,control[field]);
    rows.get(group).append(b);
  }
  $("controls").replaceChildren(...rows.values());
  if(focus)Array.from($("controls").querySelectorAll("[data-control]")).find(c=>c.dataset.control===focus)?.focus({preventScroll:true});
}
function renderPreview(next) {
  if(panelMode)return;
  const controls=next.previewControls||{enabled:true},select=$("preview-scaling");
  const options=[...(controls.options||[])];
  if(controls.index===-1)options.push({index:-1,text:controls.placeholder});
  const signature=JSON.stringify(options);
  if(select.dataset.signature!==signature){select.dataset.signature=signature;select.replaceChildren(...options.map(item=>{const option=document.createElement("option");option.value=String(item.index);option.textContent=item.text;option.disabled=item.index===-1;return option;}));}
  select.value=String(controls.index??0);
  $("preview-percent").textContent=controls.percent||"";
  $("preview-disabled").hidden=controls.enabled!==false;
  $("enable-preview").textContent=clean(controls.enableText||"Включить предпросмотр");
  $("preview-grid").classList.toggle("preview-disabled",controls.enabled===false);
  $("zoom-in").disabled=actionEntry("actionPreviewZoomIn")?.enabled===false;
  $("zoom-out").disabled=actionEntry("actionPreviewZoomOut")?.enabled===false;
  for(const axis of ["x","y"]){
    const element=$("preview-scroll-"+axis),bar=controls[axis==="x"?"previewXScrollBar":"previewYScrollBar"];
    element.hidden=controls.enabled===false;
    if(!bar)continue;
    const hostExtent=axis==="x"?controls.hostHeight:controls.hostWidth;
    const cssExtent=axis==="x"?innerHeight:innerWidth;
    if(Number.isFinite(bar.extent)&&bar.extent>0)
      $("preview-grid").style.setProperty("--preview-scroll-"+axis,Math.max(1,bar.extent*(hostExtent>0?cssExtent/hostExtent:1))+"px");
    element._nativeBar=bar;
    const extent=axis==="x"?element.clientWidth:element.clientHeight;
    const range=bar.max-bar.min,full=range+Math.max(1,bar.page);
    const content=Math.max(extent,Math.ceil(extent*full/Math.max(1,bar.page)));
    element.firstElementChild.style[axis==="x"?"width":"height"]=content+"px";
    const maximum=axis==="x"?element.scrollWidth-element.clientWidth:element.scrollHeight-element.clientHeight;
    const value=range>0?Math.round((bar.value-bar.min)*maximum/range):0;
    // A snapshot acknowledges native movement. Ignore only its own queued
    // scroll event; genuine mouse/wheel/keyboard scrolling goes to OBS.
    element._reportedScroll=value;
    if(!element.matches(":active"))element[axis==="x"?"scrollLeft":"scrollTop"]=value;
  }
}
function render(next) {
  state=next;connected=true;document.title=next.title+" — WebView2";$("connection").textContent="OBS Studio";
  $("scene-title").textContent=next.scenes.find(scene=>scene.uuid===next.currentScene)?.name??"";
  for(const [id,label]of [["scenes-title","Basic.Main.Scenes"],["sources-title","Basic.Main.Sources"],["mixer-title","Mixer"],["controls-title","Basic.Main.Controls"],["transitions-title","Basic.SceneTransitions"]])if(next.labels?.[label])$(id).textContent=next.labels[label];
  if(next.appearance){const a=next.appearance;for(const [variable,key]of [["bg","background"],["panel","panel"],["raised","raised"],["text","text"],["muted","muted"],["selected","selected"],["selection-text","selectedText"]])if(/^#[0-9a-f]{6}$/i.test(a[key]||""))document.documentElement.style.setProperty("--"+variable,a[key]);document.documentElement.style.colorScheme=a.light?"light":"dark";document.body.classList.toggle("light-theme",!!a.light);if(a.raised===a.background)document.documentElement.style.setProperty("--raised","color-mix(in srgb,var(--bg) 86%,var(--text) 14%)");}
  $("status").textContent=[Number.isFinite(next.cpu)?next.cpu.toFixed(1)+"% CPU":"",Number(next.fps||0).toFixed(2)+" FPS"].filter(Boolean).join("  ·  ");
  if(next.appearance?.fontFamily)document.documentElement.style.fontFamily=JSON.stringify(next.appearance.fontFamily)+', "Open Sans", sans-serif';
  if(next.appearance?.fontSize>0)document.documentElement.style.setProperty("--ui-font-size",next.appearance.fontSize+"px");
  $("output-status").textContent=(next.statusText||[]).filter(Boolean).join("    ")||[next.streaming?"● LIVE":"○ LIVE 00:00:00",next.recording?(next.paused?"Ⅱ REC":"● REC"):"○ REC 00:00:00"].join("    ");
  $("output-status").classList.toggle("live",next.recording||next.streaming);
  if(!panelMode&&!next.workspace?.nativeDocking)renderMenus(next.menus);else indexActions(next.menus);
  indexActions(next.actions);
  renderPreview(next);
  if(rendersPanel("scenesDock")){renderToolbar("scene-toolbar",next.sceneToolbar);renderRows($("scenes"),next.scenes);}
  if(rendersPanel("sourcesDock")){renderToolbar("source-toolbar",next.sourceToolbar);renderRows($("sources"),next.sources,true);}
  const selection=next.sources.filter(s=>s.selected),selected=selection.length===1?selection[0]:null;
  $("selected-name").textContent=selected?.name||(selection.length?selection.length+" источников выбрано":"Источник не выбран");
  $("properties").disabled=$("filters").disabled=!selected;$("interact").hidden=!selected?.interactive;
  $("source-tools").hidden=!next.sourceTools;
  if(rendersPanel("mixerDock"))renderMixer(next.audio||[]);
  if(!panelMode||panelMode==="transitionsDock")renderTransitions(next);
  if(rendersPanel("controlsDock"))renderControls(next.controls);
  renderWorkspace(next);
  scheduleBounds();
  present?.().catch(showError);
}
function rendersPanel(name){return panelMode?panelMode===name:!state?.workspace?.nativeDocking;}
function renderWorkspace(next) {
  const preferences=next.workspace||{};
  document.body.classList.toggle("native-docking",!!preferences.nativeDocking);
  for(const panel of $("panels").children){
    const dock=(next.docks||[]).find(item=>item.name===panel.dataset.panel);
    panel.hidden=panelMode?panel.dataset.panel!==panelMode:!!preferences.nativeDocking||!!dock&&(!dock.visible||dock.floating);
  }
  $("scene-toolbar").hidden=$("source-toolbar").hidden=preferences.toggleListboxToolbars===false;
  $("context-bar").hidden=preferences.toggleContextBar===false;
  document.querySelector("footer").hidden=preferences.toggleStatusBar===false;
  document.body.classList.toggle("no-source-icons",preferences.toggleSourceIcons===false);
  renderSceneGrid(next);
  $("mixer").classList.toggle("vertical-mixer",!!preferences.verticalMixer);
  $("panels").hidden=!Array.from($("panels").children).some(panel=>!panel.hidden);
  $("panel-resizer").hidden=true;
}
function renderSceneGrid(next) {
  const list=$("scenes"),enabled=!!next.workspace?.actionSceneGridMode;
  list.classList.toggle("scene-grid",enabled);
  const width=next.sceneGrid?.width>0?next.sceneGrid.width:150,height=next.sceneGrid?.height>0?next.sceneGrid.height:24;
  list.style.gridTemplateColumns=enabled?`repeat(${Math.max(1,Math.ceil(list.clientWidth/width))},minmax(0,1fr))`:"";
  list.style.setProperty("--scene-grid-height",height+"px");
}
function scheduleBounds() {
  if(panelMode)return;
  boundsDirty=true;
  if(boundsPending)return;
  if(boundsFrame)return;
  boundsFrame=requestAnimationFrame(async()=>{
    boundsFrame=0;if(!connected)return;
    boundsDirty=false;
    const overlayElements=[...document.querySelectorAll(".menu[open] > .menu-popover, .panel-drag-ghost"),...(!$("context-menu").hidden?[$("context-menu")]:[]),...($("rename-dialog").open?[$("rename-dialog")]:[])];
    const overlays=overlayElements.map(element=>{
      const rect=element.getBoundingClientRect(),x=Math.max(0,rect.left),y=Math.max(0,rect.top);
      return {x,y,width:Math.max(0,Math.min(innerWidth,rect.right)-x),height:Math.max(0,Math.min(innerHeight,rect.bottom)-y)};
    }).filter(rect=>rect.width&&rect.height);
    const surfaces=[];
    for(const target of ["preview","program"]){
      const area=$(target),bounds=area.getBoundingClientRect(),visible=target==="preview"?state.previewControls?.enabled!==false:state.studioMode;
      const args={target,x:bounds.x,y:bounds.y,width:bounds.width,height:bounds.height,visible};
      if(!bounds.width||!bounds.height){args.x=args.y=args.width=args.height=0;}
      surfaces.push(args);
    }
    const args={surfaces,viewportWidth:innerWidth,viewportHeight:innerHeight,overlays,modal:$("rename-dialog").open};
    const signature=JSON.stringify(args);if(boundsSignature===signature)return;
    boundsPending=true;
    try{if(await request("preview.layout",args)!==undefined)boundsSignature=signature;}
    finally{boundsPending=false;if(boundsDirty)scheduleBounds();}
  });
}
$("original").addEventListener("click",()=>request("window.original"));
$("error").querySelector("button").addEventListener("click",()=>{$("error").hidden=true;});
for(const kind of ["properties","filters","interact"])$(kind).addEventListener("click",()=>{
  const selected=state?.sources.filter(s=>s.selected);if(selected?.length===1)request("source."+kind,{uuid:selected[0].uuid});
});
$("advanced-audio").addEventListener("click",()=>invokeName("actionAdvAudioProperties"));
const mixerToolbar=$("advanced-audio").parentElement;
mixerToolbar.classList.add("mixer-toolbar");mixerToolbar.querySelector(".toolbar-caption")?.remove();
const mixerHidden=button("Скрыто: 0",()=>{const id=state?.mixerToolbar?.hidden?.id;if(id)request("control.click",{id});});mixerHidden.className="mixer-hidden";
const mixerLayout=iconButton("layoutVertical","Вертикальная компоновка",()=>{const id=state?.mixerToolbar?.layoutAction;if(id)request("action.invoke",{id});else invokeName("actionMixerToolbarToggleLayout");});mixerLayout.className="mixer-layout";
mixerToolbar.prepend(mixerHidden,mixerLayout);$("advanced-audio").replaceChildren(icon("gear"));
$("source-tools").addEventListener("click",()=>request("native.command",{id:"source.tools"}));
$("mixer-menu").addEventListener("click",()=>request("native.command",{id:"audio.options"}));
$("transition-type").addEventListener("change",()=>request("transition.select",{uuid:$("transition-type").value}));
$("transition-duration").addEventListener("change",()=>request("transition.duration",{value:Number($("transition-duration").value)}));
$("studio-transition").addEventListener("click",()=>request("native.command",{id:"studio.transition"}));
$("studio-config").addEventListener("click",()=>request("native.command",{id:"studio.options"}));
installNativeTBarInput($("tbar"),request);
$("tbar").addEventListener("input",()=>request("studio.tbar",{value:Number($("tbar").value)}));
$("zoom-in").addEventListener("click",()=>invokeName("actionPreviewZoomIn"));$("zoom-out").addEventListener("click",()=>invokeName("actionPreviewZoomOut"));
$("preview-scaling").addEventListener("change",()=>request("preview.scale",{index:Number($("preview-scaling").value)}));
$("enable-preview").addEventListener("click",()=>request("preview.enable"));
$("preview-disabled").addEventListener("contextmenu",event=>{event.preventDefault();request("preview.context");});
for(const axis of ["x","y"]){
  const element=$("preview-scroll-"+axis);
  element.addEventListener("scroll",()=>{
    const bar=element._nativeBar;if(!bar||bar.max<=bar.min)return;
    const value=axis==="x"?element.scrollLeft:element.scrollTop;
    if(Math.abs(value-element._reportedScroll)<1)return;
    const maximum=axis==="x"?element.scrollWidth-element.clientWidth:element.scrollHeight-element.clientHeight;
    if(maximum<=0)return;element._reportedScroll=value;
    request("preview.scroll",{axis,value:Math.round(bar.min+value*(bar.max-bar.min)/maximum)});
  });
}
$("rename-dialog").addEventListener("close",()=>{scheduleBounds();if($("rename-dialog").returnValue!=="ok"||!renameTarget)return;
  const {kind,row}=renameTarget;renameTarget=null;
  request(kind+".rename",{...(kind==="source"?sourceArgs(row):{uuid:row.uuid}),name:$("rename-value").value});
});
for(const [id,sources]of [["scenes",false],["sources",true]]){
  const element=$(id);
  element.addEventListener("contextmenu",event=>{if(event.target.closest(".row"))return;event.preventDefault();closeMenus();request("native.command",{id:sources?"source.context":"scene.context",empty:true,scene:state.currentScene});});
  element.addEventListener("dragover",event=>{if(dragged?.sources===sources&&!event.target.closest(".row"))event.preventDefault();});
  element.addEventListener("drop",async event=>{
    if(event.target.closest(".row")||!dragged||dragged.sources!==sources)return;
    event.preventDefault();const from=dragged;dragged=null;
    if(sources){if(from.scene!==state.currentScene)return;if(!state.sources.find(row=>keyOf(row)===keyOf(from.row))?.selected)await selectSource(from.row,{});request("source.move",{...sourceArgs(from.row),position:"end"});}
    else{await request("scene.select",{uuid:from.row.uuid});request("scene.move",{uuid:from.row.uuid,position:"end"});}
  });
}
document.addEventListener("click",event=>{if(!event.target.closest(".menu,.context-menu,.audio-controls,.row"))closeMenus();});
document.addEventListener("keydown",event=>{
  if(event.defaultPrevented||event.isComposing||event.getModifierState?.("AltGraph"))return;
  if(event.key==="Escape"){closeMenus();return;}
  const target=event.target;
  const editsText=target.tagName==="TEXTAREA"||target.isContentEditable||
    (target.tagName==="INPUT"&&!["range","checkbox","radio","button","submit","reset","image","color","file"].includes(target.type));
  if(editsText||target.closest("dialog"))return;
  const keyNames={Delete:"del",Backspace:"backspace",Insert:"ins",PageUp:"pgup",PageDown:"pgdown"," ":"space"};
  let key=keyNames[event.key]||event.key.toLowerCase().replace("arrow","");
  // Windows shortcuts remain usable with a Cyrillic layout. Latin layouts
  // retain their logical key (for example Y/Z on a German keyboard).
  if(!/^[\x20-\x7e]+$/.test(key)&&/^Key[A-Z]$/.test(event.code))key=event.code.slice(3).toLowerCase();
  const keys=[event.ctrlKey?"ctrl":event.metaKey?"meta":"",event.altKey?"alt":"",event.shiftKey?"shift":"",key].filter(Boolean).join("+");
  const action=[...actionsByName.values()].find(a=>a.enabled&&a.shortcutGlobal!==false&&
    (a.shortcutKeys||[a.shortcutKey||a.shortcut]).some(shortcut=>shortcut?.toLowerCase().replace(/ /g,"")===keys));
  if(action){event.preventDefault();request("action.invoke",{id:action.id});}
});
// Native QMainWindow owns docking, resizing and persistence. Each dock document
// only renders its content; its Qt title bar receives the original drag gestures.
const resizer=$("panel-resizer");resizer.hidden=true;
function resetPanelLayout(){if(state)renderWorkspace(state);scheduleBounds();}
document.addEventListener("toggle",scheduleBounds,true);
new ResizeObserver(scheduleBounds).observe($("preview"));new ResizeObserver(scheduleBounds).observe($("program"));
new ResizeObserver(()=>{if(state)renderSceneGrid(state);}).observe($("scenes"));
window.addEventListener("resize",scheduleBounds);window.addEventListener("pagehide",()=>bridge?.dispose());
try{bridge=createBridge(window.chrome?.webview);bridge.subscribe("state.changed",render);bridge.subscribe("audio.levels",scheduleLevels);bridge.subscribe("viewport.invalidate",()=>{boundsSignature="";scheduleBounds();});bridge.subscribe("workspace.reset",resetPanelLayout);bridge.subscribe("overlays.dismiss",closeMenus);bridge.subscribe("workspace.panel",data=>{if(!defaultPanelOrder.includes(data?.name))return;panelMode=data.name;document.body.classList.add("floating-panel","native-dock-panel");if(state)renderWorkspace(state);});}
catch(error){showError(error);$("connection").textContent="Нет соединения с OBS";}
if(bridge)present=createPresentation(bridge);
if(bridge)bridge.subscribe("workspace.rename",data=>{if((data?.kind==="source"&&panelMode==="sourcesDock")||(data?.kind==="scene"&&panelMode==="scenesDock")){const rows=data.kind==="source"?state?.sources:state?.scenes;const row=rows?.find(row=>data.kind==="source"?keyOf(row)===keyOf(data.row):row.uuid===data.row?.uuid);if(row)rename(data.kind,row);}});
if(bridge)bridge.subscribe("workspace.rename.finished",finishNativeRename);
if(bridge)installExternalDrop(document,{onError:showError});
