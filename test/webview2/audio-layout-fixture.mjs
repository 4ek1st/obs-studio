import http from "node:http";
import fs from "node:fs/promises";
import path from "node:path";
import { fileURLToPath } from "node:url";

// Isolated browser layout fixture. It never connects to OBS or an audio device.
// Pass a native mixer-vertical.json to use measurements from a completed run.
const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), "../..");
const snapshot = process.argv[2] && path.resolve(process.argv[2]);
const allowed = new Set(["index.html", "app.js", "style.css", "bridge.mjs", "external-drop.mjs"]);
const metrics = { devicePixelRatio: 1, minimumHeight: 184, meterMinimumHeight: 99,
  categoryHeight: 14, nameHeight: 22, dbHeight: 16, buttonsHeight: 22,
  bodyMinimumHeight: 106, bottomPadding: 4, categoryFontSize: 9,
  nameFontSize: 11, dbFontSize: 11, meterFontSize: 8, buttonWidth: 22 };
const server = http.createServer(async (request, response) => {
  try {
    const name = new URL(request.url, "http://localhost").pathname.slice(1);
    let body, type = "text/javascript";
    if (!name) {
      type = "text/html";
      body = '<!doctype html><meta charset="utf-8"><title>Native mixer geometry regression</title><style>body{background:#222;color:#eee;font:13px sans-serif}iframe{border:1px solid #777;display:block}pre{white-space:pre-wrap}</style><h2>Compact: native host 982×237</h2><iframe id="compact" src="/index.html"></iframe><h2>Tall: native host 982×450</h2><iframe id="tall" src="/index.html"></iframe><pre id="result">Waiting for the production renderer…</pre><script src="/measure.js"></script>';
    } else if (name === "measure.js") {
      body = `for(const [id,height]of[["compact",237],["tall",450]]){const frame=document.getElementById(id);frame.width=982/devicePixelRatio;frame.height=height/devicePixelRatio;}
      setInterval(()=>{const results={};for(const id of ["compact","tall"]){const doc=document.getElementById(id).contentDocument,mixer=doc?.getElementById("mixer"),channel=mixer?.querySelector(".audio-channel");if(!channel)continue;const bounds=mixer.getBoundingClientRect(),inside=element=>{const r=element.getBoundingClientRect();return r.top>=bounds.top-.5&&r.bottom<=bounds.bottom+.5};results[id]={widthPixels:channel.getBoundingClientRect().width*devicePixelRatio,viewportPixels:bounds.height*devicePixelRatio,channelPixels:channel.getBoundingClientRect().height*devicePixelRatio,scrollHeight:mixer.scrollHeight,clientHeight:mixer.clientHeight,muteVisible:inside(channel.querySelector(".audio-mute")),monitorVisible:inside(channel.querySelector(".audio-monitor")),lastTickVisible:inside(channel.querySelector(".meter-scale span:last-child"))};}document.getElementById("result").textContent=JSON.stringify(results,null,2)},500);`;
    } else if (name === "fixture.js") {
      const channel = snapshot ? JSON.parse(await fs.readFile(snapshot, "utf8")) : { name: "WebView2 stereo fixture", uuid: "fixture", volume: 1, db: 0, enabled: true, channels: 2, preferredWidth: 110, category: "Активен", monitoring: 0 };
      channel.verticalMetrics ??= metrics;
      body = (await fs.readFile(path.join(root, "test/webview2/fixtures/browser.js"), "utf8"))
        .replace('window.addEventListener("load", publish);', `state.workspace.nativeDocking=true;state.workspace.verticalMixer=true;state.audio=[${JSON.stringify(channel)}];state.appearance={fontSize:13,fontFamily:"Open Sans"};window.addEventListener("load",()=>{send({version:1,event:"workspace.panel",data:{name:"mixerDock",nativeDocking:true}});publish();});`);
    } else if (allowed.has(name)) {
      body = await fs.readFile(path.join(root, "frontend/webview2/ui", name), "utf8");
      if (name === "index.html") body = body.replace('<script type="module" src="app.js"></script>', '<script src="fixture.js"></script><script type="module" src="app.js"></script>');
      type = name.endsWith(".html") ? "text/html" : name.endsWith(".css") ? "text/css" : type;
    } else if (["OpenSans-Regular.ttf", "OpenSans-Bold.ttf", "OpenSans-Italic.ttf"].includes(name)) {
      body = await fs.readFile(path.join(root, "frontend/forms/fonts", name)); type = "font/ttf";
    } else { response.writeHead(404).end(); return; }
    response.setHeader("Content-Type", type + (type.startsWith("text/") ? "; charset=utf-8" : ""));
    response.end(body);
  } catch (error) { response.writeHead(500).end(String(error)); }
});
server.listen(0, "127.0.0.1", () => console.log(`Mixer geometry fixture: http://127.0.0.1:${server.address().port}`));
