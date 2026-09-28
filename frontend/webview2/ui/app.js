import { createBridge } from "./bridge.mjs";
import { installExternalDrop } from "./external-drop.mjs";

const $ = id => document.getElementById(id);
const clean = text => String(text ?? "").replace(/&&/g, "\u0000").replace(/&/g, "").replace(/\u0000/g, "&");
const keyOf = row => [row.owner ?? "", row.id, row.uuid].join("/");
let bridge, state, renameTarget, dragged;
let menuSignature = "", boundsFrame = 0, connected = false;
const boundsSignatures = new Map(), audioElements = new Map(), actionsByName = new Map();
const defaultPanelOrder = ["scenesDock","sourcesDock","mixerDock","transitionsDock","controlsDock"];
const defaultPanelWeights = {scenesDock:1,sourcesDock:1.13,mixerDock:1.65,transitionsDock:1.05,controlsDock:1.18};
let panelWeights = {...defaultPanelWeights}, movingPanel, panelMode = "";
const nativeCommands = new Set(["action.invoke", "control.click", "native.command", "source.visibility", "source.lock", "source.expand", "source.rename", "source.move", "scene.rename", "scene.move"]);
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
async function selectSource(row,event) {
  return request("source.select",{...sourceArgs(row),additive:!!(event?.ctrlKey||event?.metaKey),range:!!event?.shiftKey});
}
function rename(kind,row) {
  closeMenus();renameTarget={kind,row};$("rename-title").textContent=kind==="scene"?"Переименовать сцену":"Переименовать источник";
  $("rename-value").value=row.name;$("rename-dialog").returnValue="";$("rename-dialog").showModal();$("rename-value").select();scheduleBounds();
}
function actionEntry(name) { return actionsByName.get(name)||null; }
function rowContext(event,row,sources) {
  const items=sources ? [
    actionEntry("actionAddSource"),{text:"Переименовать",callback:()=>rename("source",row)},
    actionEntry("actionRemoveSource"),{separator:true},actionEntry("actionCopySource"),actionEntry("actionPasteRef"),actionEntry("actionPasteDup"),
    {separator:true},{text:"Группировать выделенные",callback:()=>request("native.command",{id:"source.group"})},
    {text:"Разгруппировать",enabled:row.group,callback:()=>request("native.command",{id:"source.ungroup"})},
    actionEntry("transformMenu"),actionEntry("orderMenu"),{separator:true},
    {text:"Свойства",callback:()=>request("source.properties",{uuid:row.uuid})},
    {text:"Фильтры",callback:()=>request("source.filters",{uuid:row.uuid})},
    {text:"Все действия источника…",callback:()=>request("native.command",{id:"source.context",...sourceArgs(row)})}
  ] : [
    actionEntry("actionAddScene"),{text:"Дублировать",callback:()=>request("native.command",{id:"scene.duplicate"})},
    {text:"Переименовать",callback:()=>rename("scene",row)},actionEntry("actionRemoveScene"),
    {separator:true},actionEntry("actionSceneUp"),actionEntry("actionSceneDown"),actionEntry("actionSceneFilters"),
    {text:"Все действия сцены…",callback:()=>request("native.command",{id:"scene.context"})}
  ];
  showContext(event,items.filter(Boolean));
}
function renderRows(element,rows,sources=false) {
  const signature=JSON.stringify([rows,state.currentScene]);
  if(element.dataset.signature===signature)return;element.dataset.signature=signature;
  const focused=element.contains(document.activeElement)?document.activeElement.closest(".row")?.dataset.id:null;
  const scroll=element.scrollTop,nodes=[];
  for(const row of rows) {
    const identity=sources?keyOf(row):row.uuid;const entry=document.createElement("div");entry.className="row";entry.tabIndex=0;
    entry.dataset.id=identity;entry.setAttribute("role","option");entry.setAttribute("aria-selected",String(sources?row.selected:row.uuid===state.currentScene));
    entry.draggable=true;entry.style.paddingLeft=(5+(row.depth||0)*16)+"px";
    if(sources&&row.group){const expand=button(row.collapsed?"▸":"▾",e=>{e.stopPropagation();request("source.expand",{...sourceArgs(row),value:!!row.collapsed});});expand.className="expand";expand.setAttribute("aria-label",row.collapsed?"Развернуть группу":"Свернуть группу");entry.append(expand);}
    const type=document.createElement("span");type.className="type-icon";
    if(sources&&row.icon?.startsWith("data:image/png;base64,")){const image=document.createElement("img");image.src=row.icon;image.width=image.height=16;image.alt="";type.append(image);}else type.append(icon(row.group?"folder":sources?"image":"monitor"));
    const name=document.createElement("span");name.className="name";name.textContent=row.name;entry.append(type,name);
    if(sources) {
      const visible=iconButton("eye",row.visible?"Скрыть источник":"Показать источник",e=>{e.stopPropagation();request("source.visibility",{...sourceArgs(row),value:!row.visible});});
      visible.className="flag"+(row.visible?"":" off");visible.setAttribute("aria-pressed",String(row.visible));
      const locked=iconButton("lock",row.locked?"Разблокировать источник":"Заблокировать источник",e=>{e.stopPropagation();request("source.lock",{...sourceArgs(row),value:!row.locked});});
      locked.className="flag"+(row.locked?"":" off");locked.setAttribute("aria-pressed",String(row.locked));entry.append(visible,locked);
    }
    const select=event=>sources?selectSource(row,event):request("scene.select",{uuid:row.uuid});
    entry.addEventListener("click",event=>{if(!event.target.closest("button"))select(event);});
    entry.addEventListener("dblclick",event=>{if(sources&&!event.target.closest("button"))request("source.properties",{uuid:row.uuid});});
    entry.addEventListener("keydown",event=>{
      if(event.target!==entry)return;
      if(event.key==="Enter"||event.key===" "){event.preventDefault();select(event);}
      else if(event.key==="F2"){event.preventDefault();rename(sources?"source":"scene",row);}
      else if(event.key==="Delete"){event.preventDefault();invokeName(sources?"actionRemoveSource":"actionRemoveScene");}
      else if(event.key==="ArrowUp"||event.key==="ArrowDown"){
        event.preventDefault();const next=event.key==="ArrowUp"?entry.previousElementSibling:entry.nextElementSibling;next?.focus();next?.click();
      }
    });
    entry.addEventListener("contextmenu",async event=>{
      event.preventDefault();const position={preventDefault(){},clientX:event.clientX,clientY:event.clientY};
      if(!(sources?row.selected:row.uuid===state.currentScene))await select({});
      rowContext(position,row,sources);
    });
    entry.addEventListener("dragstart",event=>{dragged={row,sources,scene:state.currentScene};event.dataTransfer.effectAllowed="move";event.dataTransfer.setData("text/plain",identity);});
    entry.addEventListener("dragover",event=>{if(dragged?.sources===sources){event.preventDefault();entry.classList.add("drag-over");}});
    entry.addEventListener("dragleave",()=>entry.classList.remove("drag-over"));
    entry.addEventListener("drop",async event=>{
      event.preventDefault();entry.classList.remove("drag-over");if(!dragged||dragged.sources!==sources)return;
      const from=dragged;dragged=null;
      if(sources){if(from.scene!==state.currentScene)return;if(!state.sources.find(s=>keyOf(s)===keyOf(from.row))?.selected)await selectSource(from.row,{});request("source.move",{...sourceArgs(from.row),target:sourceArgs(row),position:row.group&&event.altKey?"inside":event.offsetY>entry.clientHeight/2?"after":"before"});}
      else{await request("scene.select",{uuid:from.row.uuid});request("scene.move",{uuid:from.row.uuid,target:row.uuid});}
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
      let pending,flight=false,pointerEditing=false;
      const volumeBusy=()=>pointerEditing||flight||pending!==undefined;
      const endVolumeEdit=()=>{
        pointerEditing=false;
        const current=state?.audio.find(item=>item.uuid===channel.uuid);
        if(current&&!volumeBusy()){volume.value=current.volume;fader.style.setProperty("--volume",Number(volume.value)*100+"%");}
      };
      volume.addEventListener("pointerdown",event=>{if(event.button===0){pointerEditing=true;volume.setPointerCapture(event.pointerId);}});
      for(const event of ["pointerup","pointercancel","lostpointercapture"])volume.addEventListener(event,endVolumeEdit);
      const sendVolume=async()=>{if(flight)return;flight=true;while(pending!==undefined){const value=pending;pending=undefined;await request("audio.volume",{uuid:channel.uuid,value});}flight=false;};
      volume.addEventListener("input",()=>{fader.style.setProperty("--volume",Number(volume.value)*100+"%");pending=Number(volume.value);sendVolume();});
      const mute=iconButton("volume","Заглушить",()=>{const current=state.audio.find(c=>c.uuid===channel.uuid);if(current)request("audio.mute",{uuid:channel.uuid,value:!current.muted});});mute.className="audio-mute";
      const monitor=iconButton("headphones","Включить мониторинг",()=>{const current=state.audio.find(c=>c.uuid===channel.uuid);if(current)request("audio.monitor",{uuid:channel.uuid,value:current.monitoring?0:2});});monitor.className="audio-monitor";
      buttons.append(mute,monitor);controls.append(body,buttons);container.append(title,controls);
      container.addEventListener("contextmenu",event=>{event.preventDefault();context();});
      entry={container,name,nameText,category,db,volume,volumeBusy,mute,monitor,meters,fader,ticks};audioElements.set(channel.uuid,entry);
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
    if(entry.meters.childElementCount!==count){entry.meters.replaceChildren();for(let n=0;n<count;n++){const bar=document.createElement("div");bar.className="meter";const peak=document.createElement("span");peak.className="meter-peak";bar.append(peak);entry.meters.append(bar);}}
    entry.meters.style.setProperty("--channels",String(count));
    entry.container.style.setProperty("--channel-width",Math.max(70,Math.min(110,channel.preferredWidth||88))+"px");
    for(const [key,variable] of [["categoryColor","category-text"],["categoryBackground","category-bg"]])if(/^#[0-9a-f]{6}$/i.test(channel[key]||""))entry.container.style.setProperty("--"+variable,channel[key]);
    const disabledColors=channel.muted||channel.active===false||(channel.unassigned&&!channel.monitoring);
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
  for(const [uuid,values]of Object.entries(levels||{})){
    const entry=audioElements.get(uuid);if(!entry)continue;
    entry.meters.querySelectorAll(".meter-peak").forEach((peak,index)=>{const db=values[index]??-100;const value=Math.max(0,Math.min(1,(db+60)/60));peak.style.setProperty("--peak-empty",(100-value*100)+"%");});
  }
}
function renderTransitions(next) {
  const select=$("transition-type"),signature=JSON.stringify(next.transitions||[]);
  if(select.dataset.signature!==signature){select.dataset.signature=signature;select.replaceChildren(...(next.transitions||[]).map(value=>{const option=document.createElement("option");option.value=value.uuid;option.textContent=value.name;return option;}));}
  select.value=next.currentTransition||"";select.disabled=next.transitionEnabled===false;
  if(document.activeElement!==$("transition-duration"))$("transition-duration").value=next.transitionDuration??300;
  $("transition-duration-label").hidden=!!next.transitionFixed;
  const target=$("transition-toolbar"),buttons=next.transitionControls||[];const sig=JSON.stringify(buttons);
  if(target.dataset.signature!==sig){target.dataset.signature=sig;target.replaceChildren(...buttons.map(c=>iconButton(toolbarSymbol(c.name||c.id),c.text,()=>request("control.click",{id:c.id}),c.enabled)));}
  $("program-column").hidden=$("studio-controls").hidden=!next.studioMode;
  $("preview-mode").textContent=next.studioMode?"Предпросмотр":"";
  $("program-title").textContent=next.programName||"";
  const quick=next.quickTransitions||[],quickSignature=JSON.stringify([quick,next.addQuickTransition]);
  if($("quick-transitions").dataset.signature!==quickSignature){
    $("quick-transitions").dataset.signature=quickSignature;
    const nodes=quick.map(c=>{
      const row=document.createElement("div");row.className="quick-row";
      row.append(button(c.text,()=>request("control.click",{id:c.id}),c.enabled),iconButton("dots","Настроить "+clean(c.text),()=>request("native.command",{id:"studio.quick.options",button:c.id}),c.enabled));
      return row;
    });
    if(next.addQuickTransition)nodes.push(button("+ Быстрый переход",()=>request("control.click",{id:next.addQuickTransition})));
    $("quick-transitions").replaceChildren(...nodes);
  }
  if(document.activeElement!==$("tbar"))$("tbar").value=next.tbar??0;
}
function renderControls(controls) {
  const signature=JSON.stringify(controls||[]);if($("controls").dataset.signature===signature)return;
  $("controls").dataset.signature=signature;const focus=document.activeElement?.dataset.control;
  const nodes=(controls||[]).map(control=>{const b=button(control.text,()=>request("control.click",{id:control.id}),control.enabled);b.dataset.control=control.id;if(control.checkable)b.setAttribute("aria-pressed",String(control.checked));return b;});
  nodes.push(button("Выход",()=>request("native.command",{id:"window.close"})));$("controls").replaceChildren(...nodes);
  if(focus)Array.from($("controls").children).find(c=>c.dataset.control===focus)?.focus({preventScroll:true});
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
  renderMenus(next.menus);indexActions(next.actions);
  renderToolbar("scene-toolbar",next.sceneToolbar);renderToolbar("source-toolbar",next.sourceToolbar);
  renderRows($("scenes"),next.scenes);renderRows($("sources"),next.sources,true);
  const selection=next.sources.filter(s=>s.selected),selected=selection.length===1?selection[0]:null;
  $("selected-name").textContent=selected?.name||(selection.length?selection.length+" источников выбрано":"Источник не выбран");
  $("properties").disabled=$("filters").disabled=!selected;$("interact").hidden=!selected?.interactive;
  $("source-tools").hidden=!next.sourceTools;
  renderMixer(next.audio||[]);renderTransitions(next);renderControls(next.controls);
  renderWorkspace(next);
  scheduleBounds();
}
function renderWorkspace(next) {
  for(const panel of $("panels").children){
    const dock=(next.docks||[]).find(item=>item.name===panel.dataset.panel);
    panel.hidden=panelMode?panel.dataset.panel!==panelMode:!!dock&&(!dock.visible||dock.floating);
  }
  const preferences=next.workspace||{};
  $("scene-toolbar").hidden=$("source-toolbar").hidden=preferences.toggleListboxToolbars===false;
  $("context-bar").hidden=preferences.toggleContextBar===false;
  document.querySelector("footer").hidden=preferences.toggleStatusBar===false;
  document.body.classList.toggle("no-source-icons",preferences.toggleSourceIcons===false);
  $("scenes").classList.toggle("scene-grid",!!preferences.actionSceneGridMode);
  $("mixer").classList.toggle("vertical-mixer",!!preferences.verticalMixer);
  const panels=Array.from($("panels").children).filter(panel=>!panel.hidden);
  $("panels").style.gridTemplateColumns=panels.map(panel=>"minmax(0,"+panelWeights[panel.dataset.panel]+"fr)").join(" ");
  $("panels").hidden=$("panel-resizer").hidden=!panels.length;
  for(const panel of $("panels").children){
    const dock=(next.docks||[]).find(item=>item.name===panel.dataset.panel);
    panel.querySelector("h2").draggable=false;
    panel.querySelector("h2").classList.toggle("movable",!preferences.lockDocks&&!panelMode);
    panel.querySelector(".panel-width-handle").hidden=!!panelMode||!!preferences.lockDocks||panel===panels.at(-1);
    const toggle=panel.querySelector(".panel-float");
    toggle.disabled=!!preferences.lockDocks||dock?.floatable===false;
    const label=panelMode?"Вернуть панель в основное окно":"Отделить панель";
    toggle.setAttribute("aria-label",label+": "+panel.querySelector("h2").textContent);
    toggle.title=label;toggle.textContent=panelMode?"↙":"↗";
  }
}
function scheduleBounds() {
  if(panelMode)return;
  if(boundsFrame)return;
  boundsFrame=requestAnimationFrame(async()=>{
    boundsFrame=0;if(!connected)return;
    const overlayElements=[...document.querySelectorAll(".menu[open] > .menu-popover, .panel-drag-ghost"),...(!$("context-menu").hidden?[$("context-menu")]:[]),...($("rename-dialog").open?[$("rename-dialog")]:[])];
    const overlays=overlayElements.map(element=>{
      const rect=element.getBoundingClientRect(),x=Math.max(0,rect.left),y=Math.max(0,rect.top);
      return {x,y,width:Math.max(0,Math.min(innerWidth,rect.right)-x),height:Math.max(0,Math.min(innerHeight,rect.bottom)-y)};
    }).filter(rect=>rect.width&&rect.height);
    for(const target of ["preview","program"]){
      const area=$(target),bounds=area.getBoundingClientRect(),visible=target==="preview"||state.studioMode;
      const args={target,x:bounds.x,y:bounds.y,width:bounds.width,height:bounds.height,viewportWidth:innerWidth,viewportHeight:innerHeight,visible,overlays,modal:$("rename-dialog").open};
      if(!bounds.width||!bounds.height){args.x=args.y=args.width=args.height=0;}
      const signature=JSON.stringify(args);if(boundsSignatures.get(target)===signature)continue;
      const result=await request("preview.bounds",args);if(result!==undefined)boundsSignatures.set(target,signature);
    }
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
$("tbar").addEventListener("input",()=>request("studio.tbar",{value:Number($("tbar").value)}));
$("tbar").addEventListener("change",()=>request("studio.tbar",{value:Number($("tbar").value),release:true}));
$("zoom-in").addEventListener("click",()=>invokeName("actionPreviewZoomIn"));$("zoom-out").addEventListener("click",()=>invokeName("actionPreviewZoomOut"));
$("preview-scaling").addEventListener("change",()=>invokeName($("preview-scaling").value));
$("rename-dialog").addEventListener("close",()=>{scheduleBounds();if($("rename-dialog").returnValue!=="ok"||!renameTarget)return;
  const {kind,row}=renameTarget;renameTarget=null;
  request(kind+".rename",{...(kind==="source"?sourceArgs(row):{uuid:row.uuid}),name:$("rename-value").value});
});
$("scenes").addEventListener("contextmenu",event=>{if(!event.target.closest(".row"))showContext(event,[actionEntry("actionAddScene")].filter(Boolean));});
$("sources").addEventListener("contextmenu",event=>{if(!event.target.closest(".row"))showContext(event,[actionEntry("actionAddSource"),actionEntry("actionPasteRef"),{text:"Добавить группу",callback:()=>request("native.command",{id:"source.addGroup"})}].filter(Boolean));});
document.addEventListener("click",event=>{if(!event.target.closest(".menu,.context-menu,.audio-controls,.row"))closeMenus();});
document.addEventListener("keydown",event=>{
  if(event.defaultPrevented)return;
  if(event.key==="Escape"){closeMenus();return;}
  if(event.target.closest("input,select,textarea,dialog"))return;
  const keyNames={Delete:"del",Backspace:"backspace",Insert:"ins",PageUp:"pgup",PageDown:"pgdown"," ":"space"};
  const keys=[event.ctrlKey?"ctrl":event.metaKey?"meta":"",event.altKey?"alt":"",event.shiftKey?"shift":"",keyNames[event.key]||event.key.toLowerCase().replace("arrow","")].filter(Boolean).join("+");
  const action=[...actionsByName.values()].find(a=>a.enabled&&a.shortcutGlobal!==false&&(a.shortcutKey||a.shortcut)?.toLowerCase().replace(/ /g,"")===keys);
  if(action){event.preventDefault();request("action.invoke",{id:action.id});}
});
const resizer=$("panel-resizer");let sizing=false;
function savePanelLayout(){try{localStorage.setItem("obs.panels.order",JSON.stringify(Array.from($("panels").children,p=>p.dataset.panel)));localStorage.setItem("obs.panels.weights",JSON.stringify(panelWeights));}catch{}}
function resetPanelLayout(){panelWeights={...defaultPanelWeights};for(const id of defaultPanelOrder)$("panels").append(document.querySelector('[data-panel="'+id+'"]'));document.documentElement.style.removeProperty("--panel-height");try{localStorage.removeItem("obs.panels.height");}catch{}savePanelLayout();if(state)renderWorkspace(state);scheduleBounds();}
try{
  const order=JSON.parse(localStorage.getItem("obs.panels.order")||"null"),weights=JSON.parse(localStorage.getItem("obs.panels.weights")||"null");
  if(Array.isArray(order)&&order.length===defaultPanelOrder.length&&new Set(order).size===order.length&&order.every(id=>defaultPanelOrder.includes(id)))for(const id of order)$("panels").append(document.querySelector('[data-panel="'+id+'"]'));
  if(weights)for(const id of defaultPanelOrder)if(Number.isFinite(weights[id])&&weights[id]>=.15&&weights[id]<=10)panelWeights[id]=weights[id];
}catch{}
for(const panel of $("panels").children){
  const heading=panel.querySelector("h2"),handle=document.createElement("div");handle.className="panel-width-handle";handle.tabIndex=0;handle.setAttribute("role","separator");handle.setAttribute("aria-label","Ширина панели "+heading.textContent);handle.setAttribute("aria-orientation","vertical");panel.append(handle);
  heading.title="Перетащите по панелям для перестановки, за их пределы — в отдельное окно";
  heading.tabIndex=0;
  const toggleDock=(placement={})=>{const rect=panel.getBoundingClientRect();return request(panelMode?"dock.attach":"dock.detach",{name:panel.dataset.panel,width:rect.width,height:rect.height,viewportWidth:innerWidth,offsetX:placement.offsetX??36,offsetY:placement.offsetY??16});};
  const floatButton=button("↗",()=>toggleDock());floatButton.className="panel-float";floatButton.setAttribute("aria-label","Отделить панель: "+heading.textContent);panel.append(floatButton);
  const clearMove=()=>{
    if(movingPanel?.heading!==heading)return;
    movingPanel.ghost?.remove();movingPanel=null;panel.classList.remove("panel-dragging");
    document.querySelectorAll(".panel-drop-before,.panel-drop-after").forEach(item=>item.classList.remove("panel-drop-before","panel-drop-after"));
    scheduleBounds();
  };
  heading.addEventListener("pointerdown",event=>{
    if(event.button!==0||panelMode||state?.workspace?.lockDocks)return;
    event.preventDefault();heading.focus({preventScroll:true});
    const rect=panel.getBoundingClientRect();
    movingPanel={panel,heading,x:event.clientX,y:event.clientY,offsetX:event.clientX-rect.left,offsetY:event.clientY-rect.top,pointer:event.pointerId,active:false,target:null};
    heading.setPointerCapture(event.pointerId);
  });
  heading.addEventListener("pointermove",event=>{
    const move=movingPanel;if(!move||move.heading!==heading)return;
    if(state?.workspace?.lockDocks){clearMove();return;}
    if(!move.active&&Math.hypot(event.clientX-move.x,event.clientY-move.y)<6)return;
    if(!move.active){
      move.active=true;const rect=panel.getBoundingClientRect();move.ghost=panel.cloneNode(true);move.ghost.classList.add("panel-drag-ghost");
      move.ghost.removeAttribute("data-panel");move.ghost.inert=true;move.ghost.setAttribute("aria-hidden","true");
      for(const item of move.ghost.querySelectorAll("[id]"))item.removeAttribute("id");
      move.ghost.querySelector(".panel-width-handle")?.remove();move.ghost.style.width=rect.width+"px";move.ghost.style.height=rect.height+"px";
      panel.classList.add("panel-dragging");document.body.append(move.ghost);
    }
    move.ghost.style.left=(event.clientX-move.offsetX)+"px";move.ghost.style.top=(event.clientY-move.offsetY)+"px";
    document.querySelectorAll(".panel-drop-before,.panel-drop-after").forEach(item=>item.classList.remove("panel-drop-before","panel-drop-after"));
    const area=$("panels").getBoundingClientRect();
    move.inside=event.clientX>=area.left&&event.clientX<=area.right&&event.clientY>=area.top&&event.clientY<=area.bottom;
    move.target=null;
    if(move.inside){
      const candidates=Array.from($("panels").children).filter(item=>!item.hidden&&item!==panel);
      move.target=candidates.find(item=>event.clientX<item.getBoundingClientRect().right)||candidates.at(-1);
      if(move.target){const target=move.target.getBoundingClientRect();move.after=event.clientX>=target.left+target.width/2;move.target.classList.add(move.after?"panel-drop-after":"panel-drop-before");}
    }
    move.ghost.classList.toggle("detaching",!move.inside);scheduleBounds();
  });
  heading.addEventListener("pointerup",event=>{
    const move=movingPanel;if(!move||move.heading!==heading)return;
    if(move.active&&!state?.workspace?.lockDocks){
      if(move.inside&&move.target){move.target[move.after?"after":"before"](panel);savePanelLayout();renderWorkspace(state);scheduleBounds();}
      else if(!move.inside)toggleDock(move);
    }
    clearMove();
    if(heading.hasPointerCapture(event.pointerId))heading.releasePointerCapture(event.pointerId);
  });
  heading.addEventListener("pointercancel",clearMove);
  heading.addEventListener("lostpointercapture",()=>{if(movingPanel?.heading===heading)clearMove();});
  heading.addEventListener("dblclick",()=>{if(!state?.workspace?.lockDocks)toggleDock();});
  heading.addEventListener("keydown",event=>{
    if(state?.workspace?.lockDocks)return;
    if(event.key==="Escape"){clearMove();return;}
    if(event.key==="Enter"){event.preventDefault();toggleDock();return;}
    if(panelMode||!["ArrowLeft","ArrowRight"].includes(event.key))return;
    event.preventDefault();const list=Array.from($("panels").children).filter(item=>!item.hidden),index=list.indexOf(panel),target=list[index+(event.key==="ArrowLeft"?-1:1)];
    if(target){target[event.key==="ArrowLeft"?"before":"after"](panel);savePanelLayout();renderWorkspace(state);scheduleBounds();}
  });
  heading.addEventListener("contextmenu",event=>{const dock=state?.docks?.find(d=>d.name===panel.dataset.panel);showContext(event,[{text:panelMode?"Вернуть панель в основное окно":"Отделить панель",enabled:!state?.workspace?.lockDocks,callback:toggleDock},...(dock?[{text:"Скрыть панель",callback:()=>request("action.invoke",{id:dock.action})}]:[]),{text:"Сбросить расположение панелей",enabled:!state?.workspace?.lockDocks,callback:resetPanelLayout}]);});
  let resizing;
  const resizeWidth=delta=>{if(!resizing)return;const {left,right,width,first,second}=resizing;const shift=delta/width*(first+second),a=Math.max(.15,Math.min(first+second-.15,first+shift));panelWeights[left]=a;panelWeights[right]=first+second-a;renderWorkspace(state);};
  handle.addEventListener("pointerdown",event=>{if(event.button!==0||panelMode||state?.workspace?.lockDocks)return;const next=Array.from($("panels").children).slice(Array.from($("panels").children).indexOf(panel)+1).find(p=>!p.hidden);if(!next)return;resizing={left:panel.dataset.panel,right:next.dataset.panel,x:event.clientX,width:panel.offsetWidth+next.offsetWidth,first:panelWeights[panel.dataset.panel],second:panelWeights[next.dataset.panel]};handle.setPointerCapture(event.pointerId);});
  handle.addEventListener("pointermove",event=>resizeWidth(event.clientX-(resizing?.x||event.clientX)));
  handle.addEventListener("pointerup",()=>{resizing=null;savePanelLayout();});
  handle.addEventListener("keydown",event=>{if(!["ArrowLeft","ArrowRight"].includes(event.key)||state?.workspace?.lockDocks)return;event.preventDefault();const list=Array.from($("panels").children).filter(p=>!p.hidden),next=list[list.indexOf(panel)+1];if(!next)return;resizing={left:panel.dataset.panel,right:next.dataset.panel,width:panel.offsetWidth+next.offsetWidth,first:panelWeights[panel.dataset.panel],second:panelWeights[next.dataset.panel]};resizeWidth(event.key==="ArrowLeft"?-15:15);resizing=null;savePanelLayout();});
}
try{const saved=Number(localStorage.getItem("obs.panels.height"));if(saved>=195&&saved<=900)document.documentElement.style.setProperty("--panel-height",saved+"px");}catch{}
resizer.addEventListener("pointerdown",event=>{sizing=true;resizer.setPointerCapture(event.pointerId);});
resizer.addEventListener("pointermove",event=>{if(sizing){const height=Math.max(195,Math.min(innerHeight*.65,innerHeight-event.clientY-28));document.documentElement.style.setProperty("--panel-height",height+"px");scheduleBounds();}});
resizer.addEventListener("pointerup",()=>{sizing=false;try{localStorage.setItem("obs.panels.height",String($("panels").offsetHeight));}catch{}});
resizer.addEventListener("keydown",event=>{if(!["ArrowUp","ArrowDown"].includes(event.key))return;event.preventDefault();document.documentElement.style.setProperty("--panel-height",Math.max(195,$("panels").offsetHeight+(event.key==="ArrowUp"?10:-10))+"px");scheduleBounds();});
document.addEventListener("toggle",scheduleBounds,true);
new ResizeObserver(scheduleBounds).observe($("preview"));new ResizeObserver(scheduleBounds).observe($("program"));
window.addEventListener("resize",scheduleBounds);window.addEventListener("pagehide",()=>bridge?.dispose());
try{bridge=createBridge(window.chrome?.webview);bridge.subscribe("state.changed",render);bridge.subscribe("audio.levels",updateLevels);bridge.subscribe("viewport.invalidate",()=>{boundsSignatures.clear();scheduleBounds();});bridge.subscribe("workspace.reset",resetPanelLayout);bridge.subscribe("overlays.dismiss",closeMenus);bridge.subscribe("workspace.panel",data=>{if(!defaultPanelOrder.includes(data?.name))return;panelMode=data.name;document.body.classList.add("floating-panel");if(state)renderWorkspace(state);});}
catch(error){showError(error);$("connection").textContent="Нет соединения с OBS";}
if(bridge)installExternalDrop(document,{onError:showError});
