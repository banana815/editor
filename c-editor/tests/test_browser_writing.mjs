// Real WASM writing workflows in a fresh browser profile and isolated /notes.
// Fixture seeding and assertions use FS/IndexedDB; all edits use production UI.
import assert from 'node:assert/strict';
import {createHash} from 'node:crypto';
import {spawn} from 'node:child_process';
import {mkdtemp,readFile,writeFile,rm} from 'node:fs/promises';
import {createServer} from 'node:http';
import {tmpdir} from 'node:os';
import {join,extname,resolve,sep} from 'node:path';

const build=resolve(process.argv[2]||'c-editor/build-web');
const executable=process.argv[3]||process.env.CHROME_BIN||
    (process.platform==='darwin'?'/Applications/Google Chrome.app/Contents/MacOS/Google Chrome':'chromium');
const cpuThrottle=Number(process.env.EDITOR_TEST_CPU_THROTTLE||1);
assert.ok(Number.isFinite(cpuThrottle)&&cpuThrottle>=1&&cpuThrottle<=20);
const profile=await mkdtemp(join(tmpdir(),'notelab-writing-browser-'));
const sleep=ms=>new Promise(resolve=>setTimeout(resolve,ms));
async function until(fn,label,timeout=45000) {
    const end=Date.now()+timeout;let last;
    while(Date.now()<end) {try {const value=await fn();if(value)return value;}catch(error){last=error;}await sleep(50);}
    throw new Error(`Timed out: ${label}${last?` (${last.message})`:''}`);
}
const artifactNames=['notelab.html','notelab.js','notelab.wasm','notelab.data'];
const hashes=async()=>Object.fromEntries(await Promise.all(artifactNames.map(async name=>
    [name,createHash('sha256').update(await readFile(join(build,name))).digest('hex')])));
const proof={started:new Date().toISOString(),cpuThrottle,artifacts:await hashes(),checks:[],screenshots:[]};
const server=createServer(async(req,res)=>{
    try {
        if(req.url==='/favicon.ico'){res.writeHead(204);res.end();return;}
        const path=resolve(build,'.'+new URL(req.url,'http://localhost').pathname);
        if(!path.startsWith(build+sep))throw Error('outside build directory');
        const data=await readFile(path);
        res.setHeader('Content-Type',({'.html':'text/html','.js':'text/javascript','.wasm':'application/wasm'})[extname(path)]||'application/octet-stream');
        res.end(data);
    }catch{res.writeHead(404);res.end('Not found');}
});
await new Promise(resolve=>server.listen(0,'127.0.0.1',resolve));
const origin=`http://127.0.0.1:${server.address().port}`;
const chrome=spawn(executable,['--headless=new','--no-first-run','--no-default-browser-check',
    '--disable-background-timer-throttling','--disable-backgrounding-occluded-windows','--disable-renderer-backgrounding',
    '--remote-debugging-port=0',`--user-data-dir=${profile}`,'--window-size=1100,760',
    '--use-angle=swiftshader','--enable-unsafe-swiftshader','about:blank'],{stdio:['ignore','ignore','pipe']});
