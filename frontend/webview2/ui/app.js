import { createBridge } from "./bridge.mjs";

const $ = id => document.getElementById(id);
const clean = text => String(text ?? "").replace(/&&/g, "\u0000").replace(/&/g, "").replace(/\u0000/g, "&");
let bridge;
let state;
let selectedSource = "";
let menuSignature = "";
let boundsSignature = "";
let boundsFrame = 0;
let connected = false;

function showError(error) {
  $("error").textContent = error.message || String(error);
  $("error").hidden = false;
}
async function request(command, args = {}) {
  try {
    if (command === "action.invoke" || command === "control.click") args = { ...args, context: state?.context };
    const result = await bridge.request(command, args);
    $("error").hidden = true;
    return result;
  } catch (error) {
    showError(error);
    return undefined;
  }
}
function button(text, callback, enabled = true) {
  const element = document.createElement("button");
  element.type = "button";
  element.textContent = clean(text);
  element.disabled = !enabled;
  element.addEventListener("click", callback);
  return element;
}
function closeMenus() {
  document.querySelectorAll("details[open]").forEach(menu => menu.open = false);
  scheduleBounds();
}
function prepareMenu(menu, content, id) {
  let generation = 0;
  menu.addEventListener("toggle", async () => {
    const current = ++generation;
    if (!menu.open) return;
    const entries = await request("menu.prepare", { id });
    if (current !== generation || !menu.open || !Array.isArray(entries)) return;
    content.replaceChildren(...menuItems(entries).childNodes);
  });
}
function menuItems(items) {
  const content = document.createElement("div");
  for (const item of items) {
    if (item.separator) { content.append(document.createElement("hr")); continue; }
    if (item.children) {
      const nested = document.createElement("details");
      nested.className = "submenu";
      const title = document.createElement("summary");
      title.textContent = clean(item.text);
            const children = menuItems(item.children);
      nested.append(title, children);
      prepareMenu(nested, children, item.id);
      content.append(nested);
      continue;
    }
    const entry = button("", () => { closeMenus(); request("action.invoke", { id: item.id }); }, item.enabled);
    const name = document.createElement("span");
    name.textContent = (item.checked ? "✓ " : "") + clean(item.text);
    const shortcut = document.createElement("span");
    shortcut.className = "shortcut";
    shortcut.textContent = item.shortcut || "";
    entry.setAttribute("aria-label", clean(item.text));
    entry.append(name, shortcut);
    content.append(entry);
  }
  return content;
}
function renderMenus(menus) {
  const signature = JSON.stringify(menus);
  if (signature === menuSignature || document.querySelector(".menu[open]")) return;
  menuSignature = signature;
  const fragment = document.createDocumentFragment();
  for (const item of menus) {
    if (!item.children) continue;
    const menu = document.createElement("details");
    menu.className = "menu";
    const title = document.createElement("summary");
    title.textContent = clean(item.text);
    const list = menuItems(item.children);
    list.className = "menu-popover";
    menu.append(title, list);
    prepareMenu(menu, list, item.id);
    menu.addEventListener("toggle", () => {
      if (menu.open) document.querySelectorAll(".menu[open]").forEach(other => { if (other !== menu) other.open = false; });
      scheduleBounds();
    });
    fragment.append(menu);
  }
  $("menus").replaceChildren(fragment);
}
function renderRows(element, rows, selected, onSelect, sources = false) {
  const signature = JSON.stringify([rows, selected]);
  if (element.dataset.signature === signature) return;
  element.dataset.signature = signature;
  const focusId = element.contains(document.activeElement) ? document.activeElement.dataset.id : null;
  const fragment = document.createDocumentFragment();
  for (const row of rows) {
    const id = sources ? row.id : row.uuid;
    const entry = button("", () => onSelect(row));
    entry.className = "row";
    entry.dataset.id = id;
    entry.setAttribute("role", "option");
    entry.setAttribute("aria-selected", String(sources ? row.selected : id === selected));
    const name = document.createElement("span");
    name.className = "name";
    name.textContent = row.name;
    entry.append(name);
    if (sources) {
      const flags = document.createElement("span");
      flags.className = "flag";
      flags.textContent = row.locked ? "🔒" : row.visible ? "●" : "○";
      flags.title = row.locked ? "Заблокирован" : row.visible ? "Видим" : "Скрыт";
      entry.append(flags);
      entry.addEventListener("dblclick", () => request("source.properties", { uuid: row.uuid }));
    }
    fragment.append(entry);
  }
  element.replaceChildren(fragment);
  if (focusId) Array.from(element.children).find(row => row.dataset.id === focusId)?.focus();
}
function render(next) {
  state = next;
  connected = true;
  document.title = next.title + " — WebView2";
  $("connection").textContent = "OBS Studio · WebView2";
  $("scene-title").textContent = next.scenes.find(scene => scene.uuid === next.currentScene)?.name ?? "";
  $("studio-mode").textContent = next.studioMode ? "Studio Mode · Preview" : "";
  $("scenes-title").textContent = next.labels["Basic.Main.Scenes"];
  $("sources-title").textContent = next.labels["Basic.Main.Sources"];
  $("controls-title").textContent = next.labels["Basic.Main.Controls"];
  $("status").textContent = [next.recording ? (next.paused ? "REC ‖" : "REC ●") : "", next.streaming ? "LIVE ●" : "", next.fps.toFixed(2) + " FPS"].filter(Boolean).join("   ");
  $("status").classList.toggle("live", next.recording || next.streaming);
  renderMenus(next.menus);
  renderRows($("scenes"), next.scenes, next.currentScene, scene => request("scene.select", { uuid: scene.uuid }));
  const selection = next.sources.filter(source => source.selected);
  selectedSource = selection.length === 1 ? selection[0].id : "";
  renderRows($("sources"), next.sources, selectedSource, source =>
    request("source.select", { scene: state.currentScene, id: source.id, uuid: source.uuid }), true);
  $("properties").disabled = $("filters").disabled = !selectedSource;
  const controlsSignature = JSON.stringify(next.controls);
  if ($("controls").dataset.signature !== controlsSignature) {
    const focusedControl = $("controls").contains(document.activeElement) ? document.activeElement.dataset.control : null;
    $("controls").dataset.signature = controlsSignature;
    const fragment = document.createDocumentFragment();
    for (const control of next.controls) {
      const element = button(control.text, () => request("control.click", { id: control.id }), control.enabled);
      element.dataset.control = control.id;
      if (control.checkable) element.setAttribute("aria-pressed", String(control.checked));
      fragment.append(element);
    }
    $("controls").replaceChildren(fragment);
    if (focusedControl) Array.from($("controls").children).find(control => control.dataset.control === focusedControl)?.focus();
  }
  scheduleBounds();
}
function scheduleBounds() {
  if (boundsFrame) return;
  boundsFrame = requestAnimationFrame(async () => {
    boundsFrame = 0;
    if (!connected) return;
    const bounds = $("preview").getBoundingClientRect();
    const args = { x: bounds.x, y: bounds.y, width: bounds.width, height: bounds.height, viewportWidth: window.innerWidth, viewportHeight: window.innerHeight, visible: !document.querySelector(".menu[open]") };
    const signature = JSON.stringify(args);
    if (signature === boundsSignature) return;
    boundsSignature = signature;
    await request("preview.bounds", args);
  });
}
$("original").addEventListener("click", () => request("window.original"));
$("properties").addEventListener("click", () => {
  const source = state?.sources.find(item => item.id === selectedSource);
  if (source) request("source.properties", { uuid: source.uuid });
});
$("filters").addEventListener("click", () => {
  const source = state?.sources.find(item => item.id === selectedSource);
  if (source) request("source.filters", { uuid: source.uuid });
});
document.addEventListener("keydown", event => { if (event.key === "Escape") closeMenus(); });
document.addEventListener("click", event => { if (!event.target.closest(".menu")) closeMenus(); });
new ResizeObserver(scheduleBounds).observe($("preview"));
window.addEventListener("resize", scheduleBounds);
window.addEventListener("pagehide", () => bridge?.dispose());
try {
  bridge = createBridge(window.chrome?.webview);
  bridge.subscribe("state.changed", render);
  // The native host publishes state after document navigation completes.
} catch (error) { showError(error); $("connection").textContent = "Нет соединения с OBS"; }
