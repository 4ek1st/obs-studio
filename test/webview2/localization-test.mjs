import assert from "node:assert/strict";
import { readFileSync, readdirSync } from "node:fs";
import { fileURLToPath } from "node:url";
import { dirname, join } from "node:path";
import test from "node:test";
import { applyLocalization, translate } from "../../frontend/webview2/ui/localization.mjs";

const root = join(dirname(fileURLToPath(import.meta.url)), "../..");
const read = path => readFileSync(join(root, path), "utf8");
const en = read("frontend/data/locale/en-US.ini");
const localeKeys = new Set([...en.matchAll(/^([\w.]+)=/gm)].map(match => match[1]));

test("all WebView2 labels have an English OBS fallback and a native bridge entry", () => {
  const html = read("frontend/webview2/ui/index.html");
  const app = read("frontend/webview2/ui/app.js");
  const dialog = read("frontend/webview2/ui/dialog.js");
  const mainBridge = read("frontend/webview2/OBSWebView2.cpp");
  const dialogBridge = read("frontend/webview2/QtDialogBridge.cpp");
  const staticKeys = [...html.matchAll(/data-i18n(?:-title|-aria-label)?="([\w.]+)"/g)].map(match => match[1]);
  const appKeys = [...app.matchAll(/\btr\("([\w.]+)"/g)].map(match => match[1]);
  const dialogKeys = [...dialog.matchAll(/\btr\("([\w.]+)"/g)].map(match => match[1]);
  for (const key of new Set([...staticKeys, ...appKeys, "AddScene", "AddSource"])) {
    assert.ok(localeKeys.has(key), `${key} missing from en-US.ini`);
    assert.ok(mainBridge.includes(`"${key}"`), `${key} missing from main WebView2 snapshot`);
  }
  for (const key of dialogKeys) {
    assert.ok(localeKeys.has(key), `${key} missing from en-US.ini`);
    assert.ok(dialogBridge.includes(`"${key}"`), `${key} missing from dialog snapshot`);
  }
});

test("all bundled OBS languages can fall back to the complete English key set", () => {
  const files = readdirSync(join(root, "frontend/data/locale")).filter(name => name.endsWith(".ini"));
  assert.ok(files.length >= 70, "OBS language catalog unexpectedly incomplete");
  for (const file of files) {
    const localized = read(`frontend/data/locale/${file}`);
    assert.match(localized, /^Language=".+"/m, `${file} has no language name`);
    assert.ok(localeKeys.has("WebView2.EmptyAudio"), `${file} cannot fall back to English`);
  }
});

test("localized strings format native placeholders and restore English fallback", () => {
  assert.equal(translate({ Count: "Выбрано: %1" }, "Count", "%1 selected", 2), "Выбрано: 2");
  assert.equal(translate({}, "Count", "%1 selected", 2), "2 selected");
  const attributes = { "data-i18n": "WebView2.MainWindow", "data-i18n-title": "WebView2.MainWindow.Tooltip", title: "Open original" };
  const node = {
    textContent: "Original OBS window", title: "Open original",
    getAttribute(key) { return attributes[key] ?? null; },
    setAttribute(key, value) { attributes[key] = value; this[key] = value; },
  };
  const document = { documentElement: { lang: "en" }, querySelectorAll() { return [node]; } };
  applyLocalization(document, { "WebView2.MainWindow": "Окно OBS", "WebView2.MainWindow.Tooltip": "Открыть OBS" }, "ru-RU");
  assert.equal(node.textContent, "Окно OBS");
  assert.equal(node.title, "Открыть OBS");
  assert.equal(document.documentElement.lang, "ru-RU");
  applyLocalization(document, {}, "de-DE");
  assert.equal(node.textContent, "Original OBS window");
  assert.equal(node.title, "Open original");
  assert.equal(document.documentElement.lang, "de-DE");
});