let browserError,stderr='',ws,cdp,evaluate,browserClosed=false;
const browserFinished=new Promise(resolve=>chrome.once('close',()=>{browserClosed=true;resolve();}));
chrome.on('error',error=>{browserError=error;});
chrome.stderr.on('data',data=>{stderr=(stderr+data).slice(-6000);});
const pending=new Map(),exceptions=[],dialogs=[];
const alpha='/notes/000 Alpha.json',beta='/notes/001 Beta.json',restored='/notes/002 Restored.json';
const catalogPath='/notes/.notelab-writing/catalog.json';
const recoveryRoot='/notes/.notelab-writing';
try {
    const port=await until(async()=>{
        if(browserError)throw browserError;
        return Number((await readFile(join(profile,'DevToolsActivePort'),'utf8')).split('\n')[0]);
    },'browser debugging endpoint');
    const tabs=await(await fetch(`http://127.0.0.1:${port}/json/list`)).json();
    ws=new WebSocket(tabs.find(tab=>tab.type==='page'&&tab.url==='about:blank').webSocketDebuggerUrl);
    await new Promise((resolve,reject)=>{ws.onopen=resolve;ws.onerror=reject;});
    let id=0;
    ws.onmessage=e=>{
        const message=JSON.parse(e.data);
        if(message.id){const request=pending.get(message.id);pending.delete(message.id);
            if(message.error)request?.reject(message.error);else request?.resolve(message.result);
        }else if(message.method==='Runtime.exceptionThrown')exceptions.push(message.params.exceptionDetails);
        else if(message.method==='Page.javascriptDialogOpening')dialogs.push(message.params);
    };
    cdp=(method,params={})=>new Promise((resolve,reject)=>{
        const requestId=++id;
        const timer=setTimeout(()=>{pending.delete(requestId);reject(Error(`CDP timeout: ${method}`));},20000);
        pending.set(requestId,{resolve:value=>{clearTimeout(timer);resolve(value);},reject:error=>{clearTimeout(timer);reject(error);}});
        ws.send(JSON.stringify({id:requestId,method,params}));
    });
    evaluate=async expression=>{
        const result=await cdp('Runtime.evaluate',{expression,returnByValue:true,awaitPromise:true});
        if(result.exceptionDetails)throw Error(JSON.stringify(result.exceptionDetails));
        return result.result.value;
    };
    const rendered=()=>evaluate('new Promise(resolve=>requestAnimationFrame(()=>requestAnimationFrame(()=>resolve(true))))');
    const ready=async()=>{
        await until(()=>evaluate("typeof FS!=='undefined'&&document.getElementById('status').hidden&&!!Module.editorInputArea&&!!Module.editorInputUpdate"),'application ready');
        await rendered();
    };
    async function reload(){
        const previous=(await cdp('Page.getFrameTree')).frameTree.frame.loaderId;
        await cdp('Page.reload');
        await until(async()=>(await cdp('Page.getFrameTree')).frameTree.frame.loaderId!==previous,'new document after reload');
        await cdp('Page.bringToFront');await ready();
    }
    async function key(key,code,virtual,modifiers=0){
        const held=[[1,'Alt','AltLeft',18],[2,'Control','ControlLeft',17],[4,'Meta','MetaLeft',91],[8,'Shift','ShiftLeft',16]].filter(([mask])=>modifiers&mask);
        for(const[,key,code,v]of held)await cdp('Input.dispatchKeyEvent',{type:'rawKeyDown',key,code,windowsVirtualKeyCode:v,modifiers});
        await cdp('Input.dispatchKeyEvent',{type:'rawKeyDown',key,code,windowsVirtualKeyCode:virtual,modifiers});
        await cdp('Input.dispatchKeyEvent',{type:'keyUp',key,code,windowsVirtualKeyCode:virtual,modifiers});
        for(const[,key,code,v]of held.reverse())await cdp('Input.dispatchKeyEvent',{type:'keyUp',key,code,windowsVirtualKeyCode:v,modifiers:0});
        await rendered();
    }
    async function click(x,y){
        await cdp('Input.dispatchMouseEvent',{type:'mousePressed',x,y,button:'left',clickCount:1});
        await cdp('Input.dispatchMouseEvent',{type:'mouseReleased',x,y,button:'left',clickCount:1});await rendered();
    }
    const raw=path=>evaluate(`FS.readFile(${JSON.stringify(path)},{encoding:'utf8'})`);
    const exists=path=>evaluate(`FS.analyzePath(${JSON.stringify(path)}).exists`);
    const idle=()=>evaluate('!Module.editorUnsaved&&!Module.editorPersistencePending&&!Module.editorInputPending');
    const treeText=node=>[node.type==='text'?node.text:'',...(node.content||[]).map(treeText)].join('');
    const text=async path=>treeText(JSON.parse(await raw(path)));
    const catalog=async()=>JSON.parse(await raw(catalogPath));
    const durable=path=>evaluate(`new Promise((resolve,reject)=>{
        const request=indexedDB.open('/notes');request.onerror=()=>reject(request.error);
        request.onsuccess=()=>{const db=request.result,get=db.transaction('FILE_DATA').objectStore('FILE_DATA').get(${JSON.stringify(path)});
        get.onsuccess=()=>{resolve(get.result?new TextDecoder().decode(get.result.contents):null);db.close();};
        get.onerror=()=>{reject(get.error);db.close();};};})`);
    async function saved(path,expected){
        await until(async()=>await text(path)===expected&&await idle(),`saved ${expected}`);
        assert.equal(await durable(path),await raw(path),'committed note must reach IndexedDB');
    }
    const promptReady=()=>until(()=>evaluate(`document.activeElement.id==='editor-input'&&
        Module.editorInputArea.h*canvas.getBoundingClientRect().height/canvas.height<100&&
        Module.editorInputArea.y*canvas.getBoundingClientRect().height/canvas.height>200`),'text prompt ready');
    async function submit(value){
        await promptReady();await key('a','KeyA',65,2);await cdp('Input.insertText',{text:value});await rendered();await key('Enter','Enter',13);
    }
    async function openSearch(query,path){
        await key('p','KeyP',80,10);await submit(query);await click(250,130);
        await until(()=>evaluate(`document.title.includes(${JSON.stringify(path.split('/').at(-1))})&&document.activeElement.id==='editor-input'`),'search result article and focus');
    }
    const openManager=()=>key('m','KeyM',77,10);
    async function propertiesMatch(expected){
        await until(async()=>{const info=(await catalog()).articles[beta.split('/').at(-1)];return Object.entries(expected).every(([k,v])=>info?.[k]===v)&&await idle();},'persisted article properties');
        assert.equal(await durable(catalogPath),await raw(catalogPath));
    }
    const archives=async(trash,name)=>evaluate(`(()=>{
        const path=${JSON.stringify(recoveryRoot)}+${JSON.stringify(trash?'/trash':'/versions')};
        if(!FS.analyzePath(path).exists)return[];
        return FS.readdir(path).filter(id=>id.endsWith('.json')).map(id=>({id,...JSON.parse(FS.readFile(path+'/'+id,{encoding:'utf8'}))}))
            .filter(record=>!${JSON.stringify(name)}||record.name===${JSON.stringify(name)})
            .sort((a,b)=>b.time-a.time||(a.id<b.id?1:a.id>b.id?-1:0));})()`);
    async function screenshot(name){await rendered();await writeFile(join(build,name),Buffer.from((await cdp('Page.captureScreenshot')).data,'base64'));proof.screenshots.push(name);}
    const pass=name=>{proof.checks.push(name);console.log(`PASS: ${name}`);};

    await cdp('Page.enable');await cdp('Runtime.enable');
    await cdp('Emulation.setDeviceMetricsOverride',{width:1100,height:760,deviceScaleFactor:1,mobile:false});
    if(cpuThrottle>1)await cdp('Emulation.setCPUThrottlingRate',{rate:cpuThrottle});
    await cdp('Page.navigate',{url:origin+'/notelab.html'});await cdp('Page.bringToFront');await ready();
    const documentFor=text=>({type:'doc',attrs:{writingUnknown:4503599627370501},content:[{type:'paragraph',content:[{type:'text',text}]}]});
    const originalAlpha=JSON.stringify(documentFor('Draft alpha alpha')),originalBeta=JSON.stringify(documentFor('Beta needle 中文'));
    await evaluate(`new Promise((resolve,reject)=>{FS.writeFile(${JSON.stringify(alpha)},${JSON.stringify(originalAlpha)});FS.writeFile(${JSON.stringify(beta)},${JSON.stringify(originalBeta)});FS.syncfs(false,e=>e?reject(e):resolve());})`);
    await reload();await until(()=>evaluate('document.title.includes("000 Alpha.json")'),'first article restored');

    await key('h','KeyH',72,10);await submit('alpha');await submit('omega');await saved(alpha,'Draft omega omega');
    assert.equal(JSON.parse(await raw(alpha)).attrs.writingUnknown,4503599627370501);
    await key('z','KeyZ',90,2);await saved(alpha,'Draft alpha alpha');
    await key('y','KeyY',89,2);await saved(alpha,'Draft omega omega');
    const initialHistory=await until(async()=>{const records=await archives(false,'000 Alpha.json');return records.some(r=>treeText(JSON.parse(r.data))==='Draft alpha alpha')&&records;},'automatic pre-save history snapshot');
    pass('Replace all uses the browser shortcut, preserves metadata, and has one-step undo/redo with an automatic history snapshot');

    await openSearch('needle',beta);
    await cdp('Input.insertText',{text:'FOUND'});await saved(beta,'Beta FOUND 中文');
    await key('z','KeyZ',90,2);await saved(beta,'Beta needle 中文');
    pass('Cross-article search opens the matching article and selects the actual match for replacement');

    await openManager();await click(200,110);await submit('Blog');await propertiesMatch({folder:'Blog'});
    await click(650,110);await submit('writing,中文');await propertiesMatch({tags:'writing,中文'});
    await click(200,230);await submit('1200');await propertiesMatch({goal:1200});
    await click(200,170);await propertiesMatch({pinned:true});
    await click(650,170);await propertiesMatch({status:1});
    const expectedInfo={...(await catalog()).articles['001 Beta.json']};
    await screenshot('browser-writing-properties-validation.png');await key('Escape','Escape',27);
    await reload();assert.deepEqual((await catalog()).articles['001 Beta.json'],expectedInfo);
    await openSearch('needle',beta);await openManager();
    await click(650,170);await propertiesMatch({status:2});
    await click(650,170);await propertiesMatch({status:0});
    await click(650,170);await propertiesMatch({status:1});
    assert.deepEqual((await catalog()).articles['001 Beta.json'],expectedInfo);await key('Escape','Escape',27);
    pass('Folder, tags, writing goal, pinned state and editorial status persist through reload and initialize the actual manager UI');

    await openSearch('omega',alpha);const beforePreview=await raw(alpha);
    const records=await archives(false,'000 Alpha.json');
    const versionIndex=records.findIndex(r=>r.id===initialHistory.find(r=>treeText(JSON.parse(r.data))==='Draft alpha alpha').id);
    assert.ok(versionIndex>=0&&versionIndex<8,'controlled fixture version is visible without scrolling');
    await openManager();await click(200,290);await click(200,102+versionIndex*64);
    await screenshot('browser-writing-history-validation.png');
    assert.equal(await raw(alpha),beforePreview,'preview does not mutate current article');assert.ok(await idle());
    await click(170,35);await saved(alpha,'Draft alpha alpha');
    assert.ok((await archives(false,'000 Alpha.json')).some(r=>r.data===beforePreview),'restore retains pre-restore version');
    await key('z','KeyZ',90,2);await saved(alpha,'Draft omega omega');
    await key('y','KeyY',89,2);await saved(alpha,'Draft alpha alpha');
    pass('History previews without mutation, restores through the UI, retains the previous version, and supports undo/redo');

    await openSearch('needle',beta);const beforeTrash=await raw(beta);
    if(await evaluate('Module.editorInputArea.y*canvas.getBoundingClientRect().height/canvas.height<160'))await key('t','KeyT',84,10);
    await until(()=>evaluate('Module.editorInputArea.y*canvas.getBoundingClientRect().height/canvas.height>=160'),'expanded tools before trash');
    await click(440,106);await key('Enter','Enter',13);
    await until(async()=>!await exists(beta)&&await idle(),'article moved to trash and persisted');
    assert.equal(await durable(beta),null);
    const trash=await archives(true,'001 Beta.json');assert.equal(trash.length,1);assert.equal(trash[0].data,beforeTrash);
    assert.deepEqual(trash[0].article,expectedInfo);
    // No article is active after deletion. Use the visible controls rather
    // than a browser-owned shortcut when the native input target is disabled.
    await click(960,24);await click(676,65);await click(650,290);await click(200,102);
    await screenshot('browser-writing-trash-validation.png');await click(170,35);await submit('002 Restored.json');
    await until(async()=>await exists(restored)&&await idle(),'trash restoration persisted');
    assert.equal(await raw(restored),beforeTrash);assert.equal(await durable(restored),beforeTrash);
    assert.deepEqual((await catalog()).articles['002 Restored.json'],expectedInfo);
    assert.equal((await archives(true,'001 Beta.json'))[0].data,beforeTrash,'recovery copy remains available');
    await reload();assert.equal(await raw(restored),beforeTrash);assert.deepEqual((await catalog()).articles['002 Restored.json'],expectedInfo);
    pass('Trash preserves exact article bytes and properties; restore as a new file survives IndexedDB reload without discarding its recovery copy');

    assert.deepEqual(exceptions,[]);assert.deepEqual(dialogs,[]);
    assert.deepEqual(await hashes(),proof.artifacts);proof.completed=new Date().toISOString();
    await writeFile(join(build,'browser-writing-proof.json'),JSON.stringify(proof,null,2));
    await Promise.all([rm(join(build,'browser-writing-failure.json'),{force:true}),rm(join(build,'browser-writing-failure.png'),{force:true})]);
    console.log('All real-browser writing workflow checks passed.');
}catch(error){
    if(evaluate)try{
        const state=await evaluate(`({title:document.title,focus:document.activeElement?.id,area:Module.editorInputArea,
            unsaved:Module.editorUnsaved,pending:Module.editorPersistencePending,inputPending:Module.editorInputPending,
            notes:typeof FS!=='undefined'?FS.readdir('/notes'):[],catalog:typeof FS!=='undefined'&&FS.analyzePath(${JSON.stringify(catalogPath)}).exists?FS.readFile(${JSON.stringify(catalogPath)},{encoding:'utf8'}):null})`);
        await writeFile(join(build,'browser-writing-failure.json'),JSON.stringify({error:String(error),proof,exceptions,dialogs,state},null,2));
        await writeFile(join(build,'browser-writing-failure.png'),Buffer.from((await cdp('Page.captureScreenshot')).data,'base64'));
    }catch{}
    console.error(stderr);throw error;
}finally{
    if(cdp&&ws?.readyState===WebSocket.OPEN&&!browserClosed)try{await Promise.race([cdp('Browser.close'),sleep(1000)]);}catch{}
    try{ws?.close();}catch{}
    for(const request of pending.values())request.reject(Error('test finished'));
    server.close();server.closeAllConnections();await Promise.race([browserFinished,sleep(3000)]);
    if(!browserClosed){chrome.kill();await Promise.race([browserFinished,sleep(3000)]);}
    if(!browserClosed){chrome.kill('SIGKILL');await Promise.race([browserFinished,sleep(3000)]);}
    await rm(profile,{recursive:true,force:true,maxRetries:8,retryDelay:200});
}
