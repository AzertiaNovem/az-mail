// Fails when the built JS chunks import each other in a cycle (static imports only).
// A chunk cycle makes ES module evaluation order depend on which chunk loads first and can crash
// at startup with errors like "Cannot read properties of undefined (reading 'extend')".
import { readdirSync, readFileSync } from 'node:fs';
import { join } from 'node:path';

const dir = new URL('../dist/assets/', import.meta.url).pathname;
const files = readdirSync(dir).filter((f) => f.endsWith('.js'));
const graph = new Map();
for (const f of files) {
  const src = readFileSync(join(dir, f), 'utf8');
  const deps = new Set();
  // static `import ... from"./x.js"` and bare `import"./x.js"` (not dynamic import()).
  for (const m of src.matchAll(/(?:^|[;}\n])\s*(?:import|export)\s*(?:[^'"()]*?\bfrom\s*)?["']\.\/([^"']+\.js)["']/g)) deps.add(m[1]);
  graph.set(f, [...deps].filter((d) => graph.has(d) || files.includes(d)));
}
const state = new Map(); // 1 = visiting, 2 = done
const stack = [];
const cycles = [];
function visit(n) {
  state.set(n, 1);
  stack.push(n);
  for (const d of graph.get(n) ?? []) {
    if (state.get(d) === 1) cycles.push([...stack.slice(stack.indexOf(d)), d]);
    else if (!state.has(d)) visit(d);
  }
  stack.pop();
  state.set(n, 2);
}
for (const f of files) if (!state.has(f)) visit(f);
if (cycles.length) {
  console.error(`chunk import cycles found (${cycles.length}):`);
  for (const c of cycles) console.error('  ' + c.join(' -> '));
  process.exit(1);
}
console.log(`chunk graph OK: ${files.length} chunks, no import cycles`);
