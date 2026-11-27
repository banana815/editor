// Executes production EM_JS; real SDL/C ownership and trusted browser events
// are exercised by the separate application and browser integration tests.
import assert from 'node:assert/strict';
import {readFileSync} from 'node:fs';
import vm from 'node:vm';
const source=readFileSync(new URL('../src/platform_web_input.c',import.meta.url),'utf8').replace(/\r\n/g,'\n');
const body=source.match(/EM_JS\(void, init_js, \(void\), \{\n([\s\S]*?)\n\}\)/);assert.ok(body);
const mime='application/x-notelab-document-slice+json',custom=`web ${mime}`,limit=16*1024*1024;
let results=[],freed=[],cursor=64,failAt=0,allocations=0,grow=false,requested=false,begins=0,finishes=[],pasted=[],prepared=1;
const listeners=()=>({events:new Map(),addEventListener(name,fn){const list=this.events.get(name)||[];list.push(fn);this.events.set(name,list);},dispatch(name,e={}){for(const fn of this.events.get(name)||[])fn(e);}});
const input=Object.assign(listeners(),{value:'',style:{},focusCount:0,focus(){this.focusCount++;document.activeElement=this;},blur(){document.activeElement=null;},select(){}});
const canvas=Object.assign(listeners(),{width:800,height:600,getBoundingClientRect(){return {left:0,top:0,width:800,height:600};}});
const document=Object.assign(listeners(),{activeElement:null,getElementById(){return input;}}),window=listeners();
const navigator={clipboard:null},Module={canvas};
let supports=false,constructorRejectCustom=false;
class ClipboardItem {static supports(){if(supports instanceof Error)throw supports;return supports;} constructor(data){if(constructorRejectCustom&&custom in data)throw Error('custom unavailable');this.data=data;}}
const context=vm.createContext({document,window,navigator,Module,ClipboardItem,Blob,TextEncoder,TextDecoder,Uint8Array,ArrayBuffer,Map,Promise,Error,setTimeout,HEAPU8:new Uint8Array(65536)});
const text=p=>p?new TextDecoder("utf-8",{ignoreBOM:true}).decode(context.HEAPU8.subarray(p,context.HEAPU8.indexOf(0,p))):null;
context.UTF8ToString=text;
context._platform_web_input_alloc=size=>{if(++allocations===failAt)return 0;const p=cursor;cursor+=size+8;if(grow||cursor>context.HEAPU8.length){const next=new Uint8Array(Math.max(context.HEAPU8.length*2,cursor));next.set(context.HEAPU8);context.HEAPU8=next;grow=false;}return p;};
context._platform_web_clipboard_free=p=>freed.push(p);
context._platform_web_clipboard_complete=(id,success,...pointers)=>{results.push({id,success,values:pointers.map(text)});for(const p of pointers)if(p)freed.push(p);};
context._platform_web_input_pointer_begin=()=>{begins++;requested=false;};
context._platform_web_input_pointer_finish=()=>requested;
context._platform_web_input_push=()=>assert.fail('unexpected plain input queue');
context._platform_web_clipboard_failed=()=>{};
context._platform_web_clipboard_selection_text=()=>0;
const owned=s=>{const b=new TextEncoder().encode(s),p=context._platform_web_input_alloc(b.length+1);context.HEAPU8.set(b,p);context.HEAPU8[p+b.length]=0;return p;};
const rich=[owned('中文🙂'),owned('<b>中文🙂</b>'),owned('{"type":"documentSlice"}')];
context._platform_web_clipboard_prepare=()=>prepared;
context._platform_web_clipboard_format=i=>rich[i];
context._platform_web_clipboard_finish=accepted=>finishes.push(accepted);
context._platform_web_clipboard_rich_paste_available=()=>1;
context._platform_web_clipboard_paste_data=(...pointers)=>{pasted.push(pointers.map(text));for(const p of pointers)if(p)freed.push(p);return 1;};
vm.runInContext(`(function() { ${body[1]} })();`,context);
Module.editorInputUpdate(1,20,20,500,400);input.focusCount=0;document.activeElement=canvas;
function commandEvent(key,modifiers={}) {
    return {key,code:'',ctrlKey:false,metaKey:false,altKey:false,shiftKey:false,
        prevented:false,stopped:false,...modifiers,
        preventDefault(){this.prevented=true;},stopPropagation(){this.stopped=true;}};
}
for(const focused of [canvas,input]) {
    document.activeElement=focused;
    for(const modifier of ['ctrlKey','metaKey']) {
        for(const key of ['0','1','2','3','4','5','6']) {
            const e=commandEvent(key,{[modifier]:true,altKey:true}); window.dispatch('keydown',e);
            assert.ok(e.prevented,`heading ${key} cancels browser action`);
            assert.ok(!e.stopped,'heading shortcut still reaches SDL');
        }
        for(const key of ['F','O','T']) {
            const e=commandEvent(key,{[modifier]:true,shiftKey:true}); window.dispatch('keydown',e);
            assert.ok(e.prevented,`workspace ${key} cancels browser action`);
            assert.ok(!e.stopped,'workspace shortcut still reaches SDL');
        }
    }
}
for(const e of [commandEvent('1',{ctrlKey:true}),commandEvent('f',{shiftKey:true}),
    commandEvent('7',{ctrlKey:true,altKey:true}),commandEvent('1',{ctrlKey:true,altKey:true,isComposing:true})]) {
    window.dispatch('keydown',e);assert.ok(!e.prevented&&!e.stopped,'unowned shortcuts and IME remain untouched');
}
document.activeElement=null;
const outside=commandEvent('F',{ctrlKey:true,shiftKey:true});window.dispatch('keydown',outside);
assert.ok(!outside.prevented,'shortcuts outside editor remain browser-owned');
document.activeElement=canvas;
canvas.dispatch('pointerdown',{pointerType:'touch'});assert.equal(begins,1);
canvas.dispatch('pointerup',{pointerType:'touch',clientX:30,clientY:30});assert.equal(input.focusCount,0,'swipe does not open keyboard');
requested=true;canvas.dispatch('pointerup',{pointerType:'touch'});assert.equal(input.focusCount,1);
requested=false;canvas.dispatch('pointerup',{pointerType:'mouse',clientX:30,clientY:30});assert.equal(input.focusCount,2,'mouse behavior remains');
const flush=async()=>{for(let i=0;i<8;i++)await new Promise(resolve=>setTimeout(resolve,0));};
const start=(id,writing,plain='中文🙂',html='<b>中文🙂</b>',typed='{"version":1}')=>Module.editorClipboardStart(id,writing,plain,html,typed);
let written;
navigator.clipboard={write(items){written=items[0].data;return Promise.resolve();}};
assert.equal(start(1,true),1);assert.equal(results.length,0);await flush();assert.equal(results[0].success,1);
assert.deepEqual(Object.keys(written).sort(),['text/html','text/plain']);assert.equal(await written['text/plain'].text(),'中文🙂');
supports=true;start(2,true);await flush();assert.ok(custom in written);
supports=new Error('unsupported query');start(3,true);await flush();assert.equal(results.at(-1).success,1);assert.ok(!(custom in written));
supports=true;constructorRejectCustom=true;start(4,true);await flush();assert.equal(results.at(-1).success,1);assert.ok(!(custom in written));constructorRejectCustom=false;
let writes=0;supports=true;navigator.clipboard={write(items){writes++;written=items[0].data;if(custom in written){const e=Error('custom unavailable');e.name='NotSupportedError';return Promise.reject(e);}return Promise.resolve();}};
start(40,true);await flush();assert.equal(writes,2);assert.equal(results.at(-1).success,1);assert.ok(!(custom in written));
writes=0;navigator.clipboard={write(){writes++;return Promise.reject(Error('permission denied'));}};start(41,true);await flush();assert.equal(writes,1);assert.equal(results.at(-1).success,0,'permission failures are not retried');
let rejectCustom;writes=0;navigator.clipboard={write(){writes++;return new Promise((_resolve,reject)=>{rejectCustom=reject;});}};start(42,true);Module.editorClipboardCancel();const unsupported=Error('custom unavailable');unsupported.name='NotSupportedError';rejectCustom(unsupported);await flush();assert.equal(writes,1,'canceled request cannot start a fallback write');
navigator.clipboard={write(){throw Error('denied');}};start(5,true);assert.notEqual(results.at(-1).id,5);await flush();assert.equal(results.at(-1).success,0);assert.match(results.at(-1).values[3],/denied/);
navigator.clipboard=null;assert.equal(start(6,true),0);assert.notEqual(results.at(-1).id,6);
const blobs={'text/plain':new Blob(['中文🙂']), 'text/html':new Blob(['<i>中文🙂</i>']), [custom]:new Blob(['{"version":1}'])};
navigator.clipboard={read:()=>Promise.resolve([{types:Object.keys(blobs),getType:type=>Promise.resolve(blobs[type])}])};
grow=true;start(7,false);await flush();assert.deepEqual(results.at(-1),{id:7,success:1,values:['中文🙂','<i>中文🙂</i>','{"version":1}',null]});
blobs['text/plain']=new Blob(['\uFEFFretain leading BOM']);start(70,false);await flush();assert.equal(results.at(-1).values[0],'\uFEFFretain leading BOM');
blobs['text/plain']=new Blob([new Uint8Array([0xff])]);start(8,false);await flush();assert.equal(results.at(-1).success,0);
blobs['text/plain']=new Blob(['NUL\0tail']);start(9,false);await flush();assert.equal(results.at(-1).success,0);
blobs['text/plain']={size:limit+1,arrayBuffer(){assert.fail('oversize blob must not be read');}};start(10,false);await flush();assert.equal(results.at(-1).success,0);
blobs['text/plain']=new Blob(['good']);failAt=allocations+2;const freedBefore=freed.length;start(11,false);await flush();assert.equal(results.at(-1).success,0);assert.ok(freed.length>freedBefore,'partial buffers released');failAt=0;
let resolveWrite;navigator.clipboard={write:()=>new Promise(resolve=>{resolveWrite=resolve;})};start(12,true);Module.editorClipboardCancel();resolveWrite();await flush();assert.ok(!results.some(r=>r.id===12),'late promise after C cancellation is ignored');
let fallbackText=null;navigator.clipboard={writeText:text=>{fallbackText=text;return Promise.resolve();}};
assert.equal(start(120,true),0,'rich write cannot silently fall back to plain text');
assert.equal(fallbackText,null);const savedItem=context.ClipboardItem;context.ClipboardItem=undefined;
navigator.clipboard={write(){assert.fail('missing ClipboardItem must reject before writing');},writeText(){assert.fail('rich text must not use writeText');}};
assert.equal(start(122,true),0);context.ClipboardItem=savedItem;
navigator.clipboard={writeText:text=>{fallbackText=text;return Promise.resolve();}};
assert.equal(start(121,true,'prompt text',null,null),1);await flush();assert.equal(fallbackText,'prompt text');assert.equal(results.at(-1).success,1);
navigator.clipboard={readText:()=>Promise.resolve('fallback')};start(13,false);await flush();assert.equal(results.at(-1).values[0],'fallback');
const event=data=>({clipboardData:data,preventDefault(){},stopPropagation(){}});
const transfer=()=>({data:new Map(),get types(){return [...this.data.keys()];},setData(type,value){this.data.set(type,value);},getData(type){return this.data.get(type)||'';}});
document.activeElement=canvas;
const dt=transfer();document.dispatch('copy',event(dt));assert.equal(finishes.at(-1),1);assert.equal(dt.getData(mime),text(rich[2]));
const partial=transfer();partial.setData=function(type,value){if(type!=='text/html')this.data.set(type,value);};document.dispatch('cut',event(partial));assert.equal(finishes.at(-1),0,'plain accepted but HTML rejected cannot cut');
const noCustom=transfer();noCustom.setData=function(type,value){if(type===mime)throw Error('custom unavailable');this.data.set(type,value);};document.dispatch('cut',event(noCustom));assert.equal(finishes.at(-1),1,'HTML envelope supports custom MIME fallback');
grow=true;document.dispatch('paste',event(dt));assert.deepEqual(pasted.at(-1),rich.map(text));
const pastedBefore=pasted.length;failAt=allocations+2;document.dispatch('paste',event(dt));assert.equal(pasted.length,pastedBefore);failAt=0;
const invalid=transfer();invalid.setData('text/plain','bad\0text');document.dispatch('paste',event(invalid));assert.equal(pasted.length,pastedBefore);
prepared=0;const count=finishes.length;document.dispatch('copy',event(transfer()));assert.equal(finishes.length,count,'failed prepare does not own a finish');
console.log('Web input bridge: writing shortcuts, trusted touch focus, rich DOM clipboard acceptance, async formats/Unicode, optional custom MIME, UTF-8/NUL/size errors, allocation cleanup and late cancellation passed');
