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