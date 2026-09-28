import assert from 'node:assert/strict';
import {prepareExternalDrop,installExternalDrop} from '../../frontend/webview2/ui/external-drop.mjs';
const transfer = (types, data={}, files=[]) => ({types,files,getData: type => data[type] || ''});
const file = new File(['image bytes'], 'picture.png');
const drop = prepareExternalDrop(transfer(['Files'], {}, [file]));
assert.equal(drop?.args.kind,'files');
assert.equal(drop?.files[0],file);
assert.equal(drop?.args.count,1);
assert.deepEqual(prepareExternalDrop(transfer(['text/uri-list'],{'text/uri-list':'# comment\r\nhttps://example.com/image\r\n'}))?.args,
  {kind:'urls',urls:['https://example.com/image']});
assert.deepEqual(prepareExternalDrop(transfer(['text/plain'],{'text/plain':'hello OBS'}))?.args,{kind:'text',text:'hello OBS'});
assert.equal(prepareExternalDrop(transfer(['application/x-obs-source-uuid','text/plain'],{'text/plain':'internal source'})),null);
assert.equal(prepareExternalDrop(transfer(['text/plain'],{'text/plain':'x'.repeat(65537)})),null);
assert.equal(prepareExternalDrop(transfer(['text/uri-list'],{'text/uri-list':'file:///C:/secret.png'})),null);
console.log('PASS: external files, URL/text, internal source exclusion, bounds, local URL rejection');
const handlers = new Map(), messages = [];
const target = {addEventListener: (name, handler) => handlers.set(name, handler), removeEventListener: name => handlers.delete(name)};
let enabled = false;
const dispose = installExternalDrop(target, {enabled: () => enabled,
  transport: {postMessageWithAdditionalObjects: (message, objects) => messages.push({message, objects}), addEventListener() {}, removeEventListener() {}}});
let prevented = 0;
const event = {target: {closest: () => null}, dataTransfer: transfer(['Files'], {}, [file]),
  preventDefault() { ++prevented; }, stopPropagation() {}};
handlers.get('drop')(event);
assert.equal(messages.length, 0, 'disabled native drop state cannot send an import');
enabled = true; handlers.get('drop')(event);
assert.equal(messages.length, 1);
assert.equal(messages[0].objects[0], file, 'enabled dialog forwards the native File object, not a JSON pathname');
assert.equal(messages[0].message.args.count, 1);
event.target.closest = () => ({}); handlers.get('drop')(event);
assert.equal(messages.length, 1, 'text editor drops keep their own editing semantics');
dispose();
assert.equal(handlers.size, 0);
console.log('PASS: dynamic native drop eligibility, trusted File transport, editor exclusion, cleanup');
