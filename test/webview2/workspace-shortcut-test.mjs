import test from "node:test";
import assert from "node:assert/strict";
import { readFile } from "node:fs/promises";
import vm from "node:vm";

// Run the workspace's registered handler, without the browser fixture's old
// extra Undo listener. Requests must originate in the production handler.
const app = await readFile(new URL("../../frontend/webview2/ui/app.js", import.meta.url), "utf8");
const handlerStart = app.indexOf('document.addEventListener("keydown",event=>{');
const handlerSource = app.slice(handlerStart, app.indexOf('\n});', handlerStart) + 4);
function press(overrides = {}, actionOverrides = {}) {
  const calls = [];
  let handler, prevented = false;
  const actions = [
    { id: "undo", name: "actionMainUndo", enabled: true, shortcutKey: "Ctrl+Z", shortcutKeys: ["Ctrl+Z"] },
    { id: "redo", name: "actionMainRedo", enabled: true, shortcutKey: "Ctrl+Shift+Z", shortcutKeys: ["Ctrl+Shift+Z", "Ctrl+Y"] },
  ].map(a => ({ ...a, ...actionOverrides[a.id] }));
  const target = overrides.target ?? { tagName: "INPUT", type: "range", isContentEditable: false,
    closest(selector) { return selector.split(",").some(s => s.trim() === "input") ? this : null; } };
  vm.runInNewContext(handlerSource, { document: { addEventListener(_name, callback) { handler = callback; } },
    actionsByName: new Map(actions.map(a => [a.name, a])), closeMenus() {}, request(...args) { calls.push(args); } });
  handler({ key: "z", code: "KeyZ", ctrlKey: true, shiftKey: false, altKey: false, metaKey: false,
    defaultPrevented: false, isComposing: false, target, preventDefault() { prevented = true; }, ...overrides });
  return { calls: JSON.parse(JSON.stringify(calls)), prevented };
}

test("Undo after adjusting a focused mixer fader reaches the native action", () => {
  assert.deepEqual(press().calls, [["action.invoke", { id: "undo" }]]);
});
test("Cyrillic Ctrl+Я uses the physical Z key for OBS Undo", () => {
  assert.deepEqual(press({ key: "я" }).calls, [["action.invoke", { id: "undo" }]]);
});
test("both native Redo shortcuts are available", () => {
  assert.deepEqual(press({ key: "z", shiftKey: true }).calls, [["action.invoke", { id: "redo" }]]);
  assert.deepEqual(press({ key: "y", code: "KeyY" }).calls, [["action.invoke", { id: "redo" }]]);
});
test("text editing and modal dialogs retain their local Undo", () => {
  for (const target of [
    { tagName: "INPUT", type: "text", closest: () => null },
    { tagName: "TEXTAREA", closest: () => null },
    { tagName: "DIV", isContentEditable: true, closest: () => null },
    { tagName: "BUTTON", closest: selector => selector.includes("dialog") ? {} : null },
  ]) assert.deepEqual(press({ target }).calls, []);
});
test("disabled or scoped actions and handled/IME events are not invoked", () => {
  assert.deepEqual(press({}, { undo: { enabled: false } }).calls, []);
  assert.deepEqual(press({}, { undo: { shortcutGlobal: false } }).calls, []);
  assert.deepEqual(press({ defaultPrevented: true }).calls, []);
  assert.deepEqual(press({ isComposing: true }).calls, []);
  assert.deepEqual(press({ getModifierState: name => name === "AltGraph" }).calls, []);
});
