window.chrome.webview.addEventListener("message", event => {
  if (event.data.event !== "test.ping") return;
  window.chrome.webview.postMessage({version: 1, id: "1", command: "test.echo", args: {token: event.data.token}});
  location.assign("https://example.com/");
  setTimeout(() => {
    window.chrome.webview.postMessage({version: 1, id: "2", command: "test.stayedLocal", args: {origin: location.origin}});
  }, 200);
});
