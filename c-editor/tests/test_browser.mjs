// Real Chromium/WebAssembly/IndexedDB integration. Uses a disposable browser
// profile and temporary HTTP origin; never opens the user's browser or notes.
// node tests/test_browser.mjs <wasm-build-dir> [chromium-executable]
import assert from 'node:assert/strict';
import {spawn} from 'node:child_process';
import {mkdtemp, readFile, writeFile, rm, mkdir, readdir, open} from 'node:fs/promises';
import {createServer} from 'node:http';
import {tmpdir} from 'node:os';
import {join, extname, resolve, sep} from 'node:path';

const build = resolve(process.argv[2] || 'c-editor/build-web');
const cslSources = JSON.parse(await readFile(new URL('../csl/resources/SOURCES.json', import.meta.url),'utf8'));
const styleFiles = {apa:'apa.csl',ieee:'ieee.csl','gbt-7714-2015-numeric':'china-national-standard-gb-t-7714-2015-numeric.csl'};
const controlledRuns = runs => {
    assert.ok(Array.isArray(runs));
    for (const run of runs) {
        assert.deepEqual(Object.keys(run).sort(),run.href === undefined ? ['format','text'] : ['format','href','text']);
        assert.equal(typeof run.text,'string'); assert.ok(Number.isInteger(run.format) && run.format >= 0 && run.format <= 255);
        if (run.href !== undefined) assert.match(run.href,/^https?:\/\//);
    }
    return runs.map(run=>run.text).join('');
};
function formalCache(attrs,kind,profile) {
    assert.equal(typeof attrs?.['data-csl-render-json'],'string',`${kind} controlled cache must be saved`);
    const cache=JSON.parse(attrs['data-csl-render-json']);
    assert.equal(cache.version,1); assert.equal(cache.kind,kind); assert.equal(cache.processorProfile,'notelab-csl/1');
    assert.match(cache.sourceDigest,/^[a-f0-9]{64}$/); assert.equal(cache.style.profile,profile);
    const pin=cslSources.resources.find(resource=>resource.file===styleFiles[profile]); assert.ok(pin);
    assert.equal(cache.style.id,pin.id); assert.equal(cache.style.sha256,pin.sha256); assert.equal(cache.style.commit,pin.commit);
    assert.match(cache.style.locale,/^(en-us|zh-cn)$/);
    assert.equal(cache.style.localeCommit,cslSources.resources.find(resource=>resource.file==='locales-en-US.xml').commit);
    if(kind==='citation') {
        assert.equal(cache.citationId,attrs.citationid); controlledRuns(cache.runs);
    } else {
        assert.ok(Array.isArray(cache.entries));
        for(const entry of cache.entries) {
            assert.equal(typeof entry.sourceItemId,'string'); assert.ok(Number.isSafeInteger(entry.number) && entry.number>0);
            assert.equal(typeof entry.visible,'boolean'); assert.ok(Array.isArray(entry.rows));
            for(const row of entry.rows) {
                assert.ok(['flow','block','indent','two-columns'].includes(row.kind)); controlledRuns(row.body);
                if(row.left) controlledRuns(row.left);
            }
        }
    }
    return cache;
}
function formalState(state,profile) {
    assert.equal(state.settings?.['data-citation-profile'],profile);
    assert.equal(state.cites.length,1); assert.equal(state.tails.length,1);
    assert.equal(state.tail['data-bibliography-mode'],'citations');
    const citation=formalCache(state.cite,'citation',profile),bibliography=formalCache(state.tail,'bibliography',profile);
    assert.equal(citation.sourceDigest,bibliography.sourceDigest); assert.equal(bibliography.entries.length,1);
    assert.equal(bibliography.entries[0].number,1); assert.equal(bibliography.entries[0].visible,true);
    return {citation,bibliography};
}
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
    const file = '/notes/浏览器测试.md';
    const fileRead = `FS.readFile(${JSON.stringify(file)},{encoding:'utf8'})`;
    const read = () => evaluate(fileRead);
    const durableRead = () => evaluate(`new Promise((resolve,reject)=>{
        const request=indexedDB.open('/notes');
        request.onerror=()=>reject(request.error);
        request.onsuccess=()=>{const db=request.result;const get=db.transaction('FILE_DATA').objectStore('FILE_DATA').get(${JSON.stringify(file)});
            get.onsuccess=()=>{resolve(get.result ? new TextDecoder().decode(get.result.contents) : null);db.close();};
            get.onerror=()=>{reject(get.error);db.close();};};})`);
    async function click(x,y) {
        await cdp('Input.dispatchMouseEvent', {type:'mousePressed', x,y,button:'left',clickCount:1});
        await cdp('Input.dispatchMouseEvent', {type:'mouseReleased', x,y,button:'left',clickCount:1});
        await sleep(80);
    }
    async function key(key, code, virtual, modifiers = 0) {
        // SDL tracks physical modifier keys, not just CDP's DOM modifier flags.
        const held = [[1,'Alt','AltLeft',18],[2,'Control','ControlLeft',17],[4,'Meta','MetaLeft',91],[8,'Shift','ShiftLeft',16]].filter(([bit]) => modifiers & bit);
        for (const [,name,scan,vk] of held) await cdp('Input.dispatchKeyEvent', {type:'rawKeyDown',key:name,code:scan,modifiers,windowsVirtualKeyCode:vk,nativeVirtualKeyCode:vk});
        await cdp('Input.dispatchKeyEvent', {type:'rawKeyDown',key,code,modifiers,windowsVirtualKeyCode:virtual,nativeVirtualKeyCode:virtual});
        await cdp('Input.dispatchKeyEvent', {type:'keyUp',key,code,modifiers,windowsVirtualKeyCode:virtual,nativeVirtualKeyCode:virtual});
        for (const [,name,scan,vk] of held.reverse()) await cdp('Input.dispatchKeyEvent', {type:'keyUp',key:name,code:scan,modifiers:0,windowsVirtualKeyCode:vk,nativeVirtualKeyCode:vk});
        await sleep(80);
    }
    async function saved(expected) {
        await until(async () => await read() === expected && await evaluate('!Module.editorUnsaved && !Module.editorPersistencePending'), 'durable save');
        assert.equal(await durableRead(), expected);
    }
    const ready = async () => { await until(() => evaluate("typeof FS !== 'undefined' && document.getElementById('status').hidden && !!Module.editorInputUpdate"), 'application initialization');
        // Existing format regressions exercise the expanded toolbar.
        if (await evaluate('Module.editorInputArea && Module.editorInputArea.y * canvas.getBoundingClientRect().height / canvas.height < 160')) {
            const toolsPoint=await evaluate('(()=>{const r=canvas.getBoundingClientRect();return {x:r.right-140,y:r.top+24};})()');
            await click(toolsPoint.x,toolsPoint.y);
            await until(() => evaluate('Module.editorInputArea.y * canvas.getBoundingClientRect().height / canvas.height >= 160'), 'expanded writing tools');
        }
    };
    await cdp('Page.enable'); await cdp('Runtime.enable');
    await cdp('Emulation.setDeviceMetricsOverride', {width:1100,height:760,deviceScaleFactor:1,mobile:false});
    await cdp('Page.navigate', {url: origin + '/notelab.html'});
    await cdp('Page.bringToFront'); await ready();
    const notices = await evaluate(`Promise.all(['/THIRD_PARTY.md','/licenses/SOURCES.json','/licenses/cJSON.txt'].map(async path=>{
        const response=await fetch(path);if(!response.ok)throw new Error(path+': '+response.status);return response.text();
    }))`);
    assert.match(notices[0],/SDL/);
    assert.ok(JSON.parse(notices[1]).licenses.some(license=>license.file==='cJSON.txt' && license.sha256));
    assert.match(notices[2],/Permission is hereby granted/);
    console.log('PASS: browser-distributed notices, license provenance and license text');
    await click(125,65);
    await until(() => evaluate("document.activeElement.id === 'editor-input'"), 'filename input focus');
    await cdp('Input.insertText', {text:'浏览器测试'}); await key('Enter','Enter',13);
    await until(() => evaluate(`FS.analyzePath(${JSON.stringify(file)}).exists`), 'note creation');
    await click(400,200);
    await cdp('Input.insertText', {text:'Hello 中文🙂'});
    await saved('<p>Hello 中文🙂</p>');
    console.log('PASS: native browser text input, Unicode filename and IndexedDB save');

    await cdp('Input.imeSetComposition', {text:'nihao',selectionStart:5,selectionEnd:5});
    await sleep(1200);
    assert.equal(await read(), '<p>Hello 中文🙂</p>');
    assert.equal(await evaluate('Module.editorUnsaved'), false);
    await cdp('Input.imeSetComposition', {text:'',selectionStart:0,selectionEnd:0});
    await sleep(100);
    await cdp('Input.imeSetComposition', {text:'nihao',selectionStart:5,selectionEnd:5});
    await cdp('Input.insertText', {text:'你好'});
    await saved('<p>Hello 中文🙂你好</p>');
    console.log('PASS: IME preedit is not saved; cancellation and Unicode commit');

    await key('Enter','Enter',13);
    await saved('<p>Hello 中文🙂你好</p><p></p>');
    await key('Backspace','Backspace',8);
    await saved('<p>Hello 中文🙂你好</p>');
    await cdp('Input.insertText', {text:'X'});
    await evaluate(`document.getElementById('editor-input').dispatchEvent(new InputEvent('beforeinput',{inputType:'deleteContentBackward',bubbles:true,cancelable:true}))`);
    await saved('<p>Hello 中文🙂你好</p>');
    console.log('PASS: physical Enter/Backspace do not duplicate; software deletion');

    await evaluate(`window.realSyncfs=FS.syncfs; FS.syncfs=function(populate,callback){window.heldFlush=()=>realSyncfs.call(FS,populate,callback);};`);
    await cdp('Input.insertText', {text:' A'});
    await until(() => evaluate('!!window.heldFlush && Module.editorPersistencePending && Module.editorUnsaved'), 'pending save stays dirty');
    const pageBeforeCancel = await evaluate('performance.timeOrigin');
    const canceledReload = cdp('Page.reload');
    await until(() => dialogs.some(dialog => dialog.type === 'beforeunload'), 'unsaved navigation warning');
    await cdp('Page.handleJavaScriptDialog', {accept:false}); await canceledReload;
    assert.equal(await evaluate('performance.timeOrigin'), pageBeforeCancel);
    await cdp('Input.insertText', {text:'B'});
    await evaluate('FS.syncfs=realSyncfs; window.heldFlush(); delete window.heldFlush;');
    await saved('<p>Hello 中文🙂你好 AB</p>');
    console.log('PASS: canceling navigation keeps the app alive; edits during pending flush are saved');

    await evaluate(`FS.syncfs=function(populate,callback){setTimeout(()=>callback(new DOMException('test quota','QuotaExceededError')),0);};`);
    await cdp('Input.insertText', {text:' retry'});
    await until(() => evaluate(`${fileRead}.includes('retry') && Module.editorUnsaved && !Module.editorPersistencePending`), 'failed write keeps unsaved flag');
    assert.equal(await durableRead(), '<p>Hello 中文🙂你好 AB</p>');
    await evaluate('FS.syncfs=realSyncfs;'); await click(205,65);
    await saved('<p>Hello 中文🙂你好 AB retry</p>');
    console.log('PASS: failed IndexedDB write preserves prior storage; retry succeeds');

    const oldTime = await evaluate('performance.timeOrigin');
    await cdp('Page.reload');
    await until(() => evaluate(`performance.timeOrigin !== ${oldTime}`), 'new page context');
    await ready(); await cdp('Page.bringToFront');
    assert.equal(await read(), '<p>Hello 中文🙂你好 AB retry</p>');
    await writeFile(join(build,'browser-validation.png'), Buffer.from((await cdp('Page.captureScreenshot')).data,'base64'));
    console.log('PASS: full reload restores persisted note');

    const downloads = join(profile, 'downloads'); await mkdir(downloads);
    await cdp('Browser.setDownloadBehavior', {behavior:'allow',downloadPath:downloads,eventsEnabled:true});
    await click(360,106);
    const downloaded = await until(async () => (await readdir(downloads)).find(name => name.endsWith('.json')), 'JSON download');
    const exported = JSON.parse(await readFile(join(downloads,downloaded),'utf8'));
    assert.equal(exported.type, 'doc');
    assert.ok(JSON.stringify(exported).includes('Hello 中文🙂你好 AB retry'));
    console.log('PASS: JSON export downloads the current structured document');

    const {identifier} = await cdp('Page.addScriptToEvaluateOnNewDocument', {source:`indexedDB.open=()=>{throw new DOMException('test blocked storage','SecurityError');};`});
    await cdp('Page.reload');
    await until(() => evaluate("document.getElementById('status').textContent.includes('无法读取浏览器存储')"), 'blocked storage error');
    assert.equal(await evaluate("document.getElementById('status').hidden"), false);
    assert.equal(await evaluate('!!Module.editorInputUpdate'), false);
    await cdp('Page.removeScriptToEvaluateOnNewDocument', {identifier});
    await cdp('Page.reload'); await ready();
    assert.equal(await read(), '<p>Hello 中文🙂你好 AB retry</p>');
    await until(() => evaluate("document.activeElement.id === 'editor-input'"), 'restored editor focus');
    await key('End','End',35);
    const queued = await evaluate(`(() => {
        const input=document.getElementById('editor-input'); input.value=' queued';
        input.dispatchEvent(new InputEvent('input',{data:' queued',inputType:'insertText',bubbles:true}));
        const warning=new Event('beforeunload',{cancelable:true}); window.dispatchEvent(warning);
        return {pending:Module.editorInputPending,warning:warning.defaultPrevented};
    })()`);
    assert.deepEqual(queued, {pending:true,warning:true});
    await saved('<p>Hello 中文🙂你好 AB retry queued</p>');
    console.log('PASS: input queued before the next frame still warns on navigation');

    await cdp('Emulation.setDeviceMetricsOverride', {width:390,height:844,deviceScaleFactor:1,mobile:true});
    await until(() => evaluate('canvas.width === 390 && canvas.height === 844'), 'responsive browser canvas');
    // A fresh narrow layout exposes the editor without closing an overlay.
    await writeFile(join(build,'browser-mobile-validation.png'), Buffer.from((await cdp('Page.captureScreenshot')).data,'base64'));
    console.log('PASS: browser canvas follows a narrow mobile viewport');
    await cdp('Emulation.setDeviceMetricsOverride',{width:390,height:844,deviceScaleFactor:3,mobile:true});
    await cdp('Page.reload'); await ready();
    await until(() => evaluate('canvas.width === 1170 && canvas.height === 2532'), 'mobile DPR 3 drawing buffer');
    await click(125,65); await until(() => evaluate("document.activeElement.id === 'editor-input'"), 'DPR 3 filename hit test');
    const promptPosition = await evaluate("({left:parseFloat(document.getElementById('editor-input').style.left),top:parseFloat(document.getElementById('editor-input').style.top)})");
    assert.ok(promptPosition.left >= 20 && promptPosition.left <= 30 && promptPosition.top > 300 && promptPosition.top < 600,
        'native IME textarea must use CSS coordinates on a DPR 3 screen');
    await cdp('Input.insertText',{text:'MobileHiDPI'}); await key('Enter','Enter',13);
    const mobileFile = '/notes/MobileHiDPI.md';
    await until(() => evaluate(`FS.analyzePath(${JSON.stringify(mobileFile)}).exists`), 'DPR 3 note creation');
    // Opening a note already dismisses the narrow-screen sidebar overlay.
    const editorPoint = await evaluate(`(()=>{const r=canvas.getBoundingClientRect(),a=Module.editorInputArea;
        return {x:r.left+a.x*r.width/canvas.width,y:r.top+a.y*r.height/canvas.height};})()`);
    assert.ok(editorPoint.x >= 20 && editorPoint.x <= 30);
    await click(editorPoint.x+1,editorPoint.y+10);
    await cdp('Input.insertText',{text:'首行\n第二行'});
    const mobileRead = `FS.readFile(${JSON.stringify(mobileFile)},{encoding:'utf8'})`;
    await until(() => evaluate(`${mobileRead} === '<p>首行</p><p>第二行</p>' && !Module.editorUnsaved && !Module.editorPersistencePending`), 'DPR 3 text input');
    await cdp('Input.imeSetComposition',{text:'zhongwen',selectionStart:8,selectionEnd:8});
    await sleep(1000);
    assert.equal(await evaluate(mobileRead),'<p>首行</p><p>第二行</p>');
    assert.equal(await evaluate('Module.editorUnsaved'),false);
    await cdp('Input.insertText',{text:'中文'});
    await until(() => evaluate(`${mobileRead} === '<p>首行</p><p>第二行中文</p>' && !Module.editorUnsaved && !Module.editorPersistencePending`), 'DPR 3 IME commit');
    await click(editorPoint.x+1,editorPoint.y+10); await cdp('Input.insertText',{text:'点'});
    await until(() => evaluate(`${mobileRead} === '<p>点首行</p><p>第二行中文</p>' && !Module.editorUnsaved && !Module.editorPersistencePending`), 'DPR 3 canvas caret hit test');
    await writeFile(join(build,'browser-hidpi-validation.png'),Buffer.from((await cdp('Page.captureScreenshot')).data,'base64'));
    console.log('PASS: DPR 3 mobile layout, toolbar/caret hit tests and IME preedit/commit');
    await cdp('Emulation.setDeviceMetricsOverride', {width:1100,height:760,deviceScaleFactor:1,mobile:false});
    await until(() => evaluate('canvas.width === 1100'), 'desktop viewport restoration');
    await click(125,65); await until(() => evaluate("document.activeElement.id === 'editor-input'"), 'emoji filename focus');
    await cdp('Input.insertText', {text:'Emoji🙂 glyph check'}); await key('Enter','Enter',13);
    const emojiFile = '/notes/Emoji🙂 glyph check.md';
    await until(() => evaluate(`FS.analyzePath(${JSON.stringify(emojiFile)}).exists`), 'emoji note creation');
    await click(400,200);
    const emojiSample = '🙂 👍🏽 👩‍💻 👨‍👩‍👧‍👦 🇨🇳';
    await cdp('Input.insertText', {text:emojiSample});
    await until(() => evaluate(`FS.readFile(${JSON.stringify(emojiFile)},{encoding:'utf8'}).includes(${JSON.stringify(emojiSample)}) && !Module.editorUnsaved && !Module.editorPersistencePending`), 'emoji save');
    await writeFile(join(build,'browser-emoji-validation.png'), Buffer.from((await cdp('Page.captureScreenshot')).data,'base64'));
    await key('a','KeyA',65,2); await click(440,65);
    await until(() => evaluate(`FS.readFile(${JSON.stringify(emojiFile)},{encoding:'utf8'}).includes('<strong>') && !Module.editorUnsaved && !Module.editorPersistencePending`), 'formatted emoji save');
    await writeFile(join(build,'browser-emoji-bold-validation.png'), Buffer.from((await cdp('Page.captureScreenshot')).data,'base64'));
    console.log('PASS: emoji/ZWJ sequences preserve their bytes through styled editing (screenshots require visual glyph review)');
    await click(125,65);
    await until(() => evaluate("document.activeElement.id === 'editor-input'"), 'fast typing filename focus');
    await evaluate(`(() => {
        const input=document.getElementById('editor-input');
        input.value='Fast sequence'; input.dispatchEvent(new InputEvent('input',{data:input.value,inputType:'insertText',bubbles:true}));
        input.dispatchEvent(new KeyboardEvent('keydown',{key:'Enter',code:'Enter',keyCode:13,which:13,bubbles:true,cancelable:true}));
        input.dispatchEvent(new KeyboardEvent('keyup',{key:'Enter',code:'Enter',keyCode:13,which:13,bubbles:true,cancelable:true}));
        input.value='after Enter'; input.dispatchEvent(new InputEvent('input',{data:input.value,inputType:'insertText',bubbles:true}));
    })()`);
    await until(() => evaluate("FS.readFile('/notes/Fast sequence.md',{encoding:'utf8'}) === '<p>after Enter</p>' && !Module.editorUnsaved && !Module.editorPersistencePending"), 'typing after filename confirmation in one frame');
    console.log('PASS: rapid filename confirmation and following text retain SDL event order');
    await click(125,65); await until(() => evaluate("document.activeElement.id === 'editor-input'"), 'clipboard filename focus');
    await cdp('Input.insertText', {text:'Clipboard'}); await key('Enter','Enter',13);
    await until(() => evaluate("FS.analyzePath('/notes/Clipboard.md').exists"), 'clipboard note creation');
    await click(400,200);
    const clipboardText = 'Copy 中文🙂\nsecond';
    const clipboardHTML = '<p>Copy 中文🙂</p><p>second</p>';
    const clipboardRead = "FS.readFile('/notes/Clipboard.md',{encoding:'utf8'})";
    await cdp('Input.insertText', {text:clipboardText});
    const clipboardSaved = expected => until(() => evaluate(`${clipboardRead} === ${JSON.stringify(expected)} && !Module.editorUnsaved && !Module.editorPersistencePending`), 'clipboard document save');
    await clipboardSaved(clipboardHTML);
    await key('a','KeyA',65,2);
    // Use a real browser DataTransfer in DOM ClipboardEvents, isolated from the
    // user's operating-system clipboard. This checks the browser/C contract.
    const copied = await evaluate(`(() => {
        window.testClipboard = new DataTransfer();
        canvas.focus();
        const event=new ClipboardEvent('copy',{clipboardData:testClipboard,bubbles:true,cancelable:true});
        canvas.dispatchEvent(event); return {text:testClipboard.getData('text/plain'),handled:event.defaultPrevented};
    })()`);
    assert.deepEqual(copied, {text:clipboardText,handled:true});
    assert.equal(await evaluate(clipboardRead), clipboardHTML);
    for (const mode of ['throw','ignore']) {
        await evaluate(`(() => {
            const denied = new DataTransfer();
            denied.setData=${mode === 'throw' ? "()=>{throw new DOMException('test denied','NotAllowedError');}" : '()=>{}'};
            const event=new ClipboardEvent('cut',{clipboardData:denied,bubbles:true,cancelable:true});
            canvas.dispatchEvent(event);
        })()`);
        assert.equal(await evaluate(clipboardRead), clipboardHTML);
        assert.equal(await evaluate('Module.editorUnsaved'), false);
    }
    await evaluate(`canvas.dispatchEvent(new ClipboardEvent('cut',{clipboardData:testClipboard,bubbles:true,cancelable:true}))`);
    await clipboardSaved('<p></p>');
    await evaluate(`document.getElementById('editor-input').dispatchEvent(new ClipboardEvent('paste',{clipboardData:testClipboard,bubbles:true,cancelable:true}))`);
    await clipboardSaved(clipboardHTML);
    console.log('PASS: DOM copy/cut/paste roundtrip; throwing or ignored clipboard writes never delete text (system clipboard untouched)');
    if (process.env.EDITOR_TEST_SYSTEM_CLIPBOARD === '1') {
        // Opt-in only on a disposable CI machine. Do not enable on a user's host.
        await cdp('Browser.grantPermissions', {origin,permissions:['clipboardReadWrite','clipboardSanitizedWrite']});
        await key('a','KeyA',65,2);
        await key('c','KeyC',67,2); // Overwrite with the known fixture before reading.
        assert.equal(await evaluate('navigator.clipboard.readText()'), clipboardText);
        await key('x','KeyX',88,2); await clipboardSaved('<p></p>');
        await key('v','KeyV',86,2); await clipboardSaved(clipboardHTML);
        console.log('PASS: real system clipboard keyboard copy/cut/paste on disposable runner');
    }
    await evaluate(`window.testInputLog=[]; for(const type of ['keydown','keyup','beforeinput','input']) window.addEventListener(type,e=>{testInputLog.push({type:e.type,key:e.key,code:e.code,shift:e.shiftKey,ctrl:e.ctrlKey,data:e.data,inputType:e.inputType,value:e.target.value});if(testInputLog.length>100)testInputLog.shift();},true);`);
    await click(125,65); await until(() => evaluate("document.activeElement.id === 'editor-input'"), 'navigation filename focus');
    await cdp('Input.insertText', {text:'Navigation'}); await key('Enter','Enter',13);
    await until(() => evaluate("FS.analyzePath('/notes/Navigation.md').exists"), 'navigation note creation');
    await click(400,200); await cdp('Input.insertText', {text:'aaaa\nbbbb\ncccc'});
    await key('Home','Home',36); await key('ArrowUp','ArrowUp',38);
    await cdp('Input.insertText', {text:'Y'}); await key('End','End',35);
    await cdp('Input.insertText', {text:'Z'}); await key('ArrowDown','ArrowDown',40);
    await key('Home','Home',36,8); await cdp('Input.insertText', {text:'tail'});
    await until(() => evaluate("FS.readFile('/notes/Navigation.md',{encoding:'utf8'}) === '<p>aaaa</p><p>YbbbbZ</p><p>tail</p>' && !Module.editorUnsaved && !Module.editorPersistencePending"), 'visual line navigation and selection');
    const navigationPoint = await evaluate(`(()=>{const r=canvas.getBoundingClientRect(),a=Module.editorInputArea;
        return {x:r.left+a.x*r.width/canvas.width,y:r.top+a.y*r.height/canvas.height};})()`);
    await click(navigationPoint.x+1,navigationPoint.y+12); await cdp('Input.insertText', {text:'S'});
    await until(() => evaluate("FS.readFile('/notes/Navigation.md',{encoding:'utf8'}) === '<p>Saaaa</p><p>YbbbbZ</p><p>tail</p>' && !Module.editorUnsaved && !Module.editorPersistencePending"), 'canvas hit test');
    console.log('PASS: visual Up/Down, Home/End, Shift selection and canvas click editing');

    // A separate first-sorted note lets reload exercise actual document import,
    // followed by JSON export, rather than merely reading stored HTML bytes.
    await click(125,65); await until(() => evaluate("document.activeElement.id === 'editor-input'"), 'citation filename focus');
    await cdp('Input.insertText',{text:'000References'}); await key('Enter','Enter',13);
    const citationFile = '/notes/000References.md';
    await until(() => evaluate(`FS.analyzePath(${JSON.stringify(citationFile)}).exists`), 'citation note creation');
    await click(400,200); await cdp('Input.insertText',{text:'Before After'});
    await key('Home','Home',36);
    for (let i=0;i<7;i++) await key('ArrowRight','ArrowRight',39);
    await cdp('Page.setInterceptFileChooserDialog',{enabled:true});
    async function observeImports() { await evaluate(`window.importResults=[];window.importFinish=window.importFinish||_platform_import_web_finish;
        _platform_import_web_finish=(job,status,name,data,size,message)=>{
            window.importResults.push({status,name:name?UTF8ToString(name):'',size,
                bytes:data&&size<4096?Array.from(HEAPU8.slice(data,data+size)):null,
                message:message?UTF8ToString(message):''});
            window.importFinish(job,status,name,data,size,message);
        };`); }
    await observeImports();
    await click(1047,24);
    async function chooseImport(path, trigger = () => click(700,79)) {
        await until(() => evaluate('!Module.editorPersistencePending'), 'storage settled before file chooser');
        const before = await evaluate('window.importResults.length');
        await trigger();
        await until(() => evaluate("!!document.querySelector('[data-editor-import]')"), 'import file chooser');
        const root = (await cdp('DOM.getDocument')).root.nodeId;
        const input = (await cdp('DOM.querySelector',{nodeId:root,selector:'[data-editor-import]'})).nodeId;
        if (path) await cdp('DOM.setFileInputFiles',{nodeId:input,files:[path]});
        else await evaluate("document.querySelector('[data-editor-import]').dispatchEvent(new Event('cancel'))");
        await until(() => evaluate(`window.importResults.length === ${before + 1} && !document.querySelector('[data-editor-import]')`), 'import completion');
        await sleep(120);
        return evaluate('window.importResults.at(-1)');
    }
    const importedItem = {id:'browser-csl-one',type:'article-journal',title:'浏览器导入文献🙂',
        author:[{family:'张',given:'三'}],issued:{'date-parts':[[2024]]},
        DOI:'10.1000/browser', 'container-title':'测试期刊',volume:'12',page:'10-20'};
    const importedText = JSON.stringify([importedItem,{id:'browser-csl-two',type:'book',title:'另一本书'}]);
    const importPath = join(profile,'文献🙂.json'); await writeFile(importPath,importedText);
    let importResult = await chooseImport(importPath);
    assert.equal(importResult.status,1); assert.equal(importResult.name,'文献🙂.json');
    assert.deepEqual(importResult.bytes,Array.from(new TextEncoder().encode(importedText)));
    await evaluate('window.RealFileReader=FileReader;FileReader=class extends RealFileReader {readAsArrayBuffer(){throw new Error("injected import failure");}};');
    importResult = await chooseImport(importPath);
    assert.equal(importResult.status,3); assert.match(importResult.message,/injected import failure/);
    await evaluate('FileReader=window.RealFileReader');
    assert.equal((await chooseImport(importPath)).status,1, 'retry must work after a failed read');
    assert.equal((await chooseImport(null)).status,2);
    const oversizedPath = join(profile,'too-large.json');
    const oversized = await open(oversizedPath,'w');
    try { await oversized.truncate(128*1024*1024+1); } finally { await oversized.close(); }
    importResult = await chooseImport(oversizedPath);
    assert.equal(importResult.status,3); assert.match(importResult.message,/128/);
    const invalidPath = join(profile,'invalid.json'); await writeFile(invalidPath,'{"incomplete":');
    assert.equal((await chooseImport(invalidPath)).status,1, 'file bytes load; the library parser must reject them atomically');
    // Inserting the original item below proves cancel, limit and invalid JSON
    // did not discard the previously imported library.
    await click(860,131); await until(() => evaluate("document.activeElement.id === 'editor-input'"), 'library search focus');
    await cdp('Input.insertText',{text:'浏览器导入'}); await key('Enter','Enter',13);
    await click(700,263);
    await click(700,576); await cdp('Input.insertText',{text:'12–13'}); await key('Enter','Enter',13);
    const quotation = '摘录第一行🙂\n第二行';
    await click(970,576); await cdp('Input.insertText',{text:quotation}); await key('Enter','Enter',13,2);
    await click(700,627);
    const citationRead = `FS.readFile(${JSON.stringify(citationFile)},{encoding:'utf8'})`;
    const citationState = () => evaluate(`(()=>{
        const html=${citationRead}, doc=new DOMParser().parseFromString(html,'text/html');
        const cite=doc.querySelector('[data-type="zoteroCitationBlock"]');
        const tail=doc.querySelector('[data-type="zoteroEndnoteSection"]');
        const attrs=node=>node?Object.fromEntries(Array.from(node.attributes).map(a=>[a.name,a.value])):null;
        return {html,title:document.title,saved:!Module.editorUnsaved&&!Module.editorPersistencePending,
            cite:attrs(cite),tail:attrs(tail),settings:attrs(doc.querySelector('[data-type="notelabDocument"]')),
            cites:Array.from(doc.querySelectorAll('[data-type="zoteroCitationBlock"]')).map(attrs),
            tails:Array.from(doc.querySelectorAll('[data-type="zoteroEndnoteSection"]')).map(attrs)};
    })()`);
    await until(async () => { const s=await citationState(); return s.saved && s.cite; }, 'citation durable insertion');
    let cited = await citationState();
    assert.equal(cited.cite.title,importedItem.title); assert.equal(cited.cite.page,'12–13');
    assert.equal(cited.cite.note,quotation); assert.equal(cited.cite['data-source-id'],importedItem.id);
    assert.deepEqual(JSON.parse(cited.cite['data-csl-json']),importedItem);
    assert.match(cited.html,/<p>Before <\/p>/); assert.match(cited.html,/<p>After<\/p>/);
    let formal=formalState(cited,'apa');
    assert.match(controlledRuns(formal.citation.runs),/2024/); assert.match(controlledRuns(formal.citation.runs),/12[–-]13/);
    const firstBibliography=formal.bibliography.entries[0].rows.map(row=>controlledRuns(row.body)).join('\n');
    assert.ok(firstBibliography.includes(importedItem.title)); assert.ok(firstBibliography.includes(importedItem['container-title']));
    assert.ok(!controlledRuns(formal.citation.runs).includes(quotation),'quotation is separate from the CSL locator');
    const apaDigest=formal.citation.sourceDigest;
    await key('z','KeyZ',90,2);
    await until(async()=>{const s=await citationState();return s.saved && !s.cite && !s.tail && s.html==='<p>Before After</p>';},'one undo removes citation and automatic bibliography');
    await key('y','KeyY',89,2);
    await until(async()=>{const s=await citationState();return s.saved && s.cite && s.tail;},'one redo restores citation and automatic bibliography');
    cited=await citationState(); formal=formalState(cited,'apa'); assert.equal(formal.citation.sourceDigest,apaDigest);
    console.log('PASS: real file import preserves Unicode/CSL metadata; cancel, read errors, oversized and invalid files retain the library');
    console.log('PASS: formal C citation and automatic structured bibliography share pinned provenance; one undo/redo covers both');
    await click(1047,24); await click(700,697);
    await until(async () => { const s=await citationState(); return s.saved && s.settings?.['data-citation-profile'] === 'ieee'; }, 'IEEE formal style change');
    cited = await citationState();
    formal=formalState(cited,'ieee'); assert.equal(controlledRuns(formal.citation.runs),'[1, pp. 12–13]');
    const ieeeDigest=formal.citation.sourceDigest; assert.notEqual(ieeeDigest,apaDigest);
    assert.equal(cited.cite.endnotenumber,'1');
    const citationId = cited.cite.citationid;
    await key('Escape','Escape',27); await key('z','KeyZ',90,2);
    await until(async()=>{const s=await citationState();return s.saved && s.settings?.['data-citation-profile']==='apa';},'one undo restores APA style and output');
    assert.equal(formalState(await citationState(),'apa').citation.sourceDigest,apaDigest);
    await key('y','KeyY',89,2);
    await until(async()=>{const s=await citationState();return s.saved && s.settings?.['data-citation-profile']==='ieee';},'one redo restores IEEE style and output');
    assert.equal(formalState(await citationState(),'ieee').citation.sourceDigest,ieeeDigest);
    await click(1047,24); await click(970,697);
    await until(async()=>{const s=await citationState();return s.saved && s.cite?.citationid===citationId;},'explicit synchronization retains formal citation identity');
    await click(700,576); await key('a','KeyA',65,2);
    await cdp('Input.insertText',{text:'44'}); await key('Enter','Enter',13); await click(700,627);
    await until(async () => { const s=await citationState(); return s.saved && s.cite?.page === '44'; }, 'edit existing citation');
    cited = await citationState(); assert.equal(cited.cite.citationid,citationId); assert.equal(cited.cite.note,quotation);
    formal=formalState(cited,'ieee'); assert.equal(controlledRuns(formal.citation.runs),'[1, p. 44]');
    const page44Digest=formal.citation.sourceDigest; assert.notEqual(page44Digest,ieeeDigest);
    await key('z','KeyZ',90,2);
    await until(async () => { const s=await citationState(); return s.saved && s.cite?.page === '12–13'; }, 'undo citation metadata edit');
    assert.equal(formalState(await citationState(),'ieee').citation.sourceDigest,ieeeDigest);
    await key('y','KeyY',89,2);
    await until(async () => { const s=await citationState(); return s.saved && s.cite?.page === '44'; }, 'redo citation metadata edit');
    assert.equal(formalState(await citationState(),'ieee').citation.sourceDigest,page44Digest);
    await click(1047,24);
    await writeFile(join(build,'browser-citation-validation.png'),Buffer.from((await cdp('Page.captureScreenshot')).data,'base64'));
    await click(1060,30); // Close the panel before checking reload serialization.
    const citationHTML = (await citationState()).html;
    await cdp('Page.reload'); await ready(); await cdp('Page.bringToFront');
    await until(() => evaluate('document.title.includes("000References")'), 'reloaded citation document');
    assert.equal(await evaluate(citationRead),citationHTML);
    formal=formalState(await citationState(),'ieee'); assert.equal(formal.citation.sourceDigest,page44Digest);
    // A syntactically valid cache with matching pins/digest is still untrusted
    // on import. Regeneration must replace forged controlled text, not merely
    // accept its version or claim to authenticate it using an unkeyed hash.
    await evaluate(`new Promise((resolve,reject)=>{
        const doc=new DOMParser().parseFromString(${citationRead},'text/html');
        for(const atom of doc.querySelectorAll('[data-csl-render-json]')) {
            const cache=JSON.parse(atom.getAttribute('data-csl-render-json'));
            if(cache.kind==='citation')cache.runs=[{text:'FORGED-CACHE-TEXT',format:0}];
            else cache.entries[0].rows[0].body=[{text:'FORGED-CACHE-TEXT',format:0}];
            atom.setAttribute('data-csl-render-json',JSON.stringify(cache));
        }
        FS.writeFile(${JSON.stringify(citationFile)},doc.body.innerHTML);FS.syncfs(false,e=>e?reject(e):resolve());
    })`);
    assert.ok((await evaluate(citationRead)).includes('FORGED-CACHE-TEXT'));
    await cdp('Page.reload'); await ready(); await cdp('Page.bringToFront');
    await until(async()=>{const s=await citationState();return s.saved && s.cite && !s.html.includes('FORGED-CACHE-TEXT');},'untrusted cache is freshly regenerated and durably replaced');
    formal=formalState(await citationState(),'ieee'); assert.equal(formal.citation.sourceDigest,page44Digest);
    assert.equal(controlledRuns(formal.citation.runs),'[1, p. 44]');
    await writeFile(join(build,'browser-citation-rebuilt-cache.png'),Buffer.from((await cdp('Page.captureScreenshot')).data,'base64'));
    // Advancing the actual toolbar after reload must start from persisted IEEE,
    // so its next profile is GB/T rather than resetting silently to APA.
    await click(1047,24); await click(700,697);
    await until(async()=>{const s=await citationState();return s.saved && s.settings?.['data-citation-profile']==='gbt-7714-2015-numeric';},'reloaded toolbar retains the chosen citation profile');
    formalState(await citationState(),'gbt-7714-2015-numeric');
    await key('Escape','Escape',27); await key('z','KeyZ',90,2);
    await until(async()=>{const s=await citationState();return s.saved && s.settings?.['data-citation-profile']==='ieee';},'undo restored-profile toolbar change');
    assert.equal(formalState(await citationState(),'ieee').citation.sourceDigest,page44Digest);
    for (const name of await readdir(downloads)) await rm(join(downloads,name),{force:true});
    await click(360,106);
    const citationDownload = await until(async () => (await readdir(downloads)).find(name => name.endsWith('.json')), 'reloaded citation JSON download');
    const citationJSON = JSON.parse(await readFile(join(downloads,citationDownload),'utf8'));
    const descendants = node => [node,...(node.content || []).flatMap(descendants)];
    const nodes = descendants(citationJSON);
    const roundtrip = nodes.find(node => node.type === 'zoteroCitationBlock');
    assert.ok(roundtrip); assert.equal(roundtrip.attrs.page,'44'); assert.equal(roundtrip.attrs.note,quotation);
    assert.deepEqual(JSON.parse(roundtrip.attrs.cslJson),importedItem);
    assert.equal(citationJSON.attrs.citationProfile,'ieee');
    assert.equal(citationJSON.attrs.citationLocale,undefined,'an implicit default locale must remain implicit');
    assert.equal(formalCache({'data-csl-render-json':roundtrip.attrs.cslRenderJson,citationid:roundtrip.attrs.citationId},'citation','ieee').sourceDigest,page44Digest);
    assert.equal(formalCache({'data-csl-render-json':nodes.find(node=>node.type==='zoteroEndnoteSection').attrs.cslRenderJson},'bibliography','ieee').sourceDigest,page44Digest);
    assert.equal(nodes.find(node => node.type === 'zoteroEndnoteSection').attrs.style,'ieee');
    console.log('PASS: formal styles/page edits are atomic, persist across reload, rebuild forged caches and preserve controlled JSON output');

    const cachePath = '/notes/.notelab-library/library.json';
    await until(() => evaluate(`FS.analyzePath(${JSON.stringify(cachePath)}).exists && !Module.editorPersistencePending`), 'private library cache');
    const validCache = await evaluate(`FS.readFile(${JSON.stringify(cachePath)},{encoding:'utf8'})`);
    assert.ok(JSON.parse(validCache));
    // The preceding reload must restore the library without another chooser.
    await click(1047,24); await click(860,79); await click(860,131);
    await until(() => evaluate("document.activeElement.id === 'editor-input'"), 'restored library search');
    await key('a','KeyA',65,2); await cdp('Input.insertText',{text:'浏览器导入'}); await key('Enter','Enter',13);
    await click(700,263); await click(700,627);
    const citationCount = () => evaluate(`new DOMParser().parseFromString(${citationRead},'text/html').querySelectorAll('[data-type="zoteroCitationBlock"]').length`);
    await until(async () => (await citationCount()) === 2 && await evaluate('!Module.editorUnsaved && !Module.editorPersistencePending'), 'insert from restored cache');
    const repeated=await citationState(), repeatedBibliography=formalCache(repeated.tail,'bibliography','ieee');
    assert.equal(repeatedBibliography.entries.length,1,'two cites of one identity produce one bibliography entry');
    assert.equal(new Set(repeated.cites.map(cite=>cite.citationid)).size,2);
    for(const cite of repeated.cites) assert.equal(formalCache(cite,'citation','ieee').sourceDigest,repeatedBibliography.sourceDigest);
    const beforeBadCache = await evaluate(citationRead);
    async function replaceCache(text) {
        await evaluate(`new Promise((resolve,reject)=>{FS.writeFile(${JSON.stringify(cachePath)},${JSON.stringify(text)});FS.syncfs(false,e=>e?reject(e):resolve());})`);
    }
    await replaceCache('{broken'); await cdp('Page.reload'); await ready();
    await until(() => evaluate('document.title.includes("000References")'), 'notes remain available with invalid cache');
    assert.equal(await evaluate(citationRead),beforeBadCache);
    assert.equal(await evaluate(`FS.readFile(${JSON.stringify(cachePath)},{encoding:'utf8'})`),'{broken');
    await click(1047,24); await click(860,79); await click(700,263); await click(700,627); await sleep(500);
    assert.equal(await evaluate(citationRead),beforeBadCache, 'invalid cache must not expose a phantom selectable item');
    await writeFile(join(build,'browser-library-invalid-cache.png'),Buffer.from((await cdp('Page.captureScreenshot')).data,'base64'));
    await replaceCache(validCache); await cdp('Page.reload'); await ready();
    await until(() => evaluate('document.title.includes("000References")'), 'valid cache recovery');
    assert.equal(await evaluate(citationRead),beforeBadCache);
    console.log('PASS: private library cache survives reload; corrupt cache preserves notes and its original bytes');

    await observeImports();
    async function documentChooser(path) {
        // Desktop note opening retains the sidebar state; ensure it is visible
        // using the editor area's actual position rather than toggling blindly.
        if (await evaluate('Module.editorInputArea.x < 200')) await click(45,65);
        return chooseImport(path, () => click(186,160));
    }
    const noteSnapshot = () => evaluate(`JSON.stringify(FS.readdir('/notes').filter(n=>!n.startsWith('.')&&/\\.(json|md|txt)$/.test(n)).sort().map(n=>[n,FS.readFile('/notes/'+n,{encoding:'utf8'})]))`);
    let notesBefore = await noteSnapshot(), titleBefore = await evaluate('document.title');
    assert.ok(JSON.parse(notesBefore).length > 0, 'preservation checks must include existing notes');
    assert.equal((await documentChooser(null)).status,2);
    assert.equal(await noteSnapshot(),notesBefore); assert.equal(await evaluate('document.title'),titleBefore);
    const brokenDocument = join(profile,'Broken document.json'); await writeFile(brokenDocument,'{"type":');
    assert.equal((await documentChooser(brokenDocument)).status,1); await sleep(300);
    assert.equal(await noteSnapshot(),notesBefore); assert.equal(await evaluate('document.title'),titleBefore);
    // Typed metadata requires JSON storage; the import must not flatten it to
    // HTML or discard the original CSL snapshot embedded in its citation.
    const legacyJSON = structuredClone(citationJSON);
    legacyJSON.attrs.citationLocale='en-US';
    descendants(legacyJSON).find(n=>n.type==='zoteroCitationBlock').attrs.customData={keep:'原始信息🙂',enabled:true};
    const legacyJSONPath = join(profile,'Legacy JSON🙂.json');
    const legacyJSONText = JSON.stringify(legacyJSON); await writeFile(legacyJSONPath,legacyJSONText);
    assert.equal((await documentChooser(legacyJSONPath)).status,1);
    const importedDocumentPath = '/notes/Legacy JSON🙂.json';
    await until(() => evaluate(`FS.analyzePath(${JSON.stringify(importedDocumentPath)}).exists && !Module.editorPersistencePending`), 'legacy JSON note creation');
    const firstImported = await evaluate(`FS.readFile(${JSON.stringify(importedDocumentPath)},{encoding:'utf8'})`);
    assert.deepEqual(JSON.parse(firstImported),legacyJSON);
    assert.equal(JSON.parse(firstImported).attrs.citationLocale,'en-US','explicit locale survives document import and reprocessing');
    assert.equal((await documentChooser(legacyJSONPath)).status,1);
    await until(() => evaluate("FS.analyzePath('/notes/Legacy JSON🙂 (2).json').exists && !Module.editorPersistencePending"), 'duplicate source creates a distinct note');
    assert.equal(await evaluate(`FS.readFile(${JSON.stringify(importedDocumentPath)},{encoding:'utf8'})`),firstImported);
    assert.equal(await readFile(legacyJSONPath,'utf8'),legacyJSONText);

    const htmlEntries = [{citationId:'legacy-html',number:1,entryHtml:'<i>旧作者</i>. 原始条目🙂.'}];
    const escapeAttribute = value => String(value).replaceAll('&','&amp;').replaceAll('"','&quot;').replaceAll('<','&lt;').replaceAll('>','&gt;');
    const oldHTML = '<h2><strong>旧笔记 HTML</strong></h2><p><em>正文🙂</em></p>'+
        '<div data-type="zoteroCitationBlock" citationId="legacy-html" data-citation-id="legacy-html" endnoteNumber="1" data-endnote-number="1" itemKey="ABCD1234" itemUri="zotero://select/library/items/ABCD1234" title="旧文献" author="旧作者" year="2020" citeKey="Old2020" page="8" note="旧摘录"></div>'+
        '<div data-type="zoteroEndnoteSection" style="apa" entriesJson="'+escapeAttribute(JSON.stringify(htmlEntries))+'"></div>';
    const oldHTMLPath = join(profile,'Legacy HTML.htm'); await writeFile(oldHTMLPath,oldHTML);
    assert.equal((await documentChooser(oldHTMLPath)).status,1);
    await until(() => evaluate("FS.analyzePath('/notes/Legacy HTML.json').exists && !Module.editorPersistencePending"), 'legacy HTML note creation');
    const htmlDocument = await evaluate("JSON.parse(FS.readFile('/notes/Legacy HTML.json',{encoding:'utf8'}))");
    const htmlNodes = descendants(htmlDocument);
    assert.equal(htmlNodes.find(n=>n.type==='heading').attrs.level,2);
    assert.ok(htmlNodes.find(n=>n.text==='旧笔记 HTML').marks.some(mark=>mark.type==='bold'));
    assert.ok(htmlNodes.find(n=>n.text==='正文🙂').marks.some(mark=>mark.type==='italic'));
    assert.equal(htmlNodes.find(n=>n.type==='zoteroCitationBlock').attrs.citationId,'legacy-html');
    assert.equal(htmlNodes.find(n=>n.type==='zoteroCitationBlock').attrs.page,'8');
    assert.deepEqual(JSON.parse(htmlNodes.find(n=>n.type==='zoteroEndnoteSection').attrs.entriesJson),htmlEntries);
    const literalPath = join(profile,'Literal.txt'); await writeFile(literalPath,'<literal>&\n中文🙂');
    assert.equal((await documentChooser(literalPath)).status,1);
    await until(() => evaluate("FS.analyzePath('/notes/Literal.json').exists && !Module.editorPersistencePending"), 'plain text note creation');
    const literal = await evaluate("JSON.parse(FS.readFile('/notes/Literal.json',{encoding:'utf8'}))");
    assert.equal(descendants(literal).filter(n=>n.type==='text').map(n=>n.text).join('\n'),'<literal>&\n中文🙂');
    notesBefore = await noteSnapshot(); titleBefore = await evaluate('document.title');
    const binaryPath = join(profile,'Binary.txt'); await writeFile(binaryPath,Buffer.from([65,0,66]));
    assert.equal((await documentChooser(binaryPath)).status,1); await sleep(300);
    assert.equal(await noteSnapshot(),notesBefore); assert.equal(await evaluate('document.title'),titleBefore);
    await writeFile(join(build,'browser-document-import-validation.png'),Buffer.from((await cdp('Page.captureScreenshot')).data,'base64'));
    console.log('PASS: actual JSON/legacy HTML/TXT note chooser, typed metadata, duplicate names and rejected/canceled imports');
    // Old Tauri could store an independently maintained bibliography without
    // any citation atoms. Its ordinary fields are not proof of derived data.
    const standalonePath='/notes/0000Standalone Legacy.json';
    const standaloneEntries=[{citationId:'legacy-bibliography-only',number:1,key:'Legacy2020',
        itemUri:'zotero://select/library/items/OLDENTRY',pdfUri:'',formatting:'approximate',
        entryHtml:'<em>Standalone legacy work &amp; source</em>. Old Author (2020).'}];
    const standaloneDoc={type:'doc',content:[
        {type:'paragraph',content:[{type:'text',text:'Standalone bibliography'}]},
        {type:'zoteroEndnoteSection',attrs:{style:'apa',entriesJson:JSON.stringify(standaloneEntries)}}]};
    const standaloneRead=()=>evaluate(`JSON.parse(FS.readFile(${JSON.stringify(standalonePath)},{encoding:'utf8'}))`);
    function standalonePreserved(doc) {
        const nodes=descendants(doc),sections=nodes.filter(node=>node.type==='zoteroEndnoteSection');
        assert.equal(nodes.filter(node=>node.type==='zoteroCitationBlock').length,0);
        assert.equal(sections.length,1); assert.equal(sections[0].attrs.style,'apa');
        assert.equal(sections[0].attrs.entriesJson,JSON.stringify(standaloneEntries));
        assert.deepEqual(JSON.parse(sections[0].attrs.entriesJson),standaloneEntries);
        assert.equal(Object.hasOwn(sections[0].attrs,'bibliographyMode'),false);
        assert.equal(Object.hasOwn(sections[0].attrs,'cslRenderJson'),false);
        assert.equal(Object.hasOwn(doc.attrs||{},'citationProfile'),false);
    }
    await until(()=>evaluate('!Module.editorUnsaved && !Module.editorPersistencePending'),'idle before standalone bibliography fixture');
    await evaluate(`new Promise((resolve,reject)=>{FS.writeFile(${JSON.stringify(standalonePath)},${JSON.stringify(JSON.stringify(standaloneDoc))});FS.syncfs(false,e=>e?reject(e):resolve());})`);
    await cdp('Page.reload'); await ready(); await cdp('Page.bringToFront');
    await until(()=>evaluate('document.title.includes("0000Standalone Legacy")'),'startup opens independent legacy bibliography');
    standalonePreserved(await standaloneRead());
    await click(400,185); await key('Home','Home',36); await cdp('Input.insertText',{text:'Edited '});
    await until(async()=>descendants(await standaloneRead()).filter(node=>node.type==='text').map(node=>node.text).join('')==='Edited Standalone bibliography' &&
        await evaluate('!Module.editorUnsaved && !Module.editorPersistencePending'),'save ordinary edit without deleting legacy bibliography');
    standalonePreserved(await standaloneRead());
    await cdp('Page.reload'); await ready(); await cdp('Page.bringToFront');
    await until(()=>evaluate('document.title.includes("0000Standalone Legacy")'),'reload independent legacy bibliography from IndexedDB');
    const restoredStandalone=await standaloneRead(); standalonePreserved(restoredStandalone);
    assert.equal(descendants(restoredStandalone).filter(node=>node.type==='text').map(node=>node.text).join(''),'Edited Standalone bibliography');
    await writeFile(join(build,'browser-standalone-bibliography-preserved.png'),Buffer.from((await cdp('Page.captureScreenshot')).data,'base64'));
    console.log('PASS: standalone legacy bibliography survives ordinary editing, IndexedDB save and reload without derived-mode relabeling');
    // Seed a legacy note without a snapshot directly in durable storage so its
    // first processing really happens at startup, before any manual library
    // action. The restored private library is its sole complete CSL source.
    const cachedLegacyPath='/notes/0000Cached Legacy.json';
    const cachedLegacy={type:'doc',attrs:{citationProfile:'apa',citationLocale:'en-US'},content:[
        {type:'paragraph',content:[{type:'text',text:'Restored library source'}]},
        {type:'zoteroCitationBlock',attrs:{citationId:'restored-legacy-source',sourceId:importedItem.id,page:'5',note:'保留摘录'}}]};
    await until(()=>evaluate('!Module.editorUnsaved && !Module.editorPersistencePending'),'idle before startup fallback fixture');
    await evaluate(`new Promise((resolve,reject)=>{FS.writeFile(${JSON.stringify(cachedLegacyPath)},${JSON.stringify(JSON.stringify(cachedLegacy))});FS.syncfs(false,e=>e?reject(e):resolve());})`);
    await cdp('Page.reload'); await ready(); await cdp('Page.bringToFront');
    await until(()=>evaluate('document.title.includes("0000Cached Legacy")'),'startup opens first-sorted legacy citation');
    const readCachedLegacy=()=>evaluate(`JSON.parse(FS.readFile(${JSON.stringify(cachedLegacyPath)},{encoding:'utf8'}))`);
    await until(async()=>{
        const doc=await readCachedLegacy(),cite=descendants(doc).find(node=>node.type==='zoteroCitationBlock');
        return cite?.attrs?.cslJson && cite?.attrs?.cslRenderJson && await evaluate('!Module.editorUnsaved && !Module.editorPersistencePending');
    },'startup restores library before resolving a missing embedded snapshot');
    const recoveredLegacy=await readCachedLegacy(),recoveredNodes=descendants(recoveredLegacy);
    const recoveredCite=recoveredNodes.find(node=>node.type==='zoteroCitationBlock'),recoveredBib=recoveredNodes.find(node=>node.type==='zoteroEndnoteSection');
    assert.deepEqual(JSON.parse(recoveredCite.attrs.cslJson),importedItem); assert.equal(recoveredCite.attrs.note,'保留摘录');
    assert.equal(recoveredLegacy.attrs.citationLocale,'en-US');
    const recoveredCache=formalCache({'data-csl-render-json':recoveredCite.attrs.cslRenderJson,citationid:recoveredCite.attrs.citationId},'citation','apa');
    assert.match(controlledRuns(recoveredCache.runs),/2024/);
    assert.equal(formalCache({'data-csl-render-json':recoveredBib.attrs.cslRenderJson},'bibliography','apa').sourceDigest,recoveredCache.sourceDigest);
    assert.equal(await evaluate(`FS.readFile(${JSON.stringify(cachePath)},{encoding:'utf8'})`),validCache);
    await writeFile(join(build,'browser-citation-cached-legacy.png'),Buffer.from((await cdp('Page.captureScreenshot')).data,'base64'));
    console.log('PASS: startup restores persisted library before resolving a legacy citation without an embedded snapshot');
    assert.deepEqual(exceptions, []);
    console.log('PASS: initial storage read failure blocks editor; data survives recovery');
    await Promise.all([rm(join(build,'browser-failure.json'),{force:true}),rm(join(build,'browser-failure.png'),{force:true})]);
    console.log('All browser integration checks passed (Chromium only; device keyboard UI is not covered).');
} catch (error) {
    if (evaluate) {
        try {
            const state = await evaluate(`({title:document.title,unsaved:Module.editorUnsaved,pending:Module.editorPersistencePending,inputPending:Module.editorInputPending,inputLog:window.testInputLog,viewport:{dpr:devicePixelRatio,innerWidth,innerHeight,canvas:[canvas.width,canvas.height],rect:canvas.getBoundingClientRect().toJSON(),input:Module.editorInputArea},notes:FS.readdir('/notes').filter(n=>!n.startsWith('.')).map(n=>({name:n,data:FS.readFile('/notes/'+n,{encoding:'utf8'})}))})`);
            await writeFile(join(build,'browser-failure.json'), JSON.stringify(state,null,2));
            console.error('Browser test state:', JSON.stringify(state));
            await writeFile(join(build,'browser-failure.png'), Buffer.from((await cdp('Page.captureScreenshot')).data,'base64'));
        } catch {}
    }
    console.error(stderr);
    throw error;
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
