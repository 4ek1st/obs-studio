// Compare inventories from the same disposable profile and executable.
// This supplements behavior tests; equal inventories do not prove equal behavior.
import fs from 'node:fs';
import path from 'node:path';

const [nativeDirectory, webDirectory, output] = process.argv.slice(2);
if (!nativeDirectory || !webDirectory || !output) {
  console.error('Usage: node compare-parity-audits.mjs <native-dir> <web-dir> <output.json>');
  process.exit(2);
}
const read = directory => JSON.parse(fs.readFileSync(path.join(directory, 'audit.json'), 'utf8'));
const native = read(nativeDirectory), web = read(webDirectory);
const counts = (items, key) => {
  const result = new Map();
  for (const item of items) { const id = key(item); result.set(id, (result.get(id) || 0) + 1); }
  return result;
};
const diff = (left, right) => [...new Set([...left.keys(), ...right.keys()])].sort()
  .filter(key => left.get(key) !== right.get(key))
  .map(key => ({ key, native: left.get(key) || 0, webview2: right.get(key) || 0 }));
const nodeKey = node => JSON.stringify([node.name, node.class, node.type, node.enabled]);
const actionDifferences = diff(counts(native.actions, a => JSON.stringify(a)), counts(web.actions, a => JSON.stringify(a)));
const nativeCases = new Map(native.dialogs.map(dialog => [dialog.case, dialog]));
const webCases = new Map(web.dialogs.map(dialog => [dialog.case, dialog]));
const dialogs = [...new Set([...nativeCases.keys(), ...webCases.keys()])].map(key => {
  const n = nativeCases.get(key), w = webCases.get(key);
  return { case: key, nativeNodes: n?.nodes.length ?? null, webNodes: w?.nodes.length ?? null,
    differences: n && w ? diff(counts(n.nodes, nodeKey), counts(w.nodes, nodeKey)) : [{ missing: n ? 'webview2' : 'native' }] };
});
const result = {
  scope: 'Named actions including state, visible widget contracts including class/type/enabled; excludes random IDs, geometry and variable text. Does not certify gesture or rendered pixel parity.',
  nativePassed: native.passed, webPassed: web.passed,
  nativeActionCount: native.actions.length, webActionCount: web.actions.length,
  actionDifferences, dialogs,
};
fs.writeFileSync(output, JSON.stringify(result, null, 2) + '\n');
console.log(JSON.stringify({ nativePassed: result.nativePassed, webPassed: result.webPassed,
  actions: [result.nativeActionCount, result.webActionCount], actionDifferences: actionDifferences.length,
  dialogs: dialogs.length, dialogsWithDifferences: dialogs.filter(d => d.differences.length).length }));
process.exitCode = !native.passed || !web.passed || actionDifferences.length || dialogs.some(d => d.differences.length) ? 1 : 0;
