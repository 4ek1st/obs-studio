import assert from "node:assert/strict";
import {readFile} from "node:fs/promises";
import vm from "node:vm";

class Element {
  constructor(tag = "div") { this.tagName = tag.toUpperCase(); }
  style = {setProperty(name, value) { this[name] = value; }};
  dataset = {}; children = []; listeners = {}; value = ""; type = "text";
  classList = {add() {}, remove() {}, toggle() {}};
  get firstElementChild() { return this.children[0]; }
  getBoundingClientRect() { return {width:this.measuredWidth ?? 102}; }
  setAttribute() {} removeAttribute() {}
  append(...children) { for (const child of children) { child.parent = this; this.children.push(child); } }
  prepend(child) { child.parent = this; this.children.unshift(child); }
  replaceChildren(...children) { this.children = []; this.append(...children); }
  querySelector(selector) { return this.children.find(child => child.matches(selector)) || null; }
  matches(selector) { return selector.split(",").some(part => this.tagName.toLowerCase() === part.replace(/\[.*$/, "")); }
  closest(selector) { return selector === ".control" ? this.dataset.id ? this : this.parent?.closest(selector) : null; }
  addEventListener(name, callback) { this.listeners[name] = callback; }
  focus() { sandbox.document.activeElement = this; }
  select() {} remove() {} setSelectionRange(start, end) { this.selectionStart = start; this.selectionEnd = end; }
}
const root = new Element(), errors = new Element(), requests = [], events = new Map();
let render;
const sandbox = {
  innerWidth: 800, innerHeight: 600, setTimeout, clearTimeout, addEventListener() {},
  document: {documentElement: new Element(), activeElement: null,
    querySelector: selector => selector === "#dialog" ? root : errors,
    createElement: tag => new Element(tag), addEventListener: (name, callback) => events.set(name, callback)},
  createBridge: () => ({subscribe: (_name, callback) => { render = callback; }, request: (command, args) => {
    requests.push({command, args}); return Promise.resolve({}); }}),
  installExternalDrop() {},
  createPresentation: () => async () => {},
};
vm.runInNewContext((await readFile(new URL("../../frontend/webview2/ui/dialog.js", import.meta.url), "utf8")).replace(/^import .*?;\s*/gm, ""), sandbox);
const rect = {x: 0, y: 0, width: 220, height: 28};
const text = {id: "key", type: "text", password: true, enabled: true, value: "fixture-only-value", rect, clip: rect};
render({width: 800, height: 600, nodes: [text]});
const input = root.children[0];
assert.equal(input.type, "password");
input.focus(); input.setSelectionRange(2, 7);
render({width: 800, height: 600, nodes: [{...text, password: false}]});
assert.equal(root.children[0], input, "show key retains the focused native-linked editor");
assert.equal(input.type, "text", "native Show changes the already mounted HTML password field");
assert.equal(input.value, "fixture-only-value");
assert.equal(input.selectionStart, 2);
render({width: 800, height: 600, nodes: [text]});
assert.equal(input.type, "password", "native Hide remasks the same HTML input");
render({width: 800, height: 600, nodes: [{...text, alignment: 4,
  decoration: "data:image/png;base64,YQ==", decorationRect: {x: 0, y: 0, width: 220, height: 28},
  textRect: {x: 12, y: 4, width: 196, height: 20}}]});
assert.equal(input.style.textAlign, "center", "native line-edit alignment is retained");
assert.equal(input.style.padding, "4px 12px 4px 12px", "native stylesheet content rectangle controls text insets");
assert.match(input.style.backgroundImage, /YQ==/, "native line-edit panel overrides a misleading palette Base role");
assert.equal(input.style.border, "none", "HTML does not add a second frame around the native line-edit panel");
assert.equal(input.type, "password", "native panel decoration leaves password masking on the editable HTML field");
render({width: 800, height: 600, nodes: [{...text, itemView: "table"}]});
let prevented = false;
events.get("keydown")({key: "Tab", target: input, shiftKey: true, preventDefault() { prevented = true; }, stopPropagation() {}});
assert.ok(prevented && requests.at(-1).command === "dialog.key" && requests.at(-1).args.key === "Tab" && requests.at(-1).args.shift,
  "delegate Shift+Tab reaches native commit/navigation instead of moving through unrelated DOM controls");
render({width: 800, height: 600, nodes: [text]});
const count = requests.length;
events.get("keydown")({key: "Tab", target: input, preventDefault() { throw new Error("ordinary Tab was captured"); }});
assert.equal(requests.length, count, "ordinary dialog field Tab remains normal browser navigation");
const list = sandbox.createControl({id: "settings-categories", type: "items"});
let searched = false;
list.listeners.keydown({key: "b", target: list, preventDefault() { searched = true; }, stopPropagation() {}});
assert.ok(searched && requests.at(-1).command === "dialog.key" && requests.at(-1).args.key === "b",
  "typing a first letter in an item view reaches native keyboardSearch");
const beforeComposition = requests.length;
list.listeners.keydown({key: "あ", isComposing: true, target: list,
  preventDefault() { throw new Error("unfinished IME composition was captured"); }});
assert.equal(requests.length, beforeComposition, "unfinished IME composition is not forwarded as a search key");
const comboControl = sandbox.createControl({id: "canvas", type: "combo", editable: false});
const comboData = {id: "canvas", type: "combo", rect, clip: rect, enabled: true, index: -1, placeholder: "None",
  choices: [{text: "First", enabled: true}, {text: "Second", enabled: true}]};
sandbox.updateControl(comboControl, comboData);
const comboSelect = comboControl.querySelector("select");
assert.equal(comboSelect.value, "-1", "unselected combo displays the native placeholder item");
assert.equal(comboSelect.children[0].textContent, "None");
assert.equal(comboSelect.children[0].disabled, true, "placeholder cannot be chosen as a real native item");
comboSelect.value = "1"; comboSelect.listeners.change();
assert.equal(requests.at(-1).args.index, 1, "placeholder does not shift the native index sent for a user choice");
sandbox.updateControl(comboControl, {...comboData, index: 0});
assert.equal(comboSelect.value, "0", "native first item remains first after adding a placeholder");
const fittingCaption = new Element("span");
sandbox.fitChoiceText(fittingCaption, {text:"Native fitting caption", textRect:{width:100}, nativeTextWidth:99});
assert.equal(fittingCaption.firstElementChild.style.transform, `scaleX(${99 / 102})`, "small browser-only overflow is fitted to the actual native glyph width");
sandbox.fitChoiceText(fittingCaption, {text:"Native overlong caption", textRect:{width:100}, nativeTextWidth:120});
assert.equal(fittingCaption.firstElementChild.style.transform, "none", "text too long in Qt is not arbitrarily condensed");
fittingCaption.firstElementChild.measuredWidth = 150;
sandbox.fitChoiceText(fittingCaption, {text:"Different font fallback", textRect:{width:100}, nativeTextWidth:99});
assert.equal(fittingCaption.firstElementChild.style.transform, "none", "large font mismatch is not hidden by extreme compression");
const ime = sandbox.createControl({id: "ime-name", type: "text"});
const beforeIME = requests.length;
ime.value = "ni";
ime.listeners.input({isComposing: true});
assert.equal(requests.length, beforeIME, "IME preedit never changes the native field before composition commits");
ime.listeners.compositionstart({});
ime.listeners.input({isComposing: false});
assert.equal(requests.length, beforeIME, "composition lifecycle also protects input events lacking the composing flag");
ime.value = "你";
ime.listeners.compositionend({});
assert.equal(requests.length, beforeIME + 1, "composition end commits the final value once");
assert.equal(requests.at(-1).args.value, "你");
ime.listeners.input({isComposing: false});
assert.equal(requests.length, beforeIME + 1, "browser's final duplicate input does not commit the composition twice");
ime.value = "你好";
ime.listeners.input({isComposing: false});
assert.equal(requests.length, beforeIME + 2, "ordinary editing resumes after composition");
console.log("PASS: live password visibility, native line-edit panel/alignment, focus/selection retention, native delegate Tab, ordinary Tab, item keyboard search, IME commit");
