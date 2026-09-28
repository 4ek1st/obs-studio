import { createBridge } from "./bridge.mjs";

const root = document.querySelector("#dialog");
const errorBox = document.querySelector("#error");
const controls = new Map();
let connection;
let firstState = true;
let clearError;
let scrollAreas = [];
let nativeSize = { width: 1, height: 1 };
let viewportScale = { x: 1, y: 1 };

function applyViewportScale() {
  viewportScale = { x: innerWidth / nativeSize.width, y: innerHeight / nativeSize.height };
  root.style.width = `${nativeSize.width}px`;
  root.style.height = `${nativeSize.height}px`;
  root.style.transform = `scale(${viewportScale.x}, ${viewportScale.y})`;
}
addEventListener("resize", applyViewportScale);

function applyTheme(theme) {
  if (!theme) return;
  const style = document.documentElement.style;
  for (const key of ["window", "windowText", "base", "text", "button", "buttonText", "mid", "highlight", "highlightedText"])
    if (/^#[0-9a-f]{6}$/i.test(theme[key] || "")) style.setProperty(`--qt-${key}`, theme[key]);
  if (typeof theme.fontFamily === "string") style.setProperty("--qt-font-family", JSON.stringify(theme.fontFamily));
  if (Number.isFinite(theme.fontSize) && theme.fontSize > 0) style.setProperty("--qt-font-size", `${theme.fontSize}px`);
  style.colorScheme = theme.dark ? "dark" : "light";
}

function showError(error) {
  // The native host provides public, value-free diagnostics; never print field contents.
  errorBox.textContent = error?.message || "The control could not be updated.";
  errorBox.hidden = false;
  clearTimeout(clearError);
  clearError = setTimeout(() => { errorBox.hidden = true; }, 5000);
}

function request(command, args = {}) {
  return connection.request(command, args).catch(error => { showError(error); return null; });
}

function modifiers(event) { return { ctrl: event.ctrlKey, shift: event.shiftKey, alt: event.altKey }; }
function place(element, rect) {
  element.style.left = `${rect.x}px`;
  element.style.top = `${rect.y}px`;
  element.style.width = `${Math.max(0, rect.width)}px`;
  element.style.height = `${Math.max(0, rect.height)}px`;
}
function element(tag, className) {
  const node = document.createElement(tag);
  if (className) node.className = className;
  return node;
}
function safeIcon(src) { return typeof src === "string" && src.startsWith("data:image/png;base64,") ? src : ""; }
function setIcon(parent, source) {
  const src = safeIcon(source);
  let img = parent.querySelector(":scope > img");
  if (!src) { img?.remove(); return; }
  if (!img) { img = element("img"); img.alt = ""; parent.prepend(img); }
  if (img.src !== src) img.src = src;
}
function setValue(input, value) {
  if (document.activeElement !== input && input.value !== String(value ?? "")) input.value = value ?? "";
}
function bindEditor(input, id, numeric = false) {
  input.addEventListener("input", () => {
    if (numeric && (input.value === "" || !input.validity.valid)) return;
    request("dialog.input", { id, value: numeric ? Number(input.value) : input.value });
  });
  input.addEventListener("blur", () => {
    // Offscreen fields still need a commit. A deleted delegate may already be gone natively.
    connection.request("dialog.finish", { id }).catch(error => { if (input.isConnected) showError(error); });
  });
  input.addEventListener("contextmenu", event => {
    // Text editing uses its native Qt context actions (including OBS-specific actions).
    event.preventDefault();
    if (typeof input.selectionStart === "number") request("dialog.selection", { id, start: input.selectionStart, end: input.selectionEnd });
    request("dialog.context", { id, ...modifiers(event) });
  });
}

function createControl(node) {
  const id = node.id;
  let control;
  if (node.type === "button") {
    control = element("button", "button");
    control.append(element("span"));
    control.addEventListener("click", () => request("dialog.click", { id }));
    control.addEventListener("keydown", event => {
      if (event.altKey && event.key === "ArrowDown" && controls.get(id)?.data.menu) {
        event.preventDefault(); request("dialog.menu", { id });
      }
    });
  } else if (node.type === "check" || node.type === "radio") {
    control = element("label", "choice");
    const input = element("input"); input.type = node.type === "radio" ? "radio" : "checkbox";
    control.append(input, element("span"));
    input.addEventListener("click", event => { event.preventDefault(); request("dialog.click", { id }); });
  } else if (node.type === "text" || node.type === "multiline") {
    control = element(node.type === "text" ? "input" : "textarea", "text");
    if (node.type === "text") control.type = node.password ? "password" : "text";
    control.autocomplete = "off";
    control.spellcheck = false;
    bindEditor(control, id);
  } else if (node.type === "number") {
    control = element("div", "input-shell");
    const prefix = element("span", "affix prefix");
    const input = element("input"); input.type = "number";
    const suffix = element("span", "affix suffix");
    control.append(prefix, input, suffix); bindEditor(input, id, true);
  } else if (node.type === "combo") {
    control = element("div", "combo");
    const select = element("select"); select.style.cssText = "width:100%;height:100%";
    control.append(select);
    select.addEventListener("change", () => request("dialog.choose", { id, index: select.selectedIndex }));
    if (node.editable) {
      const input = element("input", "text");
      input.style.cssText = "position:absolute;left:1px;top:1px;width:calc(100% - 25px);height:calc(100% - 2px);border:0";
      bindEditor(input, id); control.append(input);
    }
  } else if (node.type === "label") {
    control = element("div", "label"); control.append(element("span"));
  } else if (node.type === "tabs") {
    control = element("div", "tabs"); control.setAttribute("role", "tablist");
  } else if (node.type === "group") {
    control = element("fieldset", "group");
    const legend = element("legend");
    const caption = element(node.checkable ? "button" : "span"); legend.append(caption); control.append(legend);
    if (node.checkable) caption.addEventListener("click", () => request("dialog.click", { id }));
  } else if (node.type === "slider" || node.type === "scroll") {
    control = element("input", `range ${node.type}`); control.type = "range";
    bindEditor(control, id, true);
    control.addEventListener("change", () => request("dialog.finish", { id }));
  } else if (node.type === "items") {
    control = element("div", "items"); control.tabIndex = 0;
    control.setAttribute("role", "grid");
    control.append(element("div", "item-header"), element("div", "item-viewport"));
    const vertical = element("input", "item-scroll vertical"); vertical.type = "range";
    const horizontal = element("input", "item-scroll horizontal"); horizontal.type = "range";
    control.append(vertical, horizontal);
    for (const [bar, isHorizontal] of [[vertical, false], [horizontal, true]]) {
      bar.addEventListener("input", () => request("dialog.scroll", { id, value: Number(bar.value), horizontal: isHorizontal }));
    }
    control.addEventListener("wheel", event => {
      event.preventDefault();
      const data = controls.get(id)?.data;
      const useHorizontal = event.shiftKey || Math.abs(event.deltaX) > Math.abs(event.deltaY);
      const scroll = useHorizontal ? data?.horizontalScroll : data?.verticalScroll;
      if (!scroll) return;
      const delta = useHorizontal && event.deltaX ? event.deltaX : event.deltaY;
      const step = Math.max(1, Math.round((scroll.page || 10) / 5));
      const value = Math.max(scroll.minimum, Math.min(scroll.maximum, scroll.value + Math.sign(delta) * step));
      request("dialog.scroll", { id, value, horizontal: useHorizontal });
    }, { passive: false });
    control.addEventListener("keydown", event => {
      if (["ArrowUp", "ArrowDown", "ArrowLeft", "ArrowRight", "Home", "End", "PageUp", "PageDown", "F2", "Delete", " "].includes(event.key) || (event.ctrlKey && event.key.toLowerCase() === "a")) {
        event.preventDefault(); event.stopPropagation();
        request("dialog.key", { id, key: event.key, ...modifiers(event) });
      }
    });
  } else if (node.type === "progress") {
    control = element("div", "progress"); control.append(element("i"), element("span"));
  } else {
    control = element("div");
  }
  control.classList.add("control");
  control.dataset.id = id;
  control.dataset.type = node.type;
  return control;
}

function updateItems(control, data) {
  const viewport = control.querySelector(".item-viewport");
  place(viewport, data.viewport);
  const oldRows = new Map([...viewport.children].map(row => [row.dataset.item, row]));
  for (const item of data.items ?? []) {
    let row = oldRows.get(item.id);
    if (!row) {
      row = element("div", "item-row");
      row.dataset.item = item.id;
      row.setAttribute("role", "row");
      row.append(element("span", "item-label"));
      row.addEventListener("click", event => {
        if (event.target.closest(".expand,input")) return;
        control.focus({ preventScroll: true });
        request("dialog.item", { id: data.id, item: item.id, action: "select", ...modifiers(event) });
      });
      row.addEventListener("dblclick", event => {
        if (event.target.closest(".expand,input")) return;
        request("dialog.item", { id: data.id, item: item.id, action: "activate", ...modifiers(event) });
      });
      row.addEventListener("contextmenu", event => {
        event.preventDefault(); request("dialog.item", { id: data.id, item: item.id, action: "context", ...modifiers(event) });
      });
      viewport.append(row);
    }
    oldRows.delete(item.id);
    place(row, item.rect);
    row.querySelector(".item-label").textContent = item.text;
    row.classList.toggle("selected", item.selected);
    row.classList.toggle("current", item.current);
    row.setAttribute("aria-selected", String(item.selected));
    row.setAttribute("aria-disabled", String(!item.enabled));
    row.title = item.tooltip || "";
    setIcon(row, item.icon);
    let expand = row.querySelector(".expand");
    if (item.children && item.column === 0) {
      if (!expand) {
        expand = element("button", "expand");
        expand.addEventListener("click", event => { event.stopPropagation(); request("dialog.item", { id: data.id, item: item.id, action: "expand" }); });
        row.prepend(expand);
      }
      expand.textContent = item.expanded ? "▾" : "▸";
      expand.setAttribute("aria-label", item.expanded ? "Collapse" : "Expand");
    } else expand?.remove();
    let check = row.querySelector("input");
    if (item.checkable) {
      if (!check) {
        check = element("input"); check.type = "checkbox";
        check.addEventListener("click", event => { event.preventDefault(); event.stopPropagation(); request("dialog.item", { id: data.id, item: item.id, action: "toggle" }); });
        row.prepend(check);
      }
      check.checked = item.checked === 2; check.indeterminate = item.checked === 1; check.disabled = !item.enabled;
    } else check?.remove();
  }
  for (const row of oldRows.values()) row.remove();
  const header = control.querySelector(".item-header");
  header.hidden = !data.headerRect;
  if (data.headerRect) {
    place(header, data.headerRect);
    const signature = JSON.stringify(data.columns);
    if (header.dataset.signature !== signature) {
      header.replaceChildren();
      for (const col of data.columns ?? []) {
        const button = element("button"); button.textContent = col.text;
        button.style.left = `${col.x}px`; button.style.width = `${col.width}px`;
        button.addEventListener("click", () => request("dialog.header", { id: data.id, column: col.index }));
        header.append(button);
      }
      header.dataset.signature = signature;
    }
  }
  for (const [selector, scroll, vertical] of [[".vertical", data.verticalScroll, true], [".horizontal", data.horizontalScroll, false]]) {
    const bar = control.querySelector(selector);
    bar.hidden = !scroll || scroll.maximum <= scroll.minimum;
    if (!scroll) continue;
    bar.min = scroll.minimum; bar.max = scroll.maximum; bar.value = scroll.value;
    place(bar, vertical ? { x: data.rect.width - 15, y: data.viewport.y, width: 14, height: data.viewport.height }
      : { x: data.viewport.x, y: data.rect.height - 15, width: data.viewport.width, height: 14 });
  }
}

function updateControl(control, data) {
  place(control, data.rect);
  const clip = data.clip;
  control.style.clipPath = `inset(${Math.max(0, clip.y - data.rect.y)}px ${Math.max(0, data.rect.x + data.rect.width - clip.x - clip.width)}px ${Math.max(0, data.rect.y + data.rect.height - clip.y - clip.height)}px ${Math.max(0, clip.x - data.rect.x)}px)`;
  control.title = data.tooltip || data.accessibleName || "";
  if (data.accessibleName) control.setAttribute("aria-label", data.accessibleName);
  if ("disabled" in control) control.disabled = !data.enabled;
  if (data.type === "button") {
    control.querySelector("span").textContent = data.text;
    setIcon(control, data.icon);
    control.classList.toggle("checked", data.checkable && data.checked);
    control.classList.toggle("default", data.default);
    let arrow = control.querySelector(".menu-arrow");
    if (data.menu && !arrow) {
      arrow = element("span", "menu-arrow"); arrow.textContent = "▾"; arrow.title = "Open menu (Alt+Down)";
      arrow.addEventListener("click", event => { event.stopPropagation(); request("dialog.menu", { id: data.id }); });
      control.append(arrow);
    } else if (!data.menu) arrow?.remove();
    if (data.menu) control.setAttribute("aria-haspopup", "menu"); else control.removeAttribute("aria-haspopup");
  } else if (data.type === "check" || data.type === "radio") {
    const input = control.querySelector("input");
    input.disabled = !data.enabled; input.checked = data.checked; input.indeterminate = !!data.indeterminate;
    control.classList.toggle("disabled", !data.enabled);
    control.querySelector("span").textContent = data.text;
  } else if (["text", "multiline", "number"].includes(data.type)) {
    const input = data.type === "number" ? control.querySelector("input") : control;
    setValue(input, data.value); input.readOnly = !!data.readOnly; input.disabled = !data.enabled;
    input.placeholder = data.placeholder || "";
    if (data.maxLength) input.maxLength = data.maxLength;
    if (data.type === "number") {
      input.min = data.minimum; input.max = data.maximum; input.step = data.step;
      control.querySelector(".prefix").textContent = data.prefix || "";
      control.querySelector(".suffix").textContent = data.suffix || "";
    }
  } else if (data.type === "combo") {
    const select = control.querySelector("select");
    const signature = JSON.stringify(data.choices);
    if (select.dataset.signature !== signature) {
      select.replaceChildren(...data.choices.map(choice => { const option = element("option"); option.textContent = choice.text; option.disabled = !choice.enabled; return option; }));
      select.dataset.signature = signature;
    }
    select.selectedIndex = data.index; select.disabled = !data.enabled;
    const input = control.querySelector("input");
    if (input) { setValue(input, data.value); input.disabled = !data.enabled; }
  } else if (data.type === "label") {
    control.querySelector("span").textContent = data.text;
    control.classList.toggle("wrap", data.wordWrap);
    control.classList.toggle("center", !!(data.alignment & 4));
    control.classList.toggle("right", !!(data.alignment & 2));
    setIcon(control, data.icon);
  } else if (data.type === "tabs") {
    const signature = JSON.stringify([data.tabs, data.index, data.enabled]);
    if (control.dataset.signature !== signature) {
      control.replaceChildren();
      for (const tab of data.tabs) {
        const button = element("button"); button.textContent = tab.text; button.setAttribute("role", "tab");
        button.classList.toggle("current", tab.index === data.index);
        button.setAttribute("aria-selected", String(tab.index === data.index));
        button.disabled = !data.enabled || !tab.enabled;
        place(button, tab.rect);
        button.addEventListener("click", () => request("dialog.choose", { id: data.id, index: tab.index }));
        control.append(button);
      }
      control.dataset.signature = signature;
    }
  } else if (data.type === "group") {
    control.querySelector("legend > *").textContent = (data.checkable ? (data.checked ? "☑ " : "☐ ") : "") + data.text;
  } else if (data.type === "slider" || data.type === "scroll") {
    control.min = data.minimum; control.max = data.maximum; control.step = data.step || 1;
    setValue(control, data.value); control.classList.toggle("vertical", data.vertical);
  } else if (data.type === "items") {
    updateItems(control, data);
  } else if (data.type === "progress") {
    const value = data.maximum > data.minimum ? (data.value - data.minimum) / (data.maximum - data.minimum) : 1;
    control.querySelector("i").style.width = `${Math.max(0, Math.min(100, value * 100))}%`;
    control.querySelector("span").textContent = data.text || "";
  }
}

function render(state) {
  nativeSize = { width: Math.max(1, state.width || 1), height: Math.max(1, state.height || 1) };
  applyViewportScale();
  applyTheme(state.theme);
  scrollAreas = (state.nodes ?? []).filter(node => node.type === "scrollArea");
  document.title = state.title || "OBS Studio";
  root.setAttribute("aria-label", state.title || "OBS dialog");
  const present = new Set();
  const created = new Set();
  for (const data of state.nodes ?? []) {
    present.add(data.id);
    let existing = controls.get(data.id);
    // Recreate only when the control's kind changes. Polls never replace focused editors.
    if (existing && (existing.data.type !== data.type || existing.data.editable !== data.editable)) {
      existing.element.remove(); controls.delete(data.id); existing = null;
    }
    if (!existing) {
      existing = { element: createControl(data), data }; controls.set(data.id, existing); root.append(existing.element);
      created.add(data.id);
    }
    existing.data = data;
    updateControl(existing.element, data);
  }
  for (const [id, control] of controls) {
    if (!present.has(id)) { control.element.remove(); controls.delete(id); }
  }
  if ((firstState && state.nodes?.length) || (created.has(state.focus) && controls.get(state.focus)?.data.itemView)) {
    firstState = false;
    const original = controls.get(state.focus)?.element;
    const focus = original?.matches("input,textarea,select,button,[tabindex]") ? original : original?.querySelector("input,textarea,select,button") || root.querySelector("input:not([type=range]):not(:disabled),textarea:not(:disabled),select:not(:disabled),button:not(:disabled)");
    focus?.focus({ preventScroll: true });
    if (focus?.matches("input[type=text],input[type=password]")) {
      const data = controls.get(state.focus)?.data;
      if (data?.selectionStart >= 0) focus.setSelectionRange(data.selectionStart, data.selectionStart + data.selectionLength);
      else focus.select();
    }
  }
}

document.addEventListener("keydown", event => {
  if (event.isComposing) return;
  if (event.key === "Escape" || (event.key === "Enter" && event.target.tagName !== "TEXTAREA" && event.target.tagName !== "BUTTON" && event.target.tagName !== "SELECT")) {
    event.preventDefault();
    const control = event.target.closest(".control");
    const id = control?.dataset.id;
    // Qt editors/delegates see Return/Escape before their parent dialog does.
    request("dialog.key", { ...(id ? { id } : {}), key: event.key, ...modifiers(event) });
  }
});

document.addEventListener("wheel", event => {
  if (event.defaultPrevented || event.ctrlKey) return;
  const x = event.clientX / viewportScale.x, y = event.clientY / viewportScale.y;
  const area = [...scrollAreas].reverse().find(node => x >= node.clip.x && y >= node.clip.y &&
    x < node.clip.x + node.clip.width && y < node.clip.y + node.clip.height);
  if (!area) return;
  event.preventDefault();
  const scale = event.deltaMode === 1 ? 40 : event.deltaMode === 2 ? 120 : 1;
  request("dialog.wheel", { id: area.id, deltaX: event.deltaX * scale, deltaY: event.deltaY * scale, ...modifiers(event) });
}, { passive: false });

try {
  connection = createBridge(globalThis.chrome?.webview);
  connection.subscribe("dialog.state", render);
  request("dialog.state");
} catch (error) { showError(error); }
