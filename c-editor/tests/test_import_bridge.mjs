// Executes the production EM_JS bridge with controlled picker/FileReader edges.
// Real file chooser and FileReader coverage is a separate Chromium integration.
import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import vm from 'node:vm';
const source = readFileSync(new URL('../src/platform_import.c', import.meta.url), 'utf8').replace(/\r\n/g, '\n');
const body = source.match(/EM_JS\(void, platform_import_js_begin, \([^\n]*\), \{\n([\s\S]*?)\n\}\);?/);
assert.ok(body);
const limit = 128 * 1024 * 1024;
let input, reader, cursor, failAllocation, throwPicker, throwReader, heapGrowth;
let results = [], freed = [], timers = [], focus = null;
const context = vm.createContext({ TextEncoder, ArrayBuffer, Uint8Array,
  document: { body: { appendChild() {} }, createElement() {
    input = { files: [], style: {}, setAttribute() {}, remove() { this.removed = true; }, click() { if (throwPicker) throw Error('picker denied'); } };
    return input;
  } },
  window: { addEventListener(_name, fn) { focus = fn; }, removeEventListener(_name, fn) { if (focus === fn) focus = null; } },
  FileReader: class { constructor() { reader = this; } readAsArrayBuffer(file) { if (throwReader) throw Error('provider read failed'); this.file = file; } },
  setTimeout(fn) { timers.push(fn); return timers.length; }, clearTimeout(id) { timers[id - 1] = null; },
});
context._platform_import_web_alloc = size => {
  if (failAllocation) return 0;
  if (heapGrowth && cursor > 64) {
    const next = new Uint8Array(context.HEAPU8.length * 2);
    next.set(context.HEAPU8); context.HEAPU8 = next; heapGrowth = false;
  }
  const result = cursor; cursor += size + 8; return result;
};
context._platform_import_web_free = pointer => freed.push(pointer);
const text = pointer => pointer ? new TextDecoder().decode(context.HEAPU8.subarray(pointer, context.HEAPU8.indexOf(0, pointer))) : '';
context._platform_import_web_finish = (job, status, name, data, size, message) => {
  assert.equal(job, 17);
  assert.ok(input.removed && !focus, 'remove listeners before publishing C completion');
  if (data) assert.equal(context.HEAPU8[data + size], 0);
  results.push({ status, name: text(name), data: data ? Array.from(context.HEAPU8.subarray(data, data + size)) : null, message: text(message) });
};
vm.runInContext(`function begin(job, kind, limit) { ${body[1]} }`, context);
function begin(kind = 2) {
  cursor = 64; results = []; freed = []; timers = []; focus = null; reader = null;
  context.HEAPU8 = new Uint8Array(16384);
  context.begin(17, kind, limit);
}
function select(name, bytes) { input.files = [{ name, size: bytes.byteLength }]; input.onchange(); }

begin();
assert.match(input.accept, /sqlite/);
const value = Uint8Array.from([0, 65, 0, 255]);
select('文献🙂.json', value);
assert.equal(results.length, 0, 'read is asynchronous');
heapGrowth = true; reader.result = value.buffer;
const loaded = reader.onload;
loaded(); loaded();
assert.deepEqual(results, [{ status: 1, name: '文献🙂.json', data: [0, 65, 0, 255], message: '' }]);

begin(0); assert.ok(!input.accept.includes('sqlite'));
input.oncancel(); assert.equal(results[0].status, 2);
begin(3);
for (const extension of ['.json','.html','.htm','.md','.txt']) assert.ok(input.accept.includes(extension));
assert.ok(!input.accept.includes('sqlite'));
select('旧笔记.html',value); reader.result = value.buffer; reader.onload(); assert.equal(results[0].status,1);
begin(); focus(); assert.equal(results.length, 0); timers[0](); assert.equal(results[0].status, 2);
begin(); focus(); input.files = [{ name: 'empty.json', size: 0 }]; timers[0]();
reader.result = new ArrayBuffer(0); reader.onload(); assert.deepEqual(results[0].data, []);

begin(); input.files = [{ name: 'too-large.json', size: limit + 1 }]; input.onchange();
assert.equal(results[0].status, 3); assert.match(results[0].message, /128/); assert.equal(reader, null);
begin(); select('gone.json', value); reader.onerror(); assert.equal(results[0].status, 3);
begin(); select('canceled.json', value); reader.onabort(); assert.equal(results[0].status, 2);
begin(); select('short.json', value); reader.result = new ArrayBuffer(1); reader.onload();
assert.equal(results[0].status, 3); assert.match(results[0].message, /不完整/);
throwReader = true; begin(); select('bad.json', value); throwReader = false;
assert.equal(results[0].status, 3); assert.match(results[0].message, /provider read failed/);
throwPicker = true; begin(); throwPicker = false; assert.equal(results[0].status, 3);
begin(); select('oom.json', value); failAllocation = true; reader.result = value.buffer; reader.onload(); failAllocation = false;
assert.equal(results[0].status, 3);
begin(); select('retry.json', value); reader.result = value.buffer; reader.onload(); assert.equal(results[0].status, 1);
console.log('Import JS bridge: binary/Unicode, memory growth, cancel, size limit, IO failures and retry passed');
