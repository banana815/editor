// Execute image paste/drop's production bridge with isolated in-memory files.
import assert from 'node:assert/strict';
import {readFileSync} from 'node:fs';
import vm from 'node:vm';
const source=readFileSync(new URL('../src/app_images.inc',import.meta.url),'utf8').replace(/\r\n/g,'\n');
const body=source.match(/EM_JS\(void,images_web_install,\(\),\{\n([\s\S]*?)\n\}\);/);
assert.ok(body);
const handlers={},events=[];let allowed=true,allocate=true;
const heap=new Uint8Array(128);
const Module={canvas:{addEventListener(type,fn){handlers[type]=fn;}}};
const ctx=vm.createContext({Module,Uint8Array,Array,HEAPU8:heap,
    _platform_web_input_pointer_finish(){events.push('flush');},
    _images_web_begin(){events.push('begin');return allowed?1:0;},
    _images_web_alloc(n){events.push(['allocate',n]);return allocate?8:0;},
    _images_web_finish(ptr,n){events.push(['finish',ptr,Array.from(heap.slice(ptr,ptr+n))]);}});
vm.runInContext(`(function(){${body[1]}})()`,ctx);
const file={type:'image/png',size:4,arrayBuffer:async()=>new Uint8Array([137,80,0,255]).buffer};
assert.equal(Module.noteLabImagePaste({clipboardData:{items:[{kind:'file',type:'image/png',getAsFile:()=>file}]}}),true);
await new Promise(resolve=>setImmediate(resolve));
assert.deepEqual(events,['flush','begin',['allocate',4],['finish',8,[137,80,0,255]]]);
events.length=0;allowed=false;
assert.equal(Module.noteLabImagePaste({clipboardData:{items:[{kind:'file',type:'image/png',getAsFile:()=>file}]}}),false);
assert.deepEqual(events,['flush','begin']);
allowed=true;events.length=0;file.size=16777217;
assert.equal(Module.noteLabImagePaste({clipboardData:{items:[{kind:'file',type:'image/png',getAsFile:()=>file}]}}),true);
assert.deepEqual(events,['flush','begin',['finish',0,[]]]);
file.size=4;events.length=0;allocate=false;
let prevented=false;handlers.drop({dataTransfer:{files:[file]},preventDefault(){prevented=true;},stopPropagation(){}});
await new Promise(resolve=>setImmediate(resolve));
assert.ok(prevented);assert.equal(events.at(-1)[0],'finish');assert.equal(events.at(-1)[1],0);
assert.equal(Module.noteLabImagePaste({clipboardData:{items:[{kind:'string',type:'text/plain'}]}}),false);
console.log('Image clipboard/drop bridge preserves binary bytes, rejects inactive/oversize requests and reports allocation failure');
