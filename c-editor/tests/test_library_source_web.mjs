// Runs the production EM_JS transport and C guard in real WebAssembly.
import assert from 'node:assert/strict';
import {pathToFileURL} from 'node:url';
import {resolve} from 'node:path';
const factory=(await import(pathToFileURL(resolve(process.argv[2])))).default;
const app=await factory();assert.equal(app._library_test_init(),1);
const start=app.cwrap('library_test_start','number',['string','number','number']);
const status=job=>app._library_test_status(job);
const pause=ms=>new Promise(r=>setTimeout(r,ms));
const drain=async()=>{for(let n=0;n<8;n++)await pause(0);};
const text=()=>new TextDecoder().decode(app.HEAPU8.slice(app._library_test_bytes(),app._library_test_bytes()+app._library_test_size()));
const good='[{"title":"中文🙂"}]';
let observed;
globalThis.fetch=async(url,options)=>{observed={url,options};return new Response(good,{status:200});};
let job=start('http://localhost:23119/test',1024,1000);assert.ok(job);await drain();
assert.equal(status(job),1);assert.equal(app._library_test_take(job),1);app._library_test_free(job);assert.equal(text(),good);app._library_test_clear();
assert.equal(observed.options.credentials,'omit');assert.equal(observed.options.redirect,'error');assert.equal(observed.options.cache,'no-store');assert.equal(observed.options.referrerPolicy,'no-referrer');
for(const response of [new Response('unauthorized',{status:401}),new Response('redirect',{status:302}),new Response('x'.repeat(33))]) {
 globalThis.fetch=async()=>response;job=start('http://localhost/test',32,1000);await drain();assert.equal(status(job),3);assert.equal(app._library_test_take(job),0);app._library_test_free(job);
}
globalThis.fetch=async()=>new Response(null,{status:204});job=start('http://localhost/test',32,1000);await drain();assert.equal(status(job),1);app._library_test_free(job);
globalThis.fetch=()=>{throw Error('network denied');};job=start('http://localhost/test',32,1000);await drain();assert.equal(status(job),3);app._library_test_free(job);
let resolves=[];globalThis.fetch=(_url,options)=>new Promise(resolve=>resolves.push({resolve,options}));
for(let i=0;i<8;i++){job=start('http://localhost/test',32,1000);assert.ok(job);app._library_test_cancel(job);assert.equal(status(job),2);app._library_test_free(job);}
assert.equal(start('http://localhost/test',32,1000),0,'canceled undrained backend is still bounded');
for(const {resolve,options} of resolves){assert.equal(options.signal.aborted,true);resolve(new Response('late'));}await drain();assert.equal(app.librarySourceTasks.size,0);
globalThis.fetch=(_url,options)=>new Promise((_resolve,reject)=>options.signal.addEventListener('abort',()=>reject(Error('aborted'))));
job=start('http://localhost/test',32,25);await pause(60);await drain();assert.equal(status(job),3);app._library_test_free(job);assert.equal(app.librarySourceTasks.size,0);
let readResolve;globalThis.fetch=async()=>({status:200,body:{getReader:()=>({read:()=>new Promise(r=>readResolve=r),cancel:()=>Promise.reject(Error('cancel rejected'))})}});
job=start('http://localhost/test',32,1000);await drain();app._library_test_free(job);readResolve({done:false,value:new Uint8Array([1,2,3])});await drain();assert.equal(app.librarySourceTasks.size,0,'free before late bytes must drain without UAF');
globalThis.fetch=undefined;job=start('http://localhost/test',32,1000);assert.ok(job);assert.equal(status(job),3);app._library_test_free(job);
globalThis.fetch=async()=>new Response(good);const NativeAbort=globalThis.AbortController;
globalThis.AbortController=class {constructor(){throw Error('cannot allocate controller');}};
job=start('http://localhost/test',32,1000);assert.ok(job);assert.equal(status(job),3);app._library_test_free(job);
globalThis.AbortController=NativeAbort;assert.equal(app.librarySourceTasks.size,0);
console.log('WASM HTTP production transport: exact bytes, request policy, status/limit/deadline, cancellation, eight-job cap and late replies passed');
