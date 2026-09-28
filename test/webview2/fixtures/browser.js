(() => {
  const events = new Set();
  const state = {
    title: "OBS — browser fixture", context: "fixture-context",
    scenes: [{ uuid: "scene-1", name: "Основная сцена" }, { uuid: "scene-2", name: "Пауза" }, { uuid: "scene-3", name: "Камера" }],
    currentScene: "scene-1",
    sources: [{ id: "1", uuid: "source-1", name: "Захват игры", visible: true, locked: true, selected: true }, { id: "2", uuid: "source-2", name: "Камера", visible: true, locked: false, selected: false }],
    controls: [{ id: "streamButton", text: "Начать трансляцию", enabled: true, checked: false }, { id: "recordButton", text: "Начать запись", enabled: true, checked: false }, { id: "virtualCamButton", text: "Запустить виртуальную камеру", enabled: true, checked: false }, { id: "modeSwitch", text: "Режим студии", enabled: true, checked: false }, { id: "settingsButton", text: "Настройки", enabled: true, checked: false }],
    menus: [{ id: "m1", text: "Файл", children: [{ id: "a1", text: "Показать записи", enabled: true, shortcut: "" }, { id: "a2", text: "Ремультиплексирование записей", enabled: true, shortcut: "" }, { separator: true }, { id: "a3", text: "Настройки", enabled: true, shortcut: "" }] }, { id: "m2", text: "Правка", children: [{ id: "a4", text: "Отменить", enabled: false, shortcut: "Ctrl+Z" }] }, { id: "m3", text: "Вид", children: [{ id: "a5", text: "Статистика", enabled: true, shortcut: "" }] }, { id: "m4", text: "Профиль", children: [{ id: "a6", text: "По умолчанию", enabled: true, checkable: true, checked: true }] }, { id: "m5", text: "Коллекция сцен", children: [] }, { id: "m6", text: "Инструменты", children: [] }, { id: "m7", text: "Справка", children: [] }],
    labels: { "Basic.Main.Scenes": "Сцены", "Basic.Main.Sources": "Источники", "Basic.Main.Controls": "Управление" },
    studioMode: false, recording: false, streaming: false, paused: false, fps: 60,
  };
  function send(data) { for (const callback of events) callback({ data: structuredClone(data) }); }
  function publish() { send({ version: 1, event: "state.changed", data: state }); }
  window.chrome ??= {};
  window.chrome.webview = {
    addEventListener(type, callback) { events.add(callback); },
    removeEventListener(type, callback) { events.delete(callback); },
    postMessage(message) {
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
