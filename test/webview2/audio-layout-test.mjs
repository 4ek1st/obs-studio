import test from "node:test";
import assert from "node:assert/strict";
import { readFile } from "node:fs/promises";
import vm from "node:vm";

const app = await readFile(new URL("../../frontend/webview2/ui/app.js", import.meta.url), "utf8");
function layout(metrics, vertical = true, browserDpr = 1.2) {
  const start = app.indexOf("function applyNativeMixerMetrics(");
  assert.notEqual(start, -1, "renderer must consume the original VolumeControl measurements");
  const source = app.slice(start, app.indexOf("function renderMixer(", start));
  const properties = new Map();
  const container = { style: { setProperty(name, value) { properties.set(name, value); } } };
  vm.runInNewContext(source + "\napplyNativeMixerMetrics(container, metrics, vertical);", {
    container, metrics, vertical, window: { devicePixelRatio: browserDpr },
  });
  return properties;
}
const native = { devicePixelRatio: 1, minimumHeight: 184, meterMinimumHeight: 99,
  categoryHeight: 14, nameHeight: 22, dbHeight: 16, buttonsHeight: 22,
  bodyMinimumHeight: 106, bottomPadding: 4, categoryFontSize: 9,
  nameFontSize: 11, dbFontSize: 11, meterFontSize: 8, buttonWidth: 22 };

test("vertical channels preserve native physical font and width at the observed 1.2 WebView scale", () => {
  const result = layout(native);
  const scale = Number(result.get("--channel-native-scale"));
  assert.ok(Math.abs(scale * 1.2 - 1) < 1e-10);
  assert.ok(Math.abs(110 * scale * 1.2 - 110) < 1e-10, "native110px width must not render132px");
  assert.equal(result.get("--channel-name-font"), "11px");
  assert.equal(result.get("--meter-min-height"), "99px");
  assert.equal(result.get("--channel-min-height"), "184px");
});

test("native layout metrics grow with font accessibility settings without arbitrary maxima", () => {
  const larger = { ...native, minimumHeight: 278, categoryHeight: 21, nameHeight: 33,
    dbHeight: 24, buttonsHeight: 30, nameFontSize: 18, meterMinimumHeight: 154 };
  const result = layout(larger, true, 1);
  assert.equal(result.get("--channel-name-height"), "33px");
  assert.equal(result.get("--channel-name-font"), "18px");
  assert.equal(result.get("--channel-min-height"), "278px");
  assert.equal(result.get("--meter-min-height"), "154px");
});

test("horizontal layout and a missing native metric payload keep their existing scale", () => {
  assert.equal(layout(native, false).get("--channel-native-scale"), "1");
  assert.equal(layout(undefined).get("--channel-native-scale"), "1");
  assert.equal(layout({ ...native, devicePixelRatio: 1.5 }, true, 1.5).get("--channel-native-scale"), "1");
});
