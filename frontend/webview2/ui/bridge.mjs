// NavigationCompleted only means that HTML loaded, not that OBS state, local
// fonts and QStyle images reached the compositor. Present once after that work.
export function createPresentation(bridge) {
  let started = false;
  return async function present() {
    if (started) return;
    started = true;
    // Flush styles from the first state so newly used font faces are included.
    document.documentElement.getBoundingClientRect();
    await document.fonts?.ready;
    await Promise.all([...document.images].map(image => image.decode?.().catch(() => {})));
    await new Promise(requestAnimationFrame);
    document.documentElement.dataset.presented = "true";
    // A frame callback runs before paint. The following frame acknowledges the
    // completed paint, including the layout changes from font loadingdone.
    await new Promise(requestAnimationFrame);
    await bridge.request("ui.present");
  };
}

export function createBridge(transport, { timeoutMs = 15000 } = {}) {
  if (!transport?.postMessage || !transport?.addEventListener) {
    throw new Error("The native OBS connection is unavailable.");
  }
  const pending = new Map();
  const listeners = new Map();
  const session = globalThis.crypto.randomUUID();
  let sequence = 0;
  let closed = false;
  const failure = (code, message) => Object.assign(new Error(message), { code });

  function receive({ data }) {
    if (!data || data.version !== 1 || typeof data !== "object") return;
    if (typeof data.event === "string") {
      for (const listener of listeners.get(data.event) ?? []) listener(data.data);
      return;
    }
    const request = pending.get(data.id);
    if (!request || (data.ok !== true && data.ok !== false)) return;
    clearTimeout(request.timer);
    pending.delete(data.id);
    if (data.ok) request.resolve(data.result);
    else request.reject(failure(data.error?.code ?? "NativeError", data.error?.message ?? "OBS could not complete the command."));
  }
  transport.addEventListener("message", receive);

  return {
    request(command, args = {}) {
      if (closed) return Promise.reject(failure("Closed", "The OBS connection is closed."));
      const id = `${session}:${++sequence}`;
      return new Promise((resolve, reject) => {
        const timer = setTimeout(() => {
          pending.delete(id);
          reject(failure("Timeout", "OBS did not respond in time."));
        }, timeoutMs);
        pending.set(id, { resolve, reject, timer });
        try {
          transport.postMessage({ version: 1, id, command, args });
        } catch (error) {
          clearTimeout(timer);
          pending.delete(id);
          reject(error);
        }
      });
    },
    subscribe(event, listener) {
      if (!listeners.has(event)) listeners.set(event, new Set());
      listeners.get(event).add(listener);
      return () => listeners.get(event)?.delete(listener);
    },
    dispose() {
      if (closed) return;
      closed = true;
      transport.removeEventListener("message", receive);
      for (const request of pending.values()) {
        clearTimeout(request.timer);
        request.reject(failure("Closed", "The OBS connection is closed."));
      }
      pending.clear();
      listeners.clear();
    },
  };
}
