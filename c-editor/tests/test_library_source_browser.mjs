// Actual Chromium fetch/CORS/decompression with the production C+EM_JS transport.
// Temporary localhost fixtures only; no Zotero server or user browser profile.
import assert from 'node:assert/strict';
import {spawn} from 'node:child_process';
import {mkdtemp,readFile,rm} from 'node:fs/promises';
import {createServer} from 'node:http';
import {gzipSync} from 'node:zlib';
import {tmpdir} from 'node:os';
import {join,resolve,dirname,basename} from 'node:path';
const script=resolve(process.argv[2]);
const chromePath=process.argv[3]||process.env.CHROME_BIN||(process.platform==='darwin'?'/Applications/Google Chrome.app/Contents/MacOS/Google Chrome':'chromium');
const profile=await mkdtemp(join(tmpdir(),'notelab-library-browser-'));
const BODY='[{"id":"test","title":"中文🙂"}]';
let forbidden=0,credentials=0;
const fixture=(req,res)=>{
 if(req.headers.cookie||req.headers.authorization||req.headers['proxy-authorization'])credentials++;
 const url=new URL(req.url,'http://localhost');
 if(url.pathname==='/target')forbidden++;
 if(url.pathname==='/cors')res.setHeader('Access-Control-Allow-Origin','*');
 if(url.pathname==='/redirect'){res.writeHead(302,{Location:'/target'});res.end();return;}
 if(url.pathname==='/missing'){res.writeHead(404);res.end('missing');return;}
 if(url.pathname==='/empty'){res.writeHead(204);res.end();return;}
 if(url.pathname==='/cookie')res.setHeader('Set-Cookie','must-not-send=secret');
 res.setHeader('Content-Type','text/plain; charset=utf-8');
 if(url.pathname==='/gzip'){res.setHeader('Content-Encoding','gzip');res.end(gzipSync(BODY));return;}
 if(url.pathname==='/slow'){setTimeout(()=>res.end(BODY),500);return;}
 if(url.pathname==='/truncated'){res.setHeader('Content-Length','10000');res.write(BODY);setTimeout(()=>res.destroy(),20);return;}
 res.end(BODY);
};
const cross=createServer(fixture);await new Promise(r=>cross.listen(0,'127.0.0.1',r));
const server=createServer(async(req,res)=>{
 try {
  if(req.url==='/'){res.setHeader('Content-Type','text/html');res.end('<!doctype html><script src="/transport.js"></script><script>Module().then(m=>{globalThis.app=m;m._library_test_init();globalThis.start=m.cwrap("library_test_start","number",["string","number","number"]);globalThis.ready=true;});</script>');return;}
  if(req.url==='/transport.js'){res.setHeader('Content-Type','text/javascript');res.end(await readFile(script));return;}
  if(req.url==='/'+basename(script).replace(/\.(c?js|mjs)$/,'.wasm')){res.setHeader('Content-Type','application/wasm');res.end(await readFile(join(dirname(script),basename(script).replace(/\.(c?js|mjs)$/,'.wasm'))));return;}
  fixture(req,res);
 }catch{res.writeHead(500);res.end();}
});await new Promise(r=>server.listen(0,'127.0.0.1',r));
const origin=`http://127.0.0.1:${server.address().port}`;
const other=`http://127.0.0.1:${cross.address().port}`;
const chrome=spawn(chromePath,['--headless=new','--no-first-run','--no-default-browser-check','--remote-debugging-port=0',`--user-data-dir=${profile}`,'about:blank'],{stdio:['ignore','ignore','pipe']});
let stderr='',closed=false,ws,launchError;chrome.stderr.on('data',b=>stderr=(stderr+b).slice(-4000));
chrome.on('error',error=>{launchError=error;});
const ended=new Promise(r=>chrome.once('close',()=>{closed=true;r();}));
const pause=ms=>new Promise(r=>setTimeout(r,ms));
async function until(fn,label){for(let i=0;i<600;i++){if(launchError)throw launchError;if(closed)throw Error('Chrome exited before '+label+' '+stderr);const value=await fn();if(value)return value;await pause(50);}throw Error('Timeout '+label+' '+stderr);}
try{
 const port=await until(async()=>{try{return Number((await readFile(join(profile,'DevToolsActivePort'),'utf8')).split('\n')[0]);}catch{return 0;}},'Chrome');
 const tabs=await(await fetch(`http://127.0.0.1:${port}/json/list`)).json();ws=new WebSocket(tabs.find(t=>t.type==='page').webSocketDebuggerUrl);
 await new Promise((r,j)=>{ws.onopen=r;ws.onerror=j;});let id=0;const pending=new Map();
 ws.onmessage=e=>{const m=JSON.parse(e.data);if(m.id){const p=pending.get(m.id);pending.delete(m.id);if(m.error)p.reject(Error(JSON.stringify(m.error)));else p.resolve(m.result);}};
 const cdp=(method,params={})=>new Promise((resolve,reject)=>{const n=++id;pending.set(n,{resolve,reject});ws.send(JSON.stringify({id:n,method,params}));});
 const evaluate=async expression=>{const r=await cdp('Runtime.evaluate',{expression,returnByValue:true,awaitPromise:true});if(r.exceptionDetails)throw Error(JSON.stringify(r.exceptionDetails));return r.result.value;};
 await cdp('Page.enable');await cdp('Runtime.enable');await cdp('Page.navigate',{url:origin});await until(()=>evaluate('globalThis.ready===true'),'WASM');
 const request=async(url,cap=4096,timeout=2000)=>evaluate(`(async()=>{const j=start(${JSON.stringify(url)},${cap},${timeout});if(!j)throw Error('start');let state;for(let i=0;i<500;i++){state=app._library_test_status(j);if(state!==0)break;await new Promise(r=>setTimeout(r,5));}let body=null;if(app._library_test_take(j)){body=new TextDecoder().decode(app.HEAPU8.slice(app._library_test_bytes(),app._library_test_bytes()+app._library_test_size()));app._library_test_clear();}const message=app.UTF8ToString(app._library_test_message(j));app._library_test_free(j);return {state,body,message};})()`);
 for(const path of ['/ok','/gzip','/cookie','/ok']){const result=await request(origin+path);assert.equal(result.state,1,result.message);assert.equal(result.body,BODY);}
 assert.equal((await request(origin+'/empty')).body,'');
 assert.equal((await request(other+'/cors')).body,BODY);
 for(const url of [other+'/blocked',origin+'/missing',origin+'/redirect',origin+'/truncated'])assert.equal((await request(url)).state,3,url);
 assert.equal((await request(origin+'/gzip',16)).state,3,'decoded cap');
 assert.equal((await request(origin+'/slow',4096,50)).state,3,'timeout');
 await evaluate(`(async()=>{const j=start(${JSON.stringify(origin+'/slow')},4096,2000);await new Promise(r=>setTimeout(r,10));app._library_test_cancel(j);if(app._library_test_status(j)!==2)throw Error('cancel');app._library_test_free(j);await new Promise(r=>setTimeout(r,600));if(app.librarySourceTasks.size)throw Error('late request did not drain');})()`);
 assert.equal(forbidden,0,'redirect target contacted');assert.equal(credentials,0,'cookies/auth were sent');
 console.log('Actual Chrome HTTP/WASM: CORS permitted/blocked, UTF-8, gzip decoded cap, no cookies/redirect, truncated response, timeout and canceled late completion passed');
}finally{
 if(ws)ws.close();if(!closed)chrome.kill('SIGTERM');await Promise.race([ended,pause(3000)]);if(!closed){chrome.kill('SIGKILL');await ended;}
 server.closeAllConnections();cross.closeAllConnections();await Promise.all([new Promise(r=>server.close(r)),new Promise(r=>cross.close(r))]);await rm(profile,{recursive:true,force:true});
}
