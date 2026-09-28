const internalType = 'application/x-obs-source-uuid';
const supported = types => types.some(type => ['Files', 'text/uri-list', 'text/plain'].includes(type));

export function prepareExternalDrop(transfer) {
  if (!transfer || Array.from(transfer.types || []).includes(internalType)) return null;
  const files = Array.from(transfer.files || []);
  if (files.length) return files.length <= 128 ? {args:{kind:'files',count:files.length},files} : null;
  const uriList = transfer.getData('text/uri-list');
  let args;
  if (uriList) {
    const urls = uriList.split(/\r?\n/).map(line => line.trim()).filter(line => line && !line.startsWith('#'));
    if (!urls.length || urls.length > 128) return null;
    for (const value of urls) {
      let url; try { url = new URL(value); } catch { return null; }
      if (value.length > 8192 || !['https:','http:'].includes(url.protocol) || !url.hostname) return null;
    }
    args = {kind:'urls',urls};
  } else {
    const text = transfer.getData('text/plain');
    if (!text || text.includes('\0')) return null;
    args = {kind:'text',text};
  }
  if (new TextEncoder().encode(JSON.stringify(args)).length > 60000) return null;
  return {args,files:[]};
}

// Capture is limited to external drags. Source/scene reorder dragstart originates
// inside this document and must reach its existing row handlers unchanged.
export function installExternalDrop(target, {transport = globalThis.chrome?.webview, onError = () => {}} = {}) {
  let internalDrag = false, sequence = 0;
  const session = 'drop:' + globalThis.crypto.randomUUID();
  const pending = new Map();
  const editable = event => event.target?.closest?.('input,textarea,[contenteditable="true"],dialog');
  const eligible = event => !internalDrag && !editable(event) &&
    !Array.from(event.dataTransfer?.types || []).includes(internalType) && supported(Array.from(event.dataTransfer?.types || []));
  const start = () => { internalDrag = true; };
  const end = () => { internalDrag = false; };
  const over = event => { if (eligible(event)) { event.preventDefault(); event.dataTransfer.dropEffect = 'copy'; } };
  const receive = ({data}) => {
    if (data?.version !== 1 || !pending.has(data.id)) return;
    clearTimeout(pending.get(data.id)); pending.delete(data.id);
    if (!data.ok) onError(new Error(data.error?.message || 'OBS could not import the dropped item.'));
  };
  const drop = event => {
    if (!eligible(event)) return;
    event.preventDefault(); event.stopPropagation();
    const payload = prepareExternalDrop(event.dataTransfer);
    if (!payload) { onError(new Error('The drop is empty, too large, or contains an unsupported link.')); return; }
    const id = `${session}:${++sequence}`;
    const message = {version:1,id,command:'external.drop',args:payload.args};
    const timeout = setTimeout(() => { pending.delete(id); onError(new Error('OBS did not acknowledge the external drop.')); },15000);
    pending.set(id,timeout);
    try {
      if (payload.files.length) {
        if (!transport?.postMessageWithAdditionalObjects) throw new Error('Update Microsoft Edge WebView2 Runtime to import dropped files.');
        transport.postMessageWithAdditionalObjects(message,payload.files);
      } else {
        if (!transport?.postMessage) throw new Error('The native OBS connection is unavailable.');
        transport.postMessage(message);
      }
    } catch(error) { clearTimeout(timeout); pending.delete(id); onError(error); }
  };
  target.addEventListener('dragstart',start,true);
  target.addEventListener('dragend',end,true);
  target.addEventListener('dragover',over,true);
  target.addEventListener('drop',drop,true);
  transport?.addEventListener('message',receive);
  return () => {
    target.removeEventListener('dragstart',start,true); target.removeEventListener('dragend',end,true);
    target.removeEventListener('dragover',over,true); target.removeEventListener('drop',drop,true);
    transport?.removeEventListener('message',receive);
    for (const timer of pending.values()) clearTimeout(timer);
    pending.clear();
  };
}
