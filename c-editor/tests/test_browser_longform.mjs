// Real WASM writing workflows in a fresh browser profile and isolated /notes.
// Fixture seeding and assertions use FS/IndexedDB; all edits use production UI.
import assert from 'node:assert/strict';
import {createHash} from 'node:crypto';
import {spawn} from 'node:child_process';
import {mkdtemp,readFile,writeFile,rm,mkdir} from 'node:fs/promises';
import {createServer} from 'node:http';
import {tmpdir} from 'node:os';
import {deflateSync} from 'node:zlib';
import {join,extname,resolve,sep} from 'node:path';

const build=resolve(process.argv[2]||'c-editor/build-web');
const executable=process.argv[3]||process.env.CHROME_BIN||
    (process.platform==='darwin'?'/Applications/Google Chrome.app/Contents/MacOS/Google Chrome':'chromium');
const cpuThrottle=Number(process.env.EDITOR_TEST_CPU_THROTTLE||1);
assert.ok(Number.isFinite(cpuThrottle)&&cpuThrottle>=1&&cpuThrottle<=20);
const profile=await mkdtemp(join(tmpdir(),'notelab-longform-browser-'));
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
const alpha='/notes/000 Outline.json',beta='/notes/001 Images.json',restored='/notes/002 Restored.json';
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
    const downloadDir=join(profile,'downloads');await mkdir(downloadDir);
    await cdp('Browser.setDownloadBehavior',{behavior:'allow',downloadPath:downloadDir,eventsEnabled:true});
    await cdp('Emulation.setDeviceMetricsOverride',{width:1100,height:760,deviceScaleFactor:1,mobile:false});
    if(cpuThrottle>1)await cdp('Emulation.setCPUThrottlingRate',{rate:cpuThrottle});
    await cdp('Page.navigate',{url:origin+'/notelab.html'});await cdp('Page.bringToFront');await ready();

    function crc32(bytes){let c=0xffffffff;for(const b of bytes){c^=b;for(let i=0;i<8;i++)c=(c>>>1)^((0-(c&1))&0xedb88320);}return(c^0xffffffff)>>>0;}
    function png(){
        const chunk=(type,data)=>{const t=Buffer.from(type),out=Buffer.alloc(data.length+12);out.writeUInt32BE(data.length);t.copy(out,4);data.copy(out,8);out.writeUInt32BE(crc32(Buffer.concat([t,data])),8+data.length);return out;};
        const header=Buffer.alloc(13);header.writeUInt32BE(240);header.writeUInt32BE(120,4);header[8]=8;header[9]=2;
        const pixels=Buffer.alloc(120*(1+240*3));for(let y=0;y<120;y++)for(let x=0;x<240;x++){const i=y*721+1+x*3;pixels[i]=x<120?40:230;pixels[i+1]=100+y;pixels[i+2]=x<120?210:60;}
        return Buffer.concat([Buffer.from([137,80,78,71,13,10,26,10]),chunk('IHDR',header),chunk('IDAT',deflateSync(pixels)),chunk('IEND',Buffer.alloc(0))]);
    }
    function unzip(bytes){
        const data=Buffer.from(bytes),entries=new Map();let at=0;
        while(data.readUInt32LE(at)===0x04034b50){
            assert.equal(data.readUInt16LE(at+8),0);const length=data.readUInt32LE(at+18),nameLength=data.readUInt16LE(at+26),extra=data.readUInt16LE(at+28);
            const name=data.subarray(at+30,at+30+nameLength).toString('utf8'),start=at+30+nameLength+extra,body=data.subarray(start,start+length);
            assert.equal(crc32(body),data.readUInt32LE(at+14));assert.ok(!entries.has(name));entries.set(name,body);at=start+length;
        }
        assert.equal(data.readUInt32LE(at),0x02014b50);return entries;
    }
    const p=text=>({type:'paragraph',content:text?[{type:'text',text}]:[]});
    const h=(level,text)=>({type:'heading',attrs:{level},content:[{type:'text',text}]});
    const originalAlpha=JSON.stringify({type:'doc',attrs:{untouched:'longform-fixture'},content:[h(1,'Introduction'),p('Opening body'),h(2,'Details'),p('Nested body'),h(1,'Conclusion'),p('Ending body')]}),originalBeta=JSON.stringify({type:'doc',content:[p(''),{type:'paragraph',content:[{type:'text',text:'Reference link',marks:[{type:'link',attrs:{href:'https://example.invalid/blog-reference'}}]}]}]});
    await evaluate(`new Promise((resolve,reject)=>{FS.writeFile(${JSON.stringify(alpha)},${JSON.stringify(originalAlpha)});FS.writeFile(${JSON.stringify(beta)},${JSON.stringify(originalBeta)});FS.syncfs(false,e=>e?reject(e):resolve());})`);
    await reload();await until(()=>evaluate('document.title.includes("000 Outline.json")'),'outline fixture article');
    const json=async path=>JSON.parse(await raw(path));
    const allNodes=node=>[node,...(node.content||[]).flatMap(allNodes)];
    async function persistWhere(path,predicate,label){await until(async()=>predicate(await json(path))&&await idle(),label);assert.equal(await durable(path),await raw(path));}
    console.log('STAGE: outline folding and section drag');
    await key('o','KeyO',79,10);await click(22,160);assert.equal(await raw(alpha),originalAlpha,'fold is presentation only');
    await click(90,200);await cdp('Input.insertText',{text:'Jump '});
    await persistWhere(alpha,doc=>treeText(doc.content[4])==='Jump Conclusion','folded outline jump targets next visible heading');
    await key('z','KeyZ',90,2);await persistWhere(alpha,doc=>treeText(doc.content[4])==='Conclusion','undo heading insertion');
    await cdp('Input.dispatchMouseEvent',{type:'mousePressed',x:222,y:160,button:'left',clickCount:1});
    for(const y of [180,200,218])await cdp('Input.dispatchMouseEvent',{type:'mouseMoved',x:222,y,buttons:1});
    await cdp('Input.dispatchMouseEvent',{type:'mouseReleased',x:222,y:218,button:'left',clickCount:1});await rendered();
    await persistWhere(alpha,doc=>treeText(doc.content[0])==='Conclusion'&&treeText(doc.content[2])==='Introduction'&&treeText(doc.content[4])==='Details','whole-section outline drag preserves nested body');
    await screenshot('browser-longform-outline-validation.png');await click(400,180);await key('z','KeyZ',90,2);
    await persistWhere(alpha,doc=>treeText(doc.content[0])==='Introduction','section movement single undo');
    pass('Folded outline navigation and drag reorder use actual pointer events, move complete nested sections, preserve metadata and undo');
    await key('o','KeyO',79,10);await click(95,200);await until(()=>evaluate('document.title.includes("001 Images.json")'),'image fixture selected');
    const input=async text=>{await cdp('Input.insertText',{text});await rendered();};
    await input('-');await input(' ');await input('First item');await key('Enter','Enter',13);await input('Second item');await key('Enter','Enter',13);await key('Enter','Enter',13);
    await input('```js');await key('Enter','Enter',13);await input('console.log("<hello>");');await key('Enter','Enter',13);await input('```');await key('Enter','Enter',13);
    await persistWhere(beta,doc=>allNodes(doc).some(n=>n.type==='bulletList')&&allNodes(doc).some(n=>n.type==='codeBlock'&&treeText(n).includes('console.log')),'Markdown lists and fenced code');
    pass('Typed Markdown list continuation/empty-item exit and fenced code survive browser input and save');
    let toolbarPage=0,toolsOpen=false;
    async function toolbar(index){
        if(!toolsOpen){await key('t','KeyT',84,10);toolsOpen=true;}
        const page=Math.floor(index/25);if(page!==toolbarPage){await click(988,107);toolbarPage=page;}
        const local=index%25;await click(52+(local%13)*78,67+Math.floor(local/13)*40);
    }
    const image=png(),imagePath=join(profile,'fixture.png');await writeFile(imagePath,image);
    await cdp('Page.setInterceptFileChooserDialog',{enabled:true});
    async function chooseFile(path){
        await until(()=>evaluate('!!document.querySelector("input[data-editor-import]")'),'production import input');
        const root=(await cdp('DOM.getDocument')).root.nodeId;
        const nodeId=(await cdp('DOM.querySelector',{nodeId:root,selector:'input[data-editor-import]'})).nodeId;
        await cdp('DOM.setFileInputFiles',{nodeId,files:[path]});await rendered();
    }
    console.log('STAGE: image file picker');
    await toolbar(40);await chooseFile(imagePath);
    await persistWhere(beta,doc=>allNodes(doc).filter(n=>n.type==='image').length===1,'file-picker image insertion');
    let images=allNodes(await json(beta)).filter(n=>n.type==='image'),src=images[0].attrs.src;
    const asset='/notes/.notelab-writing/assets/'+src.slice('notelab-asset:'.length);
    assert.deepEqual(Buffer.from(await evaluate(`Array.from(FS.readFile(${JSON.stringify(asset)}))`)),image);
    // Image insertion leaves the caret at the atom's end, suitable for properties.
    await toolbar(41);await submit('Illustration <alt>');await toolbar(42);await submit('A local image');await toolbar(43);await submit('320');
    await persistWhere(beta,doc=>allNodes(doc).some(n=>n.type==='image'&&n.attrs.alt==='Illustration <alt>'&&n.attrs.caption==='A local image'&&n.attrs.width===320),'image properties persisted');
    await screenshot('browser-longform-image-validation.png');
    const paste=()=>evaluate(`(()=>{const bytes=new Uint8Array(${JSON.stringify(Array.from(image))});const transfer=new DataTransfer();transfer.items.add(new File([bytes],'pasted.png',{type:'image/png'}));document.getElementById('editor-input').dispatchEvent(new ClipboardEvent('paste',{bubbles:true,cancelable:true,clipboardData:transfer}));return true;})()`);
    await paste();await persistWhere(beta,doc=>allNodes(doc).filter(n=>n.type==='image').length===2,'DOM clipboard image paste');
    await evaluate(`(()=>{const transfer=new DataTransfer();transfer.items.add(new File([new Uint8Array(${JSON.stringify(Array.from(image))})],'dropped.png',{type:'image/png'}));Module.canvas.dispatchEvent(new DragEvent('drop',{bubbles:true,cancelable:true,dataTransfer:transfer}));return true;})()`);
    await persistWhere(beta,doc=>allNodes(doc).filter(n=>n.type==='image').length===3,'DOM file drop image insertion');
    assert.equal(await evaluate("FS.readdir('/notes/.notelab-writing/assets').filter(n=>n.endsWith('.png')).length"),1,'identical image imports deduplicate');
    pass('PNG picker, DOM image paste and file drop import actual binary resources; alt/caption/width persist and assets deduplicate');
    async function captureDownloads(){await evaluate(`(()=>{window.imageDownloads=[];const original=URL.createObjectURL;URL.createObjectURL=function(blob){if(blob instanceof Blob)window.imageDownloads.push(blob.arrayBuffer().then(b=>Array.from(new Uint8Array(b))));return original.call(this,blob);};})()`);}
    async function download(){return until(()=>evaluate('window.imageDownloads.length?window.imageDownloads.shift():null'),'downloaded production ZIP bytes');}
    console.log('STAGE: blog export and preview');
    await captureDownloads();await toolbar(39);const articleBytes=await download();const articleZip=unzip(articleBytes);
    await until(async()=>{try{return (await readFile(join(downloadDir,'001 Images.zip'))).equals(Buffer.from(articleBytes));}catch{return false;}},'actual ZIP file download completion');
    assert.ok(articleZip.has('article.html'));assert.ok(articleZip.get('article.html').toString().includes('&lt;alt&gt;'));
    const articleAsset=[...articleZip].find(([name])=>name.startsWith('assets/'));assert.deepEqual(articleAsset[1],image);assert.equal(articleZip.size,2);
    pass('Actual browser HTML export downloads a ZIP with complete relative image resources and escaped metadata');
    const beforePreview=await raw(beta);await toolbar(44);await screenshot('browser-longform-preview-desktop-validation.png');
    await click(831,163);await screenshot('browser-longform-preview-mobile-validation.png');assert.equal(await raw(beta),beforePreview);
    await click(1037,163);await key('m','KeyM',77,10);await click(200,290);await click(200,102);await click(270,35);
    await screenshot('browser-longform-history-diff-validation.png');assert.equal(await raw(beta),beforePreview);await key('Escape','Escape',27);
    pass('Desktop/mobile blog preview and history comparison are read-only and render without JavaScript errors');
    console.log('STAGE: workspace backup and restore');
    await key('m','KeyM',77,10);await click(650,110);await submit('longform,images');
    await until(async()=>(await catalog()).articles['001 Images.json']?.tags==='longform,images'&&await idle(),'article tags saved before workspace backup');
    const beforeCatalog=await raw(catalogPath);await click(650,470);const backupBytes=await download();const backupZip=unzip(backupBytes);
    assert.ok(backupZip.has('workspace/001 Images.json'));assert.equal(backupZip.get('workspace/.notelab-writing/catalog.json').toString(),beforeCatalog);assert.deepEqual(backupZip.get('workspace/.notelab-writing/assets/'+src.slice(14)),image);
    assert.ok([...backupZip.keys()].some(k=>k.startsWith('workspace/.notelab-writing/versions/')));
    const backupPath=join(profile,'workspace.zip');await writeFile(backupPath,Buffer.from(backupBytes));
    await click(200,530);await chooseFile(backupPath);
    await until(async()=>await exists('/notes/.notelab-recovery/active.txt')&&await idle(),'restored workspace selection durably committed');
    const relative=await raw('/notes/.notelab-recovery/active.txt'),restoredRoot='/notes/'+relative;
    assert.ok(relative.startsWith('.notelab-restored/'));const restoredNote=restoredRoot+'/001 Images.json';
    assert.equal(await raw(restoredNote),beforePreview);assert.equal(await raw(restoredRoot+'/.notelab-writing/catalog.json'),beforeCatalog);assert.equal(await raw(beta),beforePreview,'original workspace preserved');
    assert.deepEqual(Buffer.from(await evaluate(`Array.from(FS.readFile(${JSON.stringify(restoredRoot+'/.notelab-writing/assets/'+src.slice(14))}))`)),image);
    await reload();toolbarPage=0;toolsOpen=false;
    assert.equal(await raw('/notes/.notelab-recovery/active.txt'),relative);assert.equal(await raw(restoredNote),beforePreview);
    // Reopened app must edit the restored workspace, not merely retain copied files.
    await click(95,200);await until(()=>evaluate('document.title.includes("001 Images.json")'),'restored article reopened');
    await key('Home','Home',36,2);await input('Restored edit ');
    await persistWhere(restoredNote,doc=>treeText(doc).includes('Restored edit '),'editing reopened restored workspace');assert.equal(await raw(beta),beforePreview);
    await screenshot('browser-longform-restored-validation.png');
    pass('Whole-workspace backup includes pictures/history; restored copy preserves originals and remains active after IndexedDB reload');
    // Load the exact exported HTML and ZIP assets in Chromium, not a synthetic
    // equivalent. No remote requests: only local images and an unclicked link.
    const exportRoot=join(build,'longform-export');await mkdir(join(exportRoot,'assets'),{recursive:true});
    for(const [name,bytes] of articleZip){assert.ok(name==='article.html'||/^assets\/[a-f0-9]{64}\.(png|jpg|bmp)$/.test(name));await writeFile(join(exportRoot,name),bytes);}
    await cdp('Page.navigate',{url:origin+'/longform-export/article.html'});
    await until(()=>evaluate("document.readyState==='complete'&&document.querySelectorAll('img').length===3&&Array.from(document.images).every(i=>i.complete&&i.naturalWidth===240&&i.naturalHeight===120)"),'exported HTML real image loading');
    assert.equal(await evaluate("document.querySelector('a').getAttribute('href')"),'https://example.invalid/blog-reference');
    assert.ok((await evaluate("document.querySelector('pre code').textContent")).includes('console.log("<hello>");'));
    assert.ok(await evaluate("Array.from(document.images).some(i=>i.alt==='Illustration <alt>')"));
    await screenshot('browser-longform-export-desktop-validation.png');
    await cdp('Emulation.setDeviceMetricsOverride',{width:390,height:844,deviceScaleFactor:1,mobile:true});await rendered();
    assert.ok(await evaluate('document.documentElement.scrollWidth<=innerWidth+1&&Array.from(document.images).every(i=>i.getBoundingClientRect().right<=innerWidth+1)'));
    await screenshot('browser-longform-export-mobile-validation.png');
    pass('The actual exported HTML loads every bundled image and preserves links/code on desktop and 390px mobile widths');
    assert.deepEqual(exceptions,[]);assert.deepEqual(dialogs,[]);
    assert.deepEqual(await hashes(),proof.artifacts);proof.completed=new Date().toISOString();
    await writeFile(join(build,'browser-longform-proof.json'),JSON.stringify(proof,null,2));
    await Promise.all([rm(join(build,'browser-longform-failure.json'),{force:true}),rm(join(build,'browser-longform-failure.png'),{force:true})]);
    console.log('All real-browser longform workflow checks passed.');
}catch(error){
    if(evaluate)try{
        const state=await evaluate(`({title:document.title,focus:document.activeElement?.id,area:window.Module?.editorInputArea,
            unsaved:window.Module?.editorUnsaved,pending:window.Module?.editorPersistencePending,inputPending:window.Module?.editorInputPending,
            articles:typeof FS!=='undefined'?FS.readdir('/notes').filter(n=>n.endsWith('.json')).map(n=>[n,FS.readFile('/notes/'+n,{encoding:'utf8'})]):[],notes:typeof FS!=='undefined'?FS.readdir('/notes'):[],catalog:typeof FS!=='undefined'&&FS.analyzePath(${JSON.stringify(catalogPath)}).exists?FS.readFile(${JSON.stringify(catalogPath)},{encoding:'utf8'}):null})`);
        await writeFile(join(build,'browser-longform-failure.json'),JSON.stringify({error:String(error),proof,exceptions,dialogs,state},null,2));
        await writeFile(join(build,'browser-longform-failure.png'),Buffer.from((await cdp('Page.captureScreenshot')).data,'base64'));
    }catch{}
    console.error(stderr);throw error;
}finally{
    if(cdp&&ws?.readyState===WebSocket.OPEN&&!browserClosed)try{await Promise.race([cdp('Browser.close'),sleep(1000)]);}catch{}
    try{ws?.close();}catch{}
    for(const request of pending.values())request.reject(Error('test finished'));
    server.close();server.closeAllConnections();await Promise.race([browserFinished,sleep(3000)]);
    if(!browserClosed){chrome.kill();await Promise.race([browserFinished,sleep(3000)]);}
    if(!browserClosed){chrome.kill('SIGKILL');await Promise.race([browserFinished,sleep(3000)]);}
    assert.ok(resolve(profile).startsWith(resolve(tmpdir())+sep+'notelab-longform-browser-'));
    await rm(profile,{recursive:true,force:true,maxRetries:8,retryDelay:200});
}
