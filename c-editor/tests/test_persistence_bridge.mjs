// Tests the actual EM_JS bodies with a controllable FS boundary in Node.
// This checks the bridge contract; it does not claim browser/IndexedDB coverage.
import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import vm from "node:vm";

const source = readFileSync(new URL("../src/platform_persistence.c", import.meta.url), "utf8");

function bridgeBody(name) {
  const match = source.match(new RegExp(
    `EM_JS\\(void, ${name}, \\([^\\n]*\\), \\{\\n([\\s\\S]*?)\\n\\}\\);`,
  ));
  assert.ok(match, `EM_JS body ${name} must be present in the production source`);
  return match[1];
}

const messageOffset = 64;
const capacity = 768;
const results = [];
const context = vm.createContext({
  Module: {},
  HEAPU8: new Uint8Array(2048).fill(0x55),
  TextEncoder,
  Promise,
  FS: undefined,
});

context._platform_persistence_complete = (success) => {
  const heap = context.HEAPU8;
  const end = heap.indexOf(0, messageOffset);
  assert.ok(end >= messageOffset && end < messageOffset + capacity, "error must be terminated within its buffer");
  assert.equal(heap[messageOffset - 1], 0x55, "bridge must not overwrite preceding memory");
  assert.equal(heap[messageOffset + capacity], 0x55, "bridge must not overwrite following memory");
  const message = new TextDecoder("utf-8", { fatal: true }).decode(heap.subarray(messageOffset, end));
  results.push({ success, message });
};

vm.runInContext(`
  function begin(message, capacity) { ${bridgeBody("platform_storage_js_begin")} }
  function setPending(pending) { ${bridgeBody("platform_storage_js_pending")} }
`, context);

const tick = () => new Promise((resolve) => setImmediate(resolve));
const begin = () => context.begin(messageOffset, capacity);

context.setPending(1);
assert.equal(context.Module.editorPersistencePending, true);
context.setPending(0);
assert.equal(context.Module.editorPersistencePending, false);

let finish;
context.FS = { syncfs(populate, callback) {
  assert.equal(populate, false, "save must flush memory to IndexedDB, not reload it");
  finish = callback;
} };
begin();
assert.equal(results.length, 0);
finish(null);
assert.equal(results.length, 0, "even synchronous FS completion must defer the C callback");
finish(new Error("duplicate completion"));
await tick();
assert.deepEqual(results.splice(0), [{ success: 1, message: "" }]);

context.FS = { syncfs() { throw new Error("quota exceeded"); } };
begin();
assert.equal(results.length, 0);
await tick();
assert.equal(results.length, 1);
assert.equal(results[0].success, 0);
assert.match(results[0].message, /quota exceeded/);
assert.match(results[0].message, /Keep this page open/);
results.length = 0;

context.FS = { syncfs(_populate, callback) {
  callback({ target: { error: { message: "存储失败".repeat(200) } } });
} };
begin();
await tick();
assert.equal(results.length, 1);
assert.equal(results[0].success, 0);
assert.match(results[0].message, /存储失败/);
assert.ok(new TextEncoder().encode(results[0].message).length < capacity);
results.length = 0;

for (const missingFilesystem of [undefined, {}]) {
  context.FS = missingFilesystem;
  begin();
  await tick();
  assert.equal(results.length, 1);
  assert.equal(results[0].success, 0);
  assert.match(results[0].message, /Persistent filesystem is not available/);
  results.length = 0;
}

// A growing WASM memory replaces HEAPU8 while a sync is in flight. Completion
// must write to the current heap view rather than capture a stale typed array.
context.FS = { syncfs(_populate, callback) { finish = callback; } };
begin();
context.HEAPU8 = new Uint8Array(4096).fill(0x55);
finish(new Error("transaction aborted"));
await tick();
assert.equal(results.length, 1);
assert.match(results[0].message, /transaction aborted/);
results.length = 0;

// Recovering after an error must not expose the previous batch's error text.
begin();
finish(null);
finish(null);
await tick();
assert.deepEqual(results, [{ success: 1, message: "" }]);

console.log("persistence JS bridge: async completion, errors, duplicate suppression, UTF-8, and heap growth passed");
