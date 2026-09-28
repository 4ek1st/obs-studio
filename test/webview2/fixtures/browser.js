(() => {
  const events = new Set();
  let undoAudio, redoAudio, selectionAnchor;
  const state = {
    title: "OBS — browser fixture", context: "fixture-context",
    scenes: [{ uuid: "scene-1", name: "Основная сцена" }, { uuid: "scene-2", name: "Пауза" }, { uuid: "scene-3", name: "Камера" }],
    currentScene: "scene-1",
    sources: [{ id: "1", uuid: "source-1", name: "Захват игры", visible: true, locked: true, selected: true }, { id: "2", uuid: "source-2", name: "Камера", visible: true, locked: false, selected: false }],
    controls: [{ id: "streamButton", text: "Начать трансляцию", enabled: true, checked: false }, { id: "recordButton", text: "Начать запись", enabled: true, checked: false }, { id: "virtualCamButton", text: "Запустить виртуальную камеру", enabled: true, checked: false }, { id: "virtualCamConfigButton", text: "Настройки виртуальной камеры", enabled: true, checked: false }, { id: "modeSwitch", text: "Режим студии", enabled: true, checked: false }, { id: "settingsButton", text: "Настройки", enabled: true, checked: false }],
    menus: [{ id: "m1", text: "Файл", children: [{ id: "a1", text: "Показать записи", enabled: true, shortcut: "" }, { id: "a2", text: "Ремультиплексирование записей", enabled: true, shortcut: "" }, { separator: true }, { id: "a3", text: "Настройки", enabled: true, shortcut: "" }] }, { id: "m2", text: "Правка", children: [{ id: "a4", text: "Отменить", enabled: false, shortcut: "Ctrl+Z" }] }, { id: "m3", text: "Вид", children: [{ id: "a5", text: "Статистика", enabled: true, shortcut: "" }] }, { id: "m4", text: "Профиль", children: [{ id: "a6", text: "По умолчанию", enabled: true, checkable: true, checked: true }] }, { id: "m5", text: "Коллекция сцен", children: [] }, { id: "m6", text: "Инструменты", children: [] }, { id: "m7", text: "Справка", children: [] }],
    labels: { "Basic.Main.Scenes": "Сцены", "Basic.Main.Sources": "Источники", "Basic.Main.Controls": "Управление" },
    studioMode: false, recording: false, streaming: false, paused: false, fps: 60,
  };
  const action = (name, text, enabled=true) => ({id:name,name,text,enabled});
  const undoAction = {...action("actionMainUndo", "Отменить", false), shortcut:"Ctrl+Z", shortcutKey:"Ctrl+Z", shortcutKeys:["Ctrl+Z"], shortcutGlobal:true};
  const redoAction = {...action("actionMainRedo", "Повторить", false), shortcut:"Ctrl+Shift+Z", shortcutKey:"Ctrl+Shift+Z", shortcutKeys:["Ctrl+Shift+Z","Ctrl+Y"], shortcutGlobal:true};
  state.menus.find(menu=>menu.id==="m2").children=[undoAction,redoAction];
  state.sceneToolbar=[action("actionAddScene","Добавить сцену"),action("actionRemoveScene","Удалить сцену"),{separator:true},action("actionSceneFilters","Фильтры сцены"),action("actionSceneUp","Переместить вверх"),action("actionSceneDown","Переместить вниз")];
  state.sourceToolbar=[action("actionAddSource","Добавить источник"),action("actionRemoveSource","Удалить источник"),{separator:true},action("actionSourceProperties","Свойства"),action("actionSourceUp","Переместить вверх"),action("actionSourceDown","Переместить вниз")];
  state.actions=[undoAction,redoAction,action("actionAdvAudioProperties","Расширенные свойства аудио"),action("actionMixerToolbarToggleLayout","Изменить ориентацию микшера")];
  state.workspace={verticalMixer:new URLSearchParams(location.search).get("mixer")==="vertical",lockDocks:false};
  state.workspace.actionSceneGridMode=new URLSearchParams(location.search).has("sceneGrid");
  state.sceneGrid={width:154,height:24};
  if(new URLSearchParams(location.search).has("sourceGroup"))state.sources.push({id:"3",uuid:"group-1",name:"Группа",group:true,collapsed:true,visible:true,locked:false,selected:false});
  state.previewControls={enabled:!new URLSearchParams(location.search).has("previewDisabled"),index:0,percent:"58%",
    options:[{index:0,text:"По размеру окна"},{index:1,text:"Холст (1920x1080)"},{index:2,text:"Вывод (1280x720)"}],
    enableText:"Включить предпросмотр",previewXScrollBar:{min:0,max:0,value:0,page:100},previewYScrollBar:{min:0,max:0,value:0,page:100}};
  state.mixerToolbar={hidden:{id:"mixer-hidden",text:"Скрыто: 0",enabled:false,checked:false},optionsText:"Параметры",layoutAction:"actionMixerToolbarToggleLayout"};
  state.sources.forEach(x=>{x.owner="scene-1";x.depth=0;});
  state.audio=[{uuid:"audio-1",name:"Звук рабочего стола",volume:.85,db:-3.2,muted:false,monitoring:0,enabled:true,volumeEnabled:true,visible:true},{uuid:"audio-2",name:"Микрофон",volume:.7,db:-6.5,muted:false,monitoring:0,enabled:true,volumeEnabled:true,visible:true}];
  state.audio.forEach((channel,index)=>{channel.category=index?"Активен":"Глобальный";channel.global=!index;channel.active=true;channel.channels=2;channel.monitoringAvailable=true;});
  state.transitions=[{uuid:"fade",name:"Затухание"},{uuid:"cut",name:"Обрезка"}];state.currentTransition="fade";state.transitionDuration=300;
  state.transitionControls=[{id:"transitionAdd",text:"Добавить переход",enabled:true},{id:"transitionRemove",text:"Удалить переход",enabled:false},{id:"transitionProps",text:"Свойства перехода",enabled:false}];
  state.quickTransitions=[{id:"quick-cut",text:"Обрезка",enabled:true},{id:"quick-fade",text:"Затухание (300 мс)",enabled:true}];state.nativeEditor=true;state.cpu=1.6;
  function send(data) { for (const callback of events) callback({ data: structuredClone(data) }); }
  function syncHistoryActions() { undoAction.enabled=!!undoAudio; redoAction.enabled=!!redoAudio; }
  function publish() { syncHistoryActions(); if(document.body)document.body.dataset.previewEnabled=String(state.previewControls.enabled); send({ version: 1, event: "state.changed", data: state }); }
  window.chrome ??= {};
  window.chrome.webview = {
    addEventListener(type, callback) { events.add(callback); },
    removeEventListener(type, callback) { events.delete(callback); },
    postMessage(message) {
      if(document.body&&message.command!=="preview.bounds"){
        document.body.dataset.lastCommand=message.command;
        document.body.dataset.lastArgs=JSON.stringify(message.args??{});
      }
      if(message.command==="preview.enable")state.previewControls.enabled=true;
      if(message.command==="preview.scale"){
        const c=state.previewControls;c.index=message.args.index;c.percent=c.index===0?"58%":c.index===1?"100%":"66%";
        c.previewXScrollBar={min:c.index?-500:0,max:c.index?500:0,value:0,page:1000};
        c.previewYScrollBar={min:c.index?-300:0,max:c.index?300:0,value:0,page:600};
      }
      if(message.command==="preview.scroll")state.previewControls[message.args.axis==="x"?"previewXScrollBar":"previewYScrollBar"].value=message.args.value;
      if(message.command==="source.visibility"||message.command==="source.lock"){const source=state.sources.find(x=>x.id===message.args.id);if(source)source[message.command==="source.lock"?"locked":"visible"]=message.args.value;}
      if(message.command==="source.rename"&&message.args.save!==false){const source=state.sources.find(x=>x.id===message.args.id);if(source)source.name=message.args.name;}
      if(message.command==="source.expand"){const source=state.sources.find(x=>x.id===message.args.id);if(source?.group)source.collapsed=!message.args.value;}
      if(message.command==="source.selectAll")state.sources.forEach(source=>source.selected=true);
      if(message.command==="source.hover"&&document.body)document.body.dataset.hoveredSource=message.args.value?message.args.uuid:"";
      if(message.command==="scene.activate"&&state.studioMode)state.programName=state.scenes.find(scene=>scene.uuid===message.args.uuid)?.name??"";
      if(message.command==="source.move"||message.command==="scene.move"){
        const sourceMode=message.command==="source.move",list=sourceMode?state.sources:state.scenes;
        const moved=sourceMode?list.filter(row=>row.selected):list.filter(row=>row.uuid===message.args.uuid);
        const target=sourceMode?list.find(row=>row.id===message.args.target?.id):list.find(row=>row.uuid===message.args.target);
        if(message.args.position!=="inside"&&moved.length&&!moved.includes(target)){
          const remaining=list.filter(row=>!moved.includes(row));let index=message.args.position==="end"?remaining.length:remaining.indexOf(target)+(message.args.position==="after"?1:0);
          if(index>=0){remaining.splice(index,0,...moved);if(sourceMode)state.sources=remaining;else state.scenes=remaining;}
        }
      }
      if(message.command==="scene.rename"){const scene=state.scenes.find(x=>x.uuid===message.args.uuid);if(scene)scene.name=message.args.name;}
      if(message.command.startsWith("audio.")){const source=state.audio.find(x=>x.uuid===message.args.uuid);if(source){if(message.command==="audio.volume"){undoAudio={uuid:source.uuid,volume:source.volume,db:source.db};redoAudio=undefined;source.volume=message.args.value;source.db=source.volume<=0?-100:source.volume>=1?0:6-102*Math.pow(17,-source.volume);}if(message.command==="audio.mute")source.muted=message.args.value;if(message.command==="audio.monitor")source.monitoring=message.args.value;}}
      // Keyboard handling belongs to production app.js. This fixture models
      // only the native action endpoint, so shortcut-routing bugs stay visible.
      if(message.command==="action.invoke"&&message.args.id==="actionMainUndo"&&undoAudio){
        const source=state.audio.find(channel=>channel.uuid===undoAudio.uuid);
        if(source){redoAudio={uuid:source.uuid,volume:source.volume,db:source.db};Object.assign(source,undoAudio);}
        undoAudio=undefined;
      }
      if(message.command==="action.invoke"&&message.args.id==="actionMainRedo"&&redoAudio){
        const source=state.audio.find(channel=>channel.uuid===redoAudio.uuid);
        if(source){undoAudio={uuid:source.uuid,volume:source.volume,db:source.db};Object.assign(source,redoAudio);}
        redoAudio=undefined;
      }
      syncHistoryActions();
      if(message.command==="action.invoke"&&message.args.id==="actionMixerToolbarToggleLayout")state.workspace.verticalMixer=!state.workspace.verticalMixer;
      if(message.command==="transition.select")state.currentTransition=message.args.uuid;
      if(message.command==="transition.duration")state.transitionDuration=message.args.value;
      if(message.command==="control.click"&&message.args.id==="modeSwitch")state.studioMode=!state.studioMode;
      if (message.command === "source.select") {
        const args=message.args,index=state.sources.findIndex(source=>source.id===args.id);
        if(index>=0){
          if(args.focusOnly)selectionAnchor=index;
          else if(args.range&&selectionAnchor!==undefined)state.sources.forEach((source,i)=>source.selected=(args.additive&&source.selected)||(i>=Math.min(index,selectionAnchor)&&i<=Math.max(index,selectionAnchor)));
          else{state.sources.forEach((source,i)=>source.selected=args.additive?(i===index?!source.selected:source.selected):i===index);selectionAnchor=index;}
        }
      }
      if (message.command === "menu.prepare") {
        const menu = state.menus.find(item => item.id === message.args.id);
        setTimeout(() => send({ version: 1, id: message.id, ok: true, result: menu?.children ?? [] }), 0);
        return;
      }
      if (message.command === "scene.select") state.currentScene = message.args.uuid;
      if (message.command === "control.click" && message.args.id === "recordButton") {
        state.recording = !state.recording;
        state.controls.find(item => item.id === "recordButton").text = state.recording ? "Остановить запись" : "Начать запись";
      }
      setTimeout(() => { send({ version: 1, id: message.id, ok: true, result: message.command === "state.get" ? state : {} }); publish(); }, 0);
    },
  };
  window.addEventListener("load", publish);
})();
