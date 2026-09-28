(() => {
  const events = new Set();
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
  state.sceneToolbar=[action("actionAddScene","Добавить сцену"),action("actionRemoveScene","Удалить сцену"),{separator:true},action("actionSceneFilters","Фильтры сцены"),action("actionSceneUp","Переместить вверх"),action("actionSceneDown","Переместить вниз")];
  state.sourceToolbar=[action("actionAddSource","Добавить источник"),action("actionRemoveSource","Удалить источник"),{separator:true},action("actionSourceProperties","Свойства"),action("actionSourceUp","Переместить вверх"),action("actionSourceDown","Переместить вниз")];
  state.actions=[action("actionAdvAudioProperties","Расширенные свойства аудио")];
  state.sources.forEach(x=>{x.owner="scene-1";x.depth=0;});
  state.audio=[{uuid:"audio-1",name:"Звук рабочего стола",volume:.85,db:-3.2,muted:false,monitoring:0,enabled:true,volumeEnabled:true,visible:true},{uuid:"audio-2",name:"Микрофон",volume:.7,db:-6.5,muted:false,monitoring:0,enabled:true,volumeEnabled:true,visible:true}];
  state.transitions=[{uuid:"fade",name:"Затухание"},{uuid:"cut",name:"Обрезка"}];state.currentTransition="fade";state.transitionDuration=300;
  state.transitionControls=[{id:"transitionAdd",text:"Добавить переход",enabled:true},{id:"transitionRemove",text:"Удалить переход",enabled:false},{id:"transitionProps",text:"Свойства перехода",enabled:false}];
  state.quickTransitions=[{id:"quick-cut",text:"Обрезка",enabled:true},{id:"quick-fade",text:"Затухание (300 мс)",enabled:true}];state.nativeEditor=true;state.cpu=1.6;
  function send(data) { for (const callback of events) callback({ data: structuredClone(data) }); }
  function publish() { send({ version: 1, event: "state.changed", data: state }); }
  window.chrome ??= {};
  window.chrome.webview = {
    addEventListener(type, callback) { events.add(callback); },
    removeEventListener(type, callback) { events.delete(callback); },
    postMessage(message) {
      if(message.command==="source.visibility"||message.command==="source.lock"){const source=state.sources.find(x=>x.id===message.args.id);if(source)source[message.command==="source.lock"?"locked":"visible"]=message.args.value;}
      if(message.command==="source.rename"){const source=state.sources.find(x=>x.id===message.args.id);if(source)source.name=message.args.name;}
      if(message.command==="scene.rename"){const scene=state.scenes.find(x=>x.uuid===message.args.uuid);if(scene)scene.name=message.args.name;}
      if(message.command.startsWith("audio.")){const source=state.audio.find(x=>x.uuid===message.args.uuid);if(source){if(message.command==="audio.volume")source.volume=message.args.value;if(message.command==="audio.mute")source.muted=message.args.value;if(message.command==="audio.monitor")source.monitoring=message.args.value;}}
      if(message.command==="transition.select")state.currentTransition=message.args.uuid;
      if(message.command==="transition.duration")state.transitionDuration=message.args.value;
      if(message.command==="control.click"&&message.args.id==="modeSwitch")state.studioMode=!state.studioMode;
      if (message.command === "source.select") state.sources.forEach(source => source.selected = source.id === message.args.id);
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
