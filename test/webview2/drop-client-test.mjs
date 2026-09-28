import assert from 'node:assert/strict';
import {prepareExternalDrop} from '../../frontend/webview2/ui/external-drop.mjs';
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
