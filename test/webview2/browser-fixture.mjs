import http from "node:http";
import fs from "node:fs/promises";
import path from "node:path";
import { fileURLToPath } from "node:url";

const directory = path.dirname(fileURLToPath(import.meta.url));
const assets = path.resolve(directory, "../../frontend/webview2/ui");
const allowed = new Set(["index.html", "app.js", "style.css", "bridge.mjs", "external-drop.mjs", "preview-trim.mjs", "localization.mjs"]);
const server = http.createServer(async (request, response) => {
  try {
    const name = decodeURIComponent(new URL(request.url, "http://localhost").pathname).slice(1) || "index.html";
    if (name === "fixture.js") {
      response.setHeader("Content-Type", "text/javascript; charset=utf-8");
      response.end(await fs.readFile(path.join(directory, "fixtures/browser.js")));
      return;
    }
    if (["OpenSans-Regular.ttf", "OpenSans-Bold.ttf", "OpenSans-Italic.ttf"].includes(name)) {
      response.setHeader("Content-Type", "font/ttf");
      response.end(await fs.readFile(path.resolve(assets, "../../forms/fonts", name)));
      return;
    }
    if (!allowed.has(name)) { response.writeHead(404).end(); return; }
    let content = await fs.readFile(path.join(assets, name), "utf8");
    if (name === "index.html")
      content = content.replace('<script type="module" src="app.js"></script>', '<script src="fixture.js"></script><script type="module" src="app.js"></script>');
    response.setHeader("Content-Type", name.endsWith(".html") ? "text/html; charset=utf-8" : name.endsWith(".css") ? "text/css; charset=utf-8" : "text/javascript; charset=utf-8");
    response.end(content);
  } catch {
    response.writeHead(500).end();
  }
});
server.listen(0, "127.0.0.1", () => console.log("Browser fixture: http://127.0.0.1:" + server.address().port));
