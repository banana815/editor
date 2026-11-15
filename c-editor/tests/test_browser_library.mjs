// Actual Chromium library UI, local HTTP refresh, selected bibliography and archive.
// Clipboard transport is intercepted in this page; no host clipboard is read.
import assert from 'node:assert/strict';
import {createHash} from 'node:crypto';
import {spawn} from 'node:child_process';
import {mkdtemp,readFile,writeFile,rm} from 'node:fs/promises';
import {createServer} from 'node:http';
import {tmpdir} from 'node:os';
import {join,extname,resolve,sep} from 'node:path';
const build=resolve(process.argv[2]||'c-editor/build-web');
const executable = process.argv[3] || process.env.CHROME_BIN ||
    (process.platform === 'darwin' ? '/Applications/Google Chrome.app/Contents/MacOS/Google Chrome' : 'chromium');
const profile = await mkdtemp(join(tmpdir(), 'notelab-browser-'));
const sleep = ms => new Promise(r => setTimeout(r, ms));
async function until(fn, label, timeout = 30000) {
    const end = Date.now() + timeout;
    let last;
    while (Date.now() < end) {
        try { const value = await fn(); if (value) return value; } catch (error) { last = error; }
        await sleep(50);
    }
    throw new Error(`Timed out: ${label}${last ? ` (${last.message})` : ''}`);
}
const items = [
    {id:'source-a',type:'book',title:'Alpha selected book',author:[{family:'Doe',given:'Jane'}],issued:{'date-parts':[[2020]]},publisher:'Local Press'},
    {id:'source-b',type:'book',title:'Beta selected book',author:[{family:'Roe',given:'Richard'}],issued:{'date-parts':[[2021]]},publisher:'Other Press'},
    {id:'source-c',type:'book',title:'Gamma refreshed book',author:[{family:'Zee',given:'Zoe'}],issued:{'date-parts':[[2022]]},publisher:'Fresh Press'}
];
let responseItems=items.slice(0,2), responseStatus=200, fixtureRequests=0;
const artifactNames=['notelab.html','notelab.js','notelab.wasm','notelab.data'];
async function artifactHashes(){return Object.fromEntries(await Promise.all(artifactNames.map(async name=>[name,createHash('sha256').update(await readFile(join(build,name))).digest('hex')])));}
const proof={started:new Date().toISOString(),artifacts:await artifactHashes(),checks:[],screenshots:[]};
const server = createServer(async (req, res) => {
    try {
        if (req.url === '/fixture/library.json') {
            fixtureRequests++;
            res.writeHead(responseStatus, {'Content-Type':'application/json','Cache-Control':'no-store'});
            res.end(responseStatus===200?JSON.stringify(responseItems):JSON.stringify({error:'deliberate fixture failure'}));
            return;
        }
        if (req.url === '/favicon.ico') { res.writeHead(204); res.end(); return; }
        const path = resolve(build, '.' + new URL(req.url, 'http://localhost').pathname);
        if (!path.startsWith(build + sep)) throw new Error('outside build directory');
        const data = await readFile(path);
        res.setHeader('Content-Type', ({'.html':'text/html', '.js':'text/javascript', '.wasm':'application/wasm'})[extname(path)] || 'application/octet-stream');
        res.end(data);
    } catch { res.writeHead(404); res.end('Not found'); }
});
await new Promise(r => server.listen(0, '127.0.0.1', r));
const origin = `http://127.0.0.1:${server.address().port}`;
const chrome = spawn(executable, [
    '--headless=new', '--no-first-run', '--no-default-browser-check',
    '--disable-background-timer-throttling', '--disable-backgrounding-occluded-windows',
    '--disable-renderer-backgrounding', '--remote-debugging-port=0',
    `--user-data-dir=${profile}`, '--window-size=1100,760',
    '--use-angle=swiftshader', '--enable-unsafe-swiftshader', 'about:blank'
], {stdio:['ignore','ignore','pipe']});
let browserError, stderr = '', ws, evaluate, cdp;
let browserClosed = false;
const browserFinished = new Promise(resolve => chrome.once('close', () => {
    browserClosed = true;
    resolve();
}));
chrome.on('error', error => { browserError = error; });
chrome.stderr.on('data', data => { stderr = (stderr + data).slice(-5000); });
const pending = new Map();
try {
    const port = await until(async () => {
        if (browserError) throw browserError;
        return Number((await readFile(join(profile, 'DevToolsActivePort'), 'utf8')).split('\n')[0]);
    }, 'browser debugging endpoint');
    const tabs = await (await fetch(`http://127.0.0.1:${port}/json/list`)).json();
    ws = new WebSocket(tabs.find(tab => tab.type === 'page' && tab.url === 'about:blank').webSocketDebuggerUrl);
    await new Promise((resolve, reject) => { ws.onopen = resolve; ws.onerror = reject; });
    let id = 0;
    const exceptions = [], dialogs = [], network = [];
    proof.network=network;
    ws.onmessage = e => {
        const message = JSON.parse(e.data);
        if (message.id) {
            const request = pending.get(message.id); pending.delete(message.id);
            if (message.error) request?.reject(message.error); else request?.resolve(message.result);
        } else if (message.method === 'Runtime.exceptionThrown') exceptions.push(message.params.exceptionDetails);
        else if (message.method === 'Page.javascriptDialogOpening') dialogs.push(message.params);
        else if (message.method === 'Network.loadingFailed') network.push(message.params);
        else if (message.method === 'Network.requestWillBeSent' && message.params.request.url.includes('/fixture/')) network.push({url:message.params.request.url});
        else if (message.method === 'Network.responseReceived' && message.params.response.url.includes('/fixture/')) network.push({url:message.params.response.url,status:message.params.response.status});
    };
    cdp = (method, params = {}) => new Promise((resolve, reject) => {
        const requestId = ++id;
        const timer = setTimeout(() => { pending.delete(requestId); reject(new Error(`CDP timeout: ${method}`)); }, 15000);
        pending.set(requestId, {
            resolve: result => { clearTimeout(timer); resolve(result); },
            reject: error => { clearTimeout(timer); reject(error); }
        });
        ws.send(JSON.stringify({id: requestId, method, params}));
    });
    evaluate = async expression => {
        const result = await cdp('Runtime.evaluate', {expression, returnByValue:true, awaitPromise:true});
        if (result.exceptionDetails) throw new Error(JSON.stringify(result.exceptionDetails));
        return result.result.value;
    };
    const ready=()=>until(()=>evaluate("typeof FS!=='undefined' && document.getElementById('status').hidden && !!Module.editorClipboardStart"),'app ready');
    const files=['/notes/000 Selected.json','/notes/001 Target.json','/notes/002 Legacy.json','/notes/003 List.json'];
    const blank={type:'doc',content:[{type:'paragraph',content:[]}]};
    const list={type:'doc',content:[{type:'bulletList',content:[{type:'listItem',content:[{type:'paragraph',content:[{type:'text',text:'A'}]}]},{type:'listItem',content:[{type:'paragraph',content:[{type:'text',text:'B'}]},{type:'heading',attrs:{level:2},content:[{type:'text',text:'C'}]}]}]}]};
    const archived={type:'doc',attrs:{archiveUnknown:4503599627370501},content:[{type:'paragraph',content:[]},
        {type:'zoteroEndnoteSection',attrs:{entriesJson:JSON.stringify([{citationId:'archive-a',number:7,
            entryHtml:'<div class="csl-entry">Old &amp; preserved <i>Archived 中文🙂 book</i>. <a href="https://example.org/archive">Link</a></div>',
            entryText:'Old & preserved Archived 中文🙂 book. Link',unknownArchive:{exact:4503599627370501}}]),
            cslRenderJson:'{"version":1,"kind":"bibliography","forged":"DO NOT DISPLAY THIS CACHE"}',unknownSection:true}}]};
    const cacheFile='/notes/.notelab-library/library.json';
    const read=path=>evaluate(`FS.analyzePath(${JSON.stringify(path)}).exists ? JSON.parse(FS.readFile(${JSON.stringify(path)},{encoding:'utf8'})) : null`);
    const raw=path=>evaluate(`FS.readFile(${JSON.stringify(path)},{encoding:'utf8'})`);
    const walk=node=>[node,...(node.content||[]).flatMap(walk)];
    const sections=doc=>walk(doc).filter(n=>n.type==='zoteroEndnoteSection');
    const cites=doc=>walk(doc).filter(n=>n.type==='zoteroCitationBlock');
    const cache=doc=>JSON.parse(sections(doc)[0].attrs.cslRenderJson);
    const idle=()=>evaluate('!Module.editorUnsaved && !Module.editorPersistencePending');
    async function key(key,code,virtual,modifiers=0){
        // CDP's Windows virtual codes are portable. They are not macOS native
        // key codes: passing VK_SHIFT as native code 16 can blur the window.
        const held=[[1,'Alt','AltLeft',18],[2,'Control','ControlLeft',17],[4,'Meta','MetaLeft',91],[8,'Shift','ShiftLeft',16]].filter(([mask])=>modifiers&mask);
        for(const [,key,code,v] of held)await cdp('Input.dispatchKeyEvent',{type:'rawKeyDown',key,code,windowsVirtualKeyCode:v,modifiers});
        await cdp('Input.dispatchKeyEvent',{type:'rawKeyDown',key,code,windowsVirtualKeyCode:virtual,modifiers});
        await cdp('Input.dispatchKeyEvent',{type:'keyUp',key,code,windowsVirtualKeyCode:virtual,modifiers});
        for(const [,key,code,v] of held.reverse())await cdp('Input.dispatchKeyEvent',{type:'keyUp',key,code,windowsVirtualKeyCode:v,modifiers:0});
        await sleep(100);
    }
    async function click(x,y){await cdp('Input.dispatchMouseEvent',{type:'mousePressed',x,y,button:'left',clickCount:1});await cdp('Input.dispatchMouseEvent',{type:'mouseReleased',x,y,button:'left',clickCount:1});await sleep(120);}
    async function screenshot(name){await writeFile(join(build,name),Buffer.from((await cdp('Page.captureScreenshot')).data,'base64'));proof.screenshots.push(name);}
    function pass(name){proof.checks.push(name);console.log(`PASS: ${name}`);}
    async function selected(path,count){return until(async()=>{const d=await read(path),s=sections(d)[0];return s?.attrs.bibliographyMode==='selected'&&JSON.parse(s.attrs.selectedItemsJson).length===count&&cache(d).entries.length===count&&await idle()&&d;},`selected bibliography (${count})`);}
    const openLibrary=()=>click(1047,24);
    const scrollActions=()=>click(978,28);
    const closeLibrary=()=>click(1060,28);
    const openNote=index=>click(100,200+40*index);
    await cdp('Page.enable');await cdp('Runtime.enable');await cdp('Network.enable');
    await cdp('Emulation.setDeviceMetricsOverride',{width:1100,height:760,deviceScaleFactor:1,mobile:false});
    await cdp('Page.navigate',{url:origin+'/notelab.html'});await cdp('Page.bringToFront');await ready();
    await evaluate(`new Promise((resolve,reject)=>{${files.map((f,i)=>`FS.writeFile(${JSON.stringify(f)},${JSON.stringify(JSON.stringify(i===2?archived:i===3?list:blank))});`).join('')}FS.syncfs(false,e=>e?reject(e):resolve());})`);
    await cdp('Page.reload');await ready();await cdp('Page.bringToFront');await openNote(0);
    await openLibrary();await scrollActions();await click(706,653);
    await key('a','KeyA',65,2);await cdp('Input.insertText',{text:origin+'/fixture/library.json'});await key('Enter','Enter',13);
    await until(async()=>(await read(cacheFile)).items.length===2&&await idle(),'HTTP library loaded and saved');
    assert.equal(JSON.parse((await read(cacheFile)).clientStateJson).location,origin+'/fixture/library.json');
    pass('App HTTP source connects and persists two library items');
    await click(706,603);await click(1014,603);
    const first=await selected(files[0],2);assert.equal(cites(first).length,0);assert.equal(first.attrs.citationProfile,'apa');
    const formatted=cache(first),runs=formatted.entries.flatMap(e=>e.rows.flatMap(r=>[...(r.left||[]),...r.body]));
    assert.equal(formatted.processorProfile,'notelab-csl/1');assert.equal(formatted.style.profile,'apa');
    assert.match(formatted.style.sha256,/^[0-9a-f]{64}$/);assert.match(formatted.sourceDigest,/^[0-9a-f]{64}$/);
    assert.ok(runs.some(r=>r.text.includes('Alpha selected book')&&(r.format&2)),'APA book title has controlled italic formatting');
    await screenshot('browser-library-selected-apa.png');
    pass('Select all creates two standard APA entries without body citations');
    await key('z','KeyZ',90,2);await until(async()=>sections(await read(files[0])).length===0&&await idle(),'one undo removes selected bibliography');
    await openLibrary();await scrollActions();await click(1014,603);await selected(files[0],2);
    await openLibrary();await scrollActions();await click(735,547);
    await until(async()=>(await read(files[0])).attrs.citationProfile==='ieee'&&await idle(),'IEEE style');
    await closeLibrary();await key('z','KeyZ',90,2);
    await until(async()=>(await read(files[0])).attrs.citationProfile==='apa'&&await idle(),'style one undo');
    pass('Selected bibliography and style each undo in one step');
    await key('a','KeyA',65,2);
    const copied=await evaluate(`(()=>{window.libraryClipboard=new DataTransfer();canvas.focus();canvas.dispatchEvent(new ClipboardEvent('copy',{clipboardData:libraryClipboard,bubbles:true,cancelable:true}));return {types:[...libraryClipboard.types],plain:libraryClipboard.getData('text/plain'),html:libraryClipboard.getData('text/html'),rich:libraryClipboard.getData('application/x-notelab-document-slice+json')};})()`);
    assert.ok(copied.types.includes('application/x-notelab-document-slice+json'));assert.match(copied.plain,/Alpha selected book/);assert.match(copied.plain,/Beta selected book/);assert.match(copied.rich,/selectedItemsJson/);
    await openNote(1);await evaluate("canvas.dispatchEvent(new ClipboardEvent('paste',{clipboardData:libraryClipboard,bubbles:true,cancelable:true}))");
    await selected(files[1],2);assert.equal(cites(await read(files[1])).length,0);
    await key('z','KeyZ',90,2);await until(async()=>sections(await read(files[1])).length===0&&await idle(),'selected paste one undo');
    await evaluate(`(()=>{const d=new DataTransfer();d.setData('text/plain',libraryClipboard.getData('text/plain'));d.setData('text/html',libraryClipboard.getData('text/html'));canvas.dispatchEvent(new ClipboardEvent('paste',{clipboardData:d,bubbles:true,cancelable:true}));})()`);
    await selected(files[1],2);pass('Selected bibliography typed and HTML clipboard preserve sources with one-step undo');
    responseItems=items;
    await openLibrary();await scrollActions();await click(860,653);
    await until(async()=>(await read(cacheFile)).items.length===3&&await idle(),'refresh adds third item');
    await click(860,603);await click(646,237);await click(1014,603);
    await selected(files[1],1);
    await evaluate("canvas.dispatchEvent(new ClipboardEvent('paste',{clipboardData:libraryClipboard,bubbles:true,cancelable:true}))");
    await selected(files[1],3);assert.equal(cites(await read(files[1])).length,0);
    await key('z','KeyZ',90,2);await selected(files[1],1);
    await evaluate("canvas.dispatchEvent(new ClipboardEvent('paste',{clipboardData:libraryClipboard,bubbles:true,cancelable:true}))");
    await selected(files[1],3);
    pass('Pasting selected bibliography unions independent sources and undo restores the previous selection');
    await openLibrary();await scrollActions();
    const goodCache=await raw(cacheFile),goodNote=await read(files[1]),requestsBefore=fixtureRequests;
    responseStatus=500;await click(860,653);await until(()=>fixtureRequests>requestsBefore,'failed refresh request');await until(()=>evaluate('Module.librarySourceTasks.size===0'),'failed refresh finished');await sleep(400);
    assert.equal(await raw(cacheFile),goodCache);assert.deepEqual(await read(files[1]),goodNote);
    await screenshot('browser-library-refresh-error.png');await closeLibrary();
    pass('Refresh success updates library; HTTP failure preserves saved library and note');
    await openNote(2);await sleep(400);assert.deepEqual(await read(files[2]),archived);
    await screenshot('browser-library-archive.png');
    await key('a','KeyA',65,2);
    const archiveCopy=await evaluate(`(()=>{const d=new DataTransfer();canvas.focus();canvas.dispatchEvent(new ClipboardEvent('copy',{clipboardData:d,bubbles:true,cancelable:true}));return d.getData('text/plain');})()`);
    assert.match(archiveCopy,/Archived 中文🙂 book/);assert.match(archiveCopy,/Old & preserved/);assert.doesNotMatch(archiveCopy,/DO NOT DISPLAY THIS CACHE/);
    assert.deepEqual(await read(files[2]),archived);
    await cdp('Page.reload');await ready();await openNote(2);await sleep(300);assert.deepEqual(await read(files[2]),archived);
    assert.equal((await read(cacheFile)).items.length,3);
    await openNote(0);await selected(files[0],2);assert.equal((await read(files[0])).attrs.citationProfile,'apa');
    pass('Legacy archive renders and copies safely without changing original metadata; reload restores cache and selected profile');
    await evaluate(`(()=>{window.libraryKeys=[];window.addEventListener('keydown',e=>libraryKeys.push({key:e.key,code:e.code,shift:e.shiftKey,ctrl:e.ctrlKey,location:e.location}),true)})()`);
    await openNote(3);await click(310,188);await key('End','End',35,2);await key('Tab','Tab',9);
    await until(async()=>walk(await read(files[3])).filter(n=>n.type==='bulletList').length===2&&await idle(),'Tab indents a heading in its list item');
    assert.equal(walk(await read(files[3])).filter(n=>n.type==='text').map(n=>n.text).join(''),'ABC');
    await key('Tab','Tab',9,8);await until(async()=>JSON.stringify(await read(files[3]))===JSON.stringify(list)&&await idle(),'Shift Tab restores list structure');
    pass('Tab and Shift Tab operate on a heading inside a list item without changing text');
    assert.deepEqual(exceptions,[]);assert.deepEqual(dialogs,[]);
    assert.deepEqual(await artifactHashes(),proof.artifacts,'build artifacts must remain unchanged during test');
    proof.completed=new Date().toISOString();proof.fixtureRequests=fixtureRequests;
    await writeFile(join(build,'browser-library-proof.json'),JSON.stringify(proof,null,2));
    await Promise.all([rm(join(build,'browser-library-failure.json'),{force:true}),rm(join(build,'browser-library-failure.png'),{force:true})]);
    console.log('PASS: actual Chrome library integration; proof and screenshots saved beside build artifacts');
} catch(error) {
    if(evaluate)try{await writeFile(join(build,'browser-library-failure.json'),JSON.stringify({proof,fixtureRequests,state:await evaluate('({keys:window.libraryKeys,list:typeof FS!=="undefined"&&FS.analyzePath("/notes/003 List.json").exists?JSON.parse(FS.readFile("/notes/003 List.json",{encoding:"utf8"})):null,title:document.title,focus:document.activeElement.id,unsaved:Module.editorUnsaved,pending:Module.editorPersistencePending,area:Module.editorInputArea,viewport:{innerWidth,innerHeight,dpr:devicePixelRatio},files:typeof FS!=="undefined"?FS.readdir("/notes"):[]})')},null,2));await writeFile(join(build,'browser-library-failure.png'),Buffer.from((await cdp('Page.captureScreenshot')).data,'base64'));}catch{}
    console.error(stderr);throw error;
} finally {
    if (cdp && ws?.readyState === WebSocket.OPEN && !browserClosed) {
        try { await Promise.race([cdp('Browser.close'), sleep(1000)]); } catch {}
    }
    try { ws?.close(); } catch {}
    for (const request of pending.values()) request.reject(new Error('test finished'));
    server.close(); server.closeAllConnections();
    await Promise.race([browserFinished, sleep(3000)]);
    if (!browserClosed) {chrome.kill();await Promise.race([browserFinished,sleep(3000)]);}
    if (!browserClosed) {chrome.kill('SIGKILL');await Promise.race([browserFinished,sleep(3000)]);}
    await rm(profile, {recursive:true,force:true,maxRetries:8,retryDelay:200});
}
