import { createBridge, createPresentation } from "./bridge.mjs";
import { installExternalDrop } from "./external-drop.mjs";

const root = document.querySelector("#dialog");
const errorBox = document.querySelector("#error");
const controls = new Map();
let connection;
let presentFrame;
let firstState = true;
let clearError;
let scrollAreas = [];
let nativeSize = { width: 1, height: 1 };
let viewportScale = { x: 1, y: 1 };
let fontRevision = 0;
let acceptingDrops = false;

function applyViewportScale() {
  viewportScale = { x: innerWidth / nativeSize.width, y: innerHeight / nativeSize.height };
  root.style.width = `${nativeSize.width}px`;
  root.style.height = `${nativeSize.height}px`;
  root.style.transform = `scale(${viewportScale.x}, ${viewportScale.y})`;
}
addEventListener("resize", applyViewportScale);
document.fonts?.addEventListener("loadingdone", () => {
  ++fontRevision;
  for (const { element: control, data } of controls.values()) {
    if (data.type === "check" || data.type === "radio") fitChoiceText(control.querySelector("span"), data);
    else if (data.type === "label" || data.type === "combo") updateControl(control, data);
  }
});

function applyTheme(theme, style = document.documentElement.style) {
  if (!theme) return;
  for (const key of ["window", "windowText", "base", "text", "button", "buttonText", "mid", "highlight", "highlightedText"])
    if (/^#[0-9a-f]{6}$/i.test(theme[key] || "")) style.setProperty(`--qt-${key}`, theme[key]);
  if (typeof theme.fontFamily === "string") style.setProperty("--qt-font-family", JSON.stringify(theme.fontFamily));
  if (Number.isFinite(theme.fontSize) && theme.fontSize > 0) style.setProperty("--qt-font-size", `${theme.fontSize}px`);
  if (typeof theme.dark === "boolean") style.colorScheme = theme.dark ? "dark" : "light";
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
function applyDecoration(control, data) {
  const decoration = safeIcon(data.decoration);
  control.style.backgroundColor = decoration ? "transparent" : data.background || "transparent";
  control.style.backgroundImage = decoration ? `url("${decoration}")` : "none";
  const region = data.decorationRect;
  control.style.backgroundPosition = region ? `${region.x}px ${region.y}px` : "0 0";
  control.style.backgroundSize = region ? `${region.width}px ${region.height}px` : "100% 100%";
  control.style.backgroundRepeat = "no-repeat";
  // QFrame::frameWidth includes QSS padding/margins, not just the painted edge.
  control.style.border = "none";
}
function fitChoiceText(caption, data) {
  let text = caption.firstElementChild;
  if (!text) { text = element("span", "choice-text"); caption.replaceChildren(text); }
  if (text.textContent !== data.text) text.textContent = data.text;
  const signature = JSON.stringify([data.text, data.font, data.textRect?.width, data.nativeTextWidth, fontRevision]);
  if (caption.dataset.metrics === signature) return;
  caption.dataset.metrics = signature;
  text.style.transform = "none";
  text.style.transformOrigin = data.alignment & 2 ? "right center" : data.alignment & 4 ? "center" : "left center";
  // Qt's hinted glyph advances can differ slightly from Chromium even with the
  // identical bundled font. Preserve a caption that fits in the native widget;
  // don't conceal a genuinely too-long label by shrinking it arbitrarily.
  if (data.nativeTextWidth > 0 && data.textRect && data.nativeTextWidth <= data.textRect.width + .5) {
    const width = text.getBoundingClientRect().width / viewportScale.x;
    if (width > data.textRect.width && width < data.nativeTextWidth * 1.08)
      text.style.transform = `scaleX(${data.nativeTextWidth / width})`;
  }
}
function bindEditor(input, id, numeric = false) {
  let composing = false, committedComposition = null;
  const sendInput = () => {
    if (numeric && (input.value === "" || !input.validity.valid)) return;
    request("dialog.input", { id, value: numeric ? Number(input.value) : input.value });
  };
  input.addEventListener("compositionstart", () => { composing = true; committedComposition = null; });
  input.addEventListener("compositionend", () => {
    composing = false;
    committedComposition = input.value;
    sendInput();
  });
  input.addEventListener("input", event => {
    // Qt receives committed text; the browser owns the in-progress IME preedit.
    if (composing || event.isComposing) return;
    const duplicateCommit = committedComposition !== null && committedComposition === input.value;
    committedComposition = null;
    if (!duplicateCommit) sendInput();
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

// A scrollbar represents a viewport, not a slider. Its thumb must grow with the
// visible page and its minimum must stay at the top/left, as in QScrollBar.
function scrollbarGeometry(data, length) {
  const range = Math.max(0, data.maximum - data.minimum);
  const native = data.nativeStyle;
  if (native?.grooveRect && native?.thumbRect) {
    const thumb = data.vertical ? native.thumbRect.height : native.thumbRect.width;
    const origin = data.vertical ? native.grooveRect.y : native.grooveRect.x;
    const groove = data.vertical ? native.grooveRect.height : native.grooveRect.width;
    const travel = Math.max(0, groove - thumb);
    const ratio = range ? (data.value - data.minimum) / range : 0;
    const offset = data.value === native.value ? (data.vertical ? native.thumbRect.y : native.thumbRect.x)
      : origin + travel * (native.reversed ? 1 - ratio : ratio);
    return { thumb, travel, offset, range, reversed: !!native.reversed };
  }
  const page = Math.max(1, data.page || 1);
  const thumb = Math.min(length, Math.max(18, length * page / (range + page)));
  const travel = Math.max(0, length - thumb);
  return { thumb, travel, offset: range ? travel * (data.value - data.minimum) / range : 0, range };
}
function updateScrollbar(control, data, vertical) {
  control.scrollData = { ...data, vertical };
  data = control.scrollData;
  control.classList.toggle("vertical", vertical);
  control.setAttribute("aria-orientation", vertical ? "vertical" : "horizontal");
  control.setAttribute("aria-valuemin", data.minimum);
  control.setAttribute("aria-valuemax", data.maximum);
  control.setAttribute("aria-valuenow", data.value);
  control.setAttribute("aria-disabled", String(data.enabled === false));
  const length = vertical ? control.clientHeight : control.clientWidth;
  const geometry = scrollbarGeometry(data, length);
  const thumb = control.firstElementChild;
  const native = data.nativeStyle;
  control.classList.toggle("native-style", !!safeIcon(native?.normal));
  if (native?.thumbRect) {
    thumb.style.cssText = "";
    place(thumb, { ...native.thumbRect, ...(vertical ? { y: geometry.offset } : { x: geometry.offset }) });
    for (const [name, image] of [["native", native.normal], ["hover", native.hover], ["pressed", native.pressed]])
      control.style.setProperty(`--scrollbar-${name}`, `url("${safeIcon(image)}")`);
  } else {
    thumb.style.cssText = vertical
      ? `top:${geometry.offset}px;height:${geometry.thumb}px;left:2px;right:2px`
      : `left:${geometry.offset}px;width:${geometry.thumb}px;top:2px;bottom:2px`;
  }
}
function createScrollbar(onValue, onFinish = () => {}) {
  const control = element("div", "scrollbar");
  control.tabIndex = 0;
  control.setAttribute("role", "scrollbar");
  control.setAttribute("aria-label", "Scroll");
  control.append(element("div", "scrollbar-thumb"));
  let drag = null;
  const set = value => {
    const data = control.scrollData;
    if (!data || data.enabled === false) return;
    const next = Math.max(data.minimum, Math.min(data.maximum, Math.round(value)));
    if (next === data.value) return;
    updateScrollbar(control, { ...data, value: next }, data.vertical);
    onValue(next);
  };
  control.addEventListener("pointerdown", event => {
    const data = control.scrollData;
    if (event.button !== 0 || !data || data.enabled === false) return;
    event.preventDefault(); control.focus({ preventScroll: true });
    const bounds = control.getBoundingClientRect();
    const length = data.vertical ? control.clientHeight : control.clientWidth;
    const renderedLength = data.vertical ? bounds.height : bounds.width;
    const geometry = scrollbarGeometry(data, length);
    const position = ((data.vertical ? event.clientY - bounds.top : event.clientX - bounds.left) / renderedLength) * length;
    if (event.target === control.firstElementChild && geometry.travel > 0) {
      drag = { start: data.vertical ? event.clientY : event.clientX, value: data.value,
        unitsPerPixel: geometry.range / geometry.travel * length / renderedLength * (geometry.reversed ? -1 : 1) };
      control.classList.toggle("dragging", true);
      control.setPointerCapture(event.pointerId);
    } else {
      const point = { x: (event.clientX - bounds.left) / bounds.width * control.clientWidth,
        y: (event.clientY - bounds.top) / bounds.height * control.clientHeight };
      const inside = rect => rect && point.x >= rect.x && point.x < rect.x + rect.width && point.y >= rect.y && point.y < rect.y + rect.height;
      if (inside(data.nativeStyle?.subLineRect)) set(data.value - (data.step || 1));
      else if (inside(data.nativeStyle?.addLineRect)) set(data.value + (data.step || 1));
      else set(data.value + (position < geometry.offset ? -1 : 1) * (geometry.reversed ? -1 : 1) * Math.max(1, data.page || data.step || 1));
      onFinish();
    }
  });
  control.addEventListener("pointermove", event => {
    if (!drag) return;
    const position = control.scrollData.vertical ? event.clientY : event.clientX;
    set(drag.value + (position - drag.start) * drag.unitsPerPixel);
  });
  const finish = () => { if (drag) { drag = null; control.classList.toggle("dragging", false); onFinish(); } };
  control.addEventListener("pointerup", finish);
  control.addEventListener("pointercancel", finish);
  control.addEventListener("lostpointercapture", finish);
  control.addEventListener("keydown", event => {
    const data = control.scrollData;
    if (!data) return;
    let value = data.value;
    if (event.key === "Home") value = data.minimum;
    else if (event.key === "End") value = data.maximum;
    else if (event.key === "PageUp") value -= data.page || 1;
    else if (event.key === "PageDown") value += data.page || 1;
    else if (event.key === "ArrowUp" || event.key === "ArrowLeft") value -= data.step || 1;
    else if (event.key === "ArrowDown" || event.key === "ArrowRight") value += data.step || 1;
    else return;
    event.preventDefault(); event.stopPropagation(); set(value); onFinish();
  });
  return control;
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
    select.addEventListener("change", () => request("dialog.choose", { id, index: Number(select.value) }));
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
  } else if (node.type === "scroll") {
    control = createScrollbar(value => request("dialog.input", { id, value }), () => request("dialog.finish", { id }));
  } else if (node.type === "slider") {
    control = element("input", `range ${node.type}`); control.type = "range";
    bindEditor(control, id, true);
    control.addEventListener("change", () => request("dialog.finish", { id }));
  } else if (node.type === "items") {
    control = element("div", "items"); control.tabIndex = 0;
    control.setAttribute("role", "grid");
    control.append(element("div", "item-header"), element("div", "item-viewport"));
    const vertical = createScrollbar(value => request("dialog.scroll", { id, value, horizontal: false }));
    const horizontal = createScrollbar(value => request("dialog.scroll", { id, value, horizontal: true }));
    vertical.classList.add("item-scroll", "vertical");
    horizontal.classList.add("item-scroll", "horizontal");
    control.append(vertical, horizontal);
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
      if (event.isComposing) return;
      const printable = event.key.length === 1 && !event.ctrlKey && !event.altKey && !event.metaKey;
      if (["ArrowUp", "ArrowDown", "ArrowLeft", "ArrowRight", "Home", "End", "PageUp", "PageDown", "F2", "Delete", " "].includes(event.key) || (event.ctrlKey && event.key.toLowerCase() === "a") || printable) {
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
        button.style.textAlign = col.alignment & 4 ? "center" : col.alignment & 2 ? "right" : "left";
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
    place(bar, scroll.rect || (vertical ? { x: data.rect.width - 15, y: data.viewport.y, width: 14, height: data.viewport.height }
      : { x: data.viewport.x, y: data.rect.height - 15, width: data.viewport.width, height: 14 }));
    updateScrollbar(bar, { ...scroll, enabled: data.enabled }, vertical);
  }
}

function updateControl(control, data) {
  place(control, data.rect);
  control.classList.toggle("search-hit", !!data.searchHit);
  if (data.palette) {
    applyTheme(data.palette, control.style);
    control.dataset.themed = "true";
    const foreground = data.type === "button" ? "buttonText" : ["text", "multiline", "number", "combo", "items"].includes(data.type) ? "text" : "windowText";
    control.style.color = `var(--qt-${foreground})`;
  }
  if (data.type === "panel") {
    applyDecoration(control, data);
  }
  if (data.font) {
    control.style.fontFamily = JSON.stringify(data.font.family);
    control.style.fontSize = `${data.font.pixelSize}px`;
    control.style.fontWeight = data.font.weight;
    control.style.fontStyle = data.font.italic ? "italic" : "normal";
    control.style.lineHeight = `${data.font.lineHeight}px`;
  }
  const clip = data.clip;
  control.style.clipPath = `inset(${Math.max(0, clip.y - data.rect.y)}px ${Math.max(0, data.rect.x + data.rect.width - clip.x - clip.width)}px ${Math.max(0, data.rect.y + data.rect.height - clip.y - clip.height)}px ${Math.max(0, clip.x - data.rect.x)}px)`;
  control.title = data.tooltip || data.accessibleName || "";
  if (data.accessibleName) control.setAttribute("aria-label", data.accessibleName);
  if ("disabled" in control) control.disabled = !data.enabled;
  if (data.type === "button") {
    control.querySelector("span").textContent = data.text;
    setIcon(control, data.icon);
    control.classList.toggle("checked", data.checkable && data.checked);
    if (data.checkable && data.checked) control.style.color = "var(--qt-highlightedText)";
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
    const caption = control.querySelector("span");
    if (data.indicatorRect && data.textRect) {
      control.classList.add("native-metrics");
      place(input, data.indicatorRect); place(caption, data.textRect);
    }
    fitChoiceText(caption, data);
  } else if (["text", "multiline", "number"].includes(data.type)) {
    const input = data.type === "number" ? control.querySelector("input") : control;
    if (data.type === "text") {
      input.type = data.password ? "password" : "text";
      if (safeIcon(data.decoration)) {
        applyDecoration(input, data);
        input.style.borderRadius = "0";
      }
      input.style.textAlign = data.alignment & 4 ? "center" : data.alignment & 2 ? "right" : "left";
      if (data.textRect) {
        const text = data.textRect;
        input.style.padding = `${Math.max(0, text.y)}px ${Math.max(0, data.rect.width - text.x - text.width)}px ${Math.max(0, data.rect.height - text.y - text.height)}px ${Math.max(0, text.x)}px`;
      }
    }
    setValue(input, data.value); input.readOnly = !!data.readOnly; input.disabled = !data.enabled;
    input.placeholder = data.placeholder || "";
    if (data.maxLength !== undefined) input.maxLength = data.maxLength;
    if (data.type === "number") {
      input.min = data.minimum; input.max = data.maximum; input.step = data.step;
      control.querySelector(".prefix").textContent = data.prefix || "";
      control.querySelector(".suffix").textContent = data.suffix || "";
    }
  } else if (data.type === "combo") {
    const select = control.querySelector("select");
    const signature = JSON.stringify([data.choices, data.placeholder]);
    if (select.dataset.signature !== signature) {
      const options = data.choices.map((choice, index) => { const option = element("option"); option.textContent = choice.text; option.value = String(index); option.disabled = !choice.enabled; return option; });
      if (data.placeholder) {
        const placeholder = element("option"); placeholder.textContent = data.placeholder;
        placeholder.value = "-1"; placeholder.disabled = true; placeholder.hidden = true; options.unshift(placeholder);
      }
      select.replaceChildren(...options);
      select.dataset.signature = signature;
    }
    select.value = String(data.index); select.disabled = !data.enabled;
    // Keep the real select for its keyboard, accessibility and popup semantics;
    // draw only its closed caption in Qt's actual edit-field rectangle.
    let caption = control.querySelector(".combo-caption");
    if (!data.editable && data.textRect && Number.isFinite(data.nativeTextWidth)) {
      if (!caption) { caption = element("span", "combo-caption"); caption.setAttribute("aria-hidden", "true"); control.append(caption); }
      select.classList.add("native-caption-select");
      place(caption, data.textRect);
      fitChoiceText(caption, { ...data, text: data.index < 0 ? data.placeholder || "" : data.value || "" });
    } else { select.classList.remove("native-caption-select"); caption?.remove(); }
    const input = control.querySelector("input");
    if (input) {
      setValue(input, data.value); input.disabled = !data.enabled; input.readOnly = !!data.readOnly;
      if (data.maxLength !== undefined) input.maxLength = data.maxLength;
      input.placeholder = data.placeholder || "";
    }
  } else if (data.type === "label") {
    const caption = control.querySelector("span");
    if (!data.wordWrap && data.textRect && Number.isFinite(data.nativeTextWidth)) {
      caption.style.display = "block"; caption.style.width = "100%";
      caption.style.textAlign = data.alignment & 4 ? "center" : data.alignment & 2 ? "right" : "left";
      fitChoiceText(caption, data);
    } else { caption.textContent = data.text; caption.style.display = ""; caption.style.width = ""; }
    control.classList.toggle("wrap", data.wordWrap);
    control.classList.toggle("center", !!(data.alignment & 4));
    control.classList.toggle("right", !!(data.alignment & 2));
    applyDecoration(control, data);
    setIcon(control, data.icon);
    if (data.iconRect) {
      const image = control.querySelector(":scope > img");
      if (image) { image.style.position = "absolute"; place(image, data.iconRect); }
    }
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
    const caption = control.querySelector("legend > *");
    caption.textContent = (data.checkable ? (data.checked ? "☑ " : "☐ ") : "") + data.text;
    const decorated = !!safeIcon(data.decoration);
    control.classList.toggle("native-decoration", decorated);
    if (decorated) {
      caption.classList.add("native-caption");
      applyDecoration(control, data);
      if (data.titleRect) place(control.querySelector("legend"), data.titleRect);
      if (data.checkable) caption.setAttribute("aria-pressed", String(!!data.checked));
      caption.disabled = !data.enabled;
    }
  } else if (data.type === "scroll") {
    updateScrollbar(control, data, data.vertical);
  } else if (data.type === "slider") {
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
  acceptingDrops = !!state.acceptDrops && !!state.enabled && !state.closed;
  nativeSize = { width: Math.max(1, state.width || 1), height: Math.max(1, state.height || 1) };
  applyViewportScale();
  applyTheme(state.theme);
  scrollAreas = (state.nodes ?? []).filter(node => node.type === "scrollArea");
  document.title = state.title || "OBS Studio";
  root.setAttribute("aria-label", state.title || "OBS dialog");
  const present = new Set();
  const created = new Set();
  let layer = 0;
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
    // A container may become painted after a theme/property change. Preserve Qt
    // parent-before-child stacking without reparenting the focused HTML input.
    existing.element.style.zIndex = ++layer;
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
  presentFrame?.().catch(showError);
}

document.addEventListener("keydown", event => {
  if (event.isComposing) return;
  const settingsSearch = [...controls.values()].find(control => control.data.name === "settingsSearch");
  if (settingsSearch && (event.ctrlKey || event.metaKey) && event.key.toLowerCase() === "f") {
    event.preventDefault();
    settingsSearch.element.focus({ preventScroll: true });
    settingsSearch.element.select();
    return;
  }
  const control = event.target.closest(".control");
  const id = control?.dataset.id;
  if (id === settingsSearch?.data.id && ["ArrowDown", "ArrowUp", "Enter", "Escape"].includes(event.key)) {
    event.preventDefault(); event.stopPropagation();
    request("dialog.key", { id, key: event.key, ...modifiers(event) });
    return;
  }
  if (event.key === "Tab" && controls.get(id)?.data.itemView && event.target.matches("input,textarea,select")) {
    // Qt delegates use Tab/Backtab to commit and move their model cell.
    event.preventDefault(); event.stopPropagation();
    request("dialog.key", { id, key: event.key, ...modifiers(event) });
    return;
  }
  if (event.key === "Escape" || (event.key === "Enter" && event.target.tagName !== "TEXTAREA" && event.target.tagName !== "BUTTON" && event.target.tagName !== "SELECT")) {
    event.preventDefault();
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
  presentFrame = createPresentation(connection);
  connection.subscribe("dialog.state", render);
  installExternalDrop(document, { onError: showError, enabled: () => acceptingDrops });
  request("dialog.state");
} catch (error) { showError(error); }
