import test from "node:test";
import assert from "node:assert/strict";
import { createBridge } from "../../frontend/webview2/ui/bridge.mjs";

function channel() {
  let receive;
  const sent = [];
  const transport = {
    postMessage(value) { sent.push(value); },
    addEventListener(type, callback) { receive = callback; },
    removeEventListener(type, callback) { if (receive === callback) receive = null; },
  };
  return { transport, sent, reply(value) { receive?.({ data: value }); } };
}

test("matches responses to requests even when native replies arrive out of order", async () => {
  const c = channel();
  const bridge = createBridge(c.transport, { timeoutMs: 100 });
  const first = bridge.request("state.get", {});
  const second = bridge.request("scene.select", { uuid: "scene-2" });
  assert.equal(c.sent[0].version, 1);
  assert.equal(c.sent[1].command, "scene.select");
  c.reply({ version: 1, id: c.sent[1].id, ok: true, result: { scene: "scene-2" } });
  c.reply({ version: 1, id: c.sent[0].id, ok: true, result: { scenes: [] } });
  assert.deepEqual(await first, { scenes: [] });
  assert.deepEqual(await second, { scene: "scene-2" });
  bridge.dispose();
});

test("returns native error details and ignores unrelated replies", async () => {
  const c = channel();
  const bridge = createBridge(c.transport, { timeoutMs: 100 });
  const pending = bridge.request("action.invoke", { id: "missing" });
  c.reply({ version: 1, id: "stale", ok: true, result: null });
  c.reply({ version: 1, id: c.sent[0].id, ok: false, error: { code: "Unavailable", message: "Action is no longer available" } });
  await assert.rejects(pending, error => error.code === "Unavailable" && error.message === "Action is no longer available");
  bridge.dispose();
});

test("times out requests and refuses responses with another protocol version", async () => {
  const c = channel();
  const bridge = createBridge(c.transport, { timeoutMs: 15 });
  const pending = bridge.request("state.get", {});
  c.reply({ version: 2, id: c.sent[0].id, ok: true, result: { wrong: true } });
  await assert.rejects(pending, error => error.code === "Timeout");
  bridge.dispose();
});

test("delivers unsolicited state updates independently of replies", () => {
  const c = channel();
  const bridge = createBridge(c.transport);
  let state;
  const unsubscribe = bridge.subscribe("state.changed", value => state = value);
  c.reply({ version: 1, event: "state.changed", data: { currentScene: "studio" } });
  assert.deepEqual(state, { currentScene: "studio" });
  unsubscribe();
  c.reply({ version: 1, event: "state.changed", data: { currentScene: "other" } });
  assert.equal(state.currentScene, "studio");
  bridge.dispose();
});

test("disposing rejects pending work and prevents new native commands", async () => {
  const c = channel();
  const bridge = createBridge(c.transport);
  const pending = bridge.request("state.get", {});
  bridge.dispose();
  await assert.rejects(pending, error => error.code === "Closed");
  await assert.rejects(bridge.request("state.get", {}), error => error.code === "Closed");
  assert.equal(c.sent.length, 1);
});
