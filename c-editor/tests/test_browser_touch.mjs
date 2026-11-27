// Actual Chromium touch events and rich clipboard using a disposable profile.
// Clipboard transport is intercepted in this page; no host clipboard is read.
import assert from 'node:assert/strict';
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
const server = createServer(async (req, res) => {
    try {
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
    const exceptions = [], dialogs = [];
    ws.onmessage = e => {
        const message = JSON.parse(e.data);
        if (message.id) {
            const request = pending.get(message.id); pending.delete(message.id);
            if (message.error) request?.reject(message.error); else request?.resolve(message.result);
        } else if (message.method === 'Runtime.exceptionThrown') exceptions.push(message.params.exceptionDetails);
        else if (message.method === 'Page.javascriptDialogOpening') dialogs.push(message.params);
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
    const file='/notes/000 Touch.json';
    const item={id:'touch-source',type:'book',title:'Touch reference',author:[{family:'Doe',given:'Jane'}],issued:{'date-parts':[[2020]]}};
    const original={type:'doc',attrs:{citationProfile:'apa',unknownTouchMetadata:4503599627370501},content:[
        ...Array.from({length:70},(_,i)=>({type:'paragraph',content:[{type:'text',text:`Line ${String(i).padStart(2,'0')} 中文🙂 touch selection.`}]})),
        {type:'zoteroCitationBlock',attrs:{citationId:'touch-cite',sourceId:item.id,cslJson:JSON.stringify(item),page:'7',note:''}}]};
    const read=()=>evaluate(`JSON.parse(FS.readFile(${JSON.stringify(file)},{encoding:'utf8'}))`);
    const walk=node=>[node,...(node.content||[]).flatMap(walk)];
    const cites=doc=>walk(doc).filter(n=>n.type==='zoteroCitationBlock');
    const idle=()=>evaluate('!Module.editorUnsaved && !Module.editorPersistencePending');
    async function key(key,code,virtual,modifiers=0){
        if(modifiers&2)await cdp('Input.dispatchKeyEvent',{type:'rawKeyDown',key:'Control',code:'ControlLeft',windowsVirtualKeyCode:17,nativeVirtualKeyCode:17,modifiers});
        await cdp('Input.dispatchKeyEvent',{type:'rawKeyDown',key,code,windowsVirtualKeyCode:virtual,nativeVirtualKeyCode:virtual,modifiers});
        await cdp('Input.dispatchKeyEvent',{type:'keyUp',key,code,windowsVirtualKeyCode:virtual,nativeVirtualKeyCode:virtual,modifiers});
        if(modifiers&2)await cdp('Input.dispatchKeyEvent',{type:'keyUp',key:'Control',code:'ControlLeft',windowsVirtualKeyCode:17,nativeVirtualKeyCode:17,modifiers:0});
        await sleep(80);
    }
    async function touch(type,x,y){await cdp('Input.dispatchTouchEvent',{type,touchPoints:type==='touchEnd'||type==='touchCancel'?[]:[{x,y,id:1,radiusX:2,radiusY:2,force:1}]});}
    async function tap(x,y){await touch('touchStart',x,y);await sleep(45);await touch('touchEnd',x,y);await sleep(100);}
    const screenshot=async name=>writeFile(join(build,name),Buffer.from((await cdp('Page.captureScreenshot')).data,'base64'));
    await cdp('Page.enable');await cdp('Runtime.enable');
    await cdp('Emulation.setDeviceMetricsOverride',{width:390,height:844,deviceScaleFactor:2,mobile:true});
    await cdp('Emulation.setTouchEmulationEnabled',{enabled:true,maxTouchPoints:5});
    await cdp('Page.navigate',{url:origin+'/notelab.html'});await cdp('Page.bringToFront');await ready();
    await evaluate(`new Promise((resolve,reject)=>{FS.writeFile(${JSON.stringify(file)},${JSON.stringify(JSON.stringify(original))});FS.writeFile('/notes/zzz-invalid.json','{bad');FS.syncfs(false,e=>e?reject(e):resolve());})`);
    await cdp('Page.reload');await ready();await cdp('Page.bringToFront');
    await until(async()=>cites(await read())[0]?.attrs.cslRenderJson && await idle(),'initial formal cache');
    const baseline=await read();
    await evaluate(`(()=>{const update=Module.editorInputUpdate;Module.editorInputUpdate=(...args)=>{window.lastInputTarget=args[0];return update(...args);};window.touchStack=false;window.touchStacks=[];window.addEventListener('pointerup',()=>{touchStack=true;},true);window.addEventListener('pointerup',()=>{touchStack=false;});})()`);
    await tap(40,65);await tap(60,202);await tap(40,65);
    await until(()=>evaluate('lastInputTarget===0'),'failed note open leaves original C editor unfocused');
    assert.deepEqual(await read(),baseline,'failed note open preserves the original document');

    await evaluate(`(()=>{window.touchFocuses=0;const input=document.getElementById('editor-input'),focus=input.focus.bind(input);input.focus=(...args)=>{touchFocuses++;touchStacks.push(touchStack);return focus(...args);};input.blur();canvas.focus();window.touchWrites=[];Object.defineProperty(navigator,'clipboard',{configurable:true,value:{write(items){touchWrites.push({trusted:navigator.userActivation.isActive,item:items[0]});return Promise.resolve();}}});})()`);
    const area=await evaluate(`(()=>{const r=canvas.getBoundingClientRect(),a=Module.editorInputArea;return {x:r.left+a.x*r.width/canvas.width,y:r.top+a.y*r.height/canvas.height,w:a.w*r.width/canvas.width,h:a.h*r.height/canvas.height};})()`);
    assert.ok(area.x<40 && area.w>300,'phone sidebar defaults collapsed');
    await screenshot('browser-touch-before.png');
    const x=area.x+80,y=area.y+250;
    await touch('touchStart',x,y);await sleep(30);
    for(let delta=20;delta<=140;delta+=20){await touch('touchMove',x,y-delta);await sleep(25);}
    await touch('touchEnd',x,y-140);await sleep(200);
    assert.equal(await evaluate('touchFocuses'),0,'swipe must not focus the hidden keyboard input');
    assert.notEqual(await evaluate('document.activeElement.id'),'editor-input');assert.deepEqual(await read(),baseline);assert.ok(await idle());
    await screenshot('browser-touch-scrolled.png');
    // A tap activates input only after its release; the touched row now comes
    // from the scrolled body, which is checked through a reversible insertion.
    await tap(x,area.y+45);assert.ok(await evaluate('touchFocuses>0'));assert.equal(await evaluate('touchStacks.at(-1)'),true,'unfocused editor tap must focus within pointerup stack');
    await cdp('Input.insertText',{text:'TOUCH_MARK'});
    await until(async()=>walk(await read()).some(n=>n.text?.includes('TOUCH_MARK')) && await idle(),'tap edit saved');
    const edited=walk(await read()).filter(n=>n.type==='paragraph').map(n=>walk(n).filter(x=>x.type==='text').map(x=>x.text).join('')).find(text=>text.includes('TOUCH_MARK'));assert.match(edited.replace('TOUCH_MARK',''),/Line 0[2-9]/,'swipe moved the document before hit testing');
    await key('z','KeyZ',90,2);await until(async()=>JSON.stringify(await read())===JSON.stringify(baseline)&&await idle(),'tap edit one undo');
    await evaluate("document.getElementById('editor-input').blur();canvas.focus();touchFocuses=0");
    await touch('touchStart',x,area.y+60);await sleep(650);await touch('touchMove',x+95,area.y+95);await touch('touchEnd',x+95,area.y+95);await sleep(150);
    assert.equal(await evaluate('touchFocuses'),0,'long press and dragging do not open keyboard');
    await screenshot('browser-touch-selected.png');
    // Floating action row is computed in logical CSS pixels, independent of DPR.
    await tap(60,844-58);
    await until(()=>evaluate('touchWrites.length===1'),'touch copy completes');
    assert.equal(await evaluate('touchWrites[0].trusted'),true,'clipboard API begins inside trusted pointerup');
    const selected=await evaluate("touchWrites[0].item.getType('text/plain').then(b=>b.text())");assert.ok(selected.length>1 && selected.length<200);
    assert.deepEqual(await read(),baseline);
    // Full rich copy through DOM DataTransfer includes citation identity and
    // readable HTML. Pasting repairs duplicate IDs and regenerates bibliography.
    await tap(x,area.y+45);await key('a','KeyA',65,2);
    const formats=await evaluate(`(()=>{window.richClipboard=new DataTransfer();canvas.focus();canvas.dispatchEvent(new ClipboardEvent('copy',{clipboardData:richClipboard,bubbles:true,cancelable:true}));return [...richClipboard.types];})()`);
    assert.ok(formats.includes('text/html')&&formats.includes('application/x-notelab-document-slice+json'));
    await key('Home','Home',36,2);
    await evaluate("canvas.dispatchEvent(new ClipboardEvent('paste',{clipboardData:richClipboard,bubbles:true,cancelable:true}))");
    await until(async()=>cites(await read()).length===2&&await idle(),'typed citation paste');
    const twice=await read(),two=cites(twice);assert.notEqual(two[0].attrs.citationId,two[1].attrs.citationId);
    assert.equal(twice.attrs.unknownTouchMetadata,4503599627370501);
    for(const cite of two)assert.ok(cite.attrs.cslRenderJson && JSON.parse(cite.attrs.cslJson).id===item.id);
    const bibliography=walk(twice).filter(n=>n.type==='zoteroEndnoteSection');assert.equal(bibliography.length,1);assert.equal(JSON.parse(bibliography[0].attrs.cslRenderJson).entries.length,1);
    await key('z','KeyZ',90,2);await until(async()=>JSON.stringify(await read())===JSON.stringify(baseline)&&await idle(),'typed citation paste one undo');
    await key('Home','Home',36,2);
    await evaluate(`(()=>{const data=new DataTransfer();data.setData('text/plain',richClipboard.getData('text/plain'));data.setData('text/html',richClipboard.getData('text/html'));canvas.dispatchEvent(new ClipboardEvent('paste',{clipboardData:data,bubbles:true,cancelable:true}));})()`);
    await until(async()=>cites(await read()).length===2&&await idle(),'HTML envelope citation paste');
    assert.notEqual(...cites(await read()).map(n=>n.attrs.citationId));
    await key('z','KeyZ',90,2);await until(async()=>JSON.stringify(await read())===JSON.stringify(baseline)&&await idle(),'HTML citation paste one undo');
    await cdp('Page.reload');await ready();await until(async()=>JSON.stringify(await read())===JSON.stringify(baseline)&&await idle(),'IndexedDB reload after rich clipboard undo');
    assert.deepEqual(exceptions,[]);
    await Promise.all([rm(join(build,'browser-touch-failure.json'),{force:true}),rm(join(build,'browser-touch-failure.png'),{force:true})]);
    console.log('PASS: actual DPR2 Chrome touch swipe/tap/long-press, trusted clipboard activation, typed/HTML citation ID repair, one-step undo and durable reload');
} catch(error) {
    if(evaluate)try{await writeFile(join(build,'browser-touch-failure.json'),JSON.stringify(await evaluate('({title:document.title,focus:document.activeElement.id,focuses:window.touchFocuses,unsaved:Module.editorUnsaved,pending:Module.editorPersistencePending,area:Module.editorInputArea,viewport:{innerWidth,innerHeight,dpr:devicePixelRatio},writes:window.touchWrites?.length})'),null,2));await writeFile(join(build,'browser-touch-failure.png'),Buffer.from((await cdp('Page.captureScreenshot')).data,'base64'));}catch{}
    console.error(stderr);throw error;
} finally {
    // Browser.close gives Chrome a chance to stop its workers and finish profile
    // writes. Killing only the parent can race a renderer recreating Default/.
    if (cdp && ws?.readyState === WebSocket.OPEN && !browserClosed) {
        try { await Promise.race([cdp('Browser.close'), sleep(1000)]); } catch {}
    }
    try { ws?.close(); } catch {}
    for (const request of pending.values()) request.reject(new Error('test finished'));
    server.close(); server.closeAllConnections();
    await Promise.race([browserFinished, sleep(3000)]);
    if (!browserClosed) {
        chrome.kill();
        await Promise.race([browserFinished, sleep(3000)]);
    }
    if (!browserClosed) {
        chrome.kill('SIGKILL');
        await Promise.race([browserFinished, sleep(3000)]);
    }
    await rm(profile, {recursive:true,force:true,maxRetries:8,retryDelay:200});
}
