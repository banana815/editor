// Execute the production download bridge without a real browser or clipboard.
import assert from 'node:assert/strict';
import {readFileSync} from 'node:fs';
import vm from 'node:vm';
const source=readFileSync(new URL('../src/platform_export.c',import.meta.url),'utf8').replace(/\r\n/g,'\n');
const body=source.match(/EM_JS\(int, download_text, \([^\n]*\), \{\n([\s\S]*?)\n\}\);?/);
assert.ok(body);
let link,blob,timer,revoked=[],denied=false;
const context=vm.createContext({
    UTF8ToString:value=>value, HEAPU8:new Uint8Array(1024),
    Blob:class { constructor(parts,options) { this.parts=parts;this.type=options.type;blob=this; } },
    URL:{createObjectURL:()=> 'blob:fixture',revokeObjectURL:value=>revoked.push(value)},
    document:{body:{appendChild(){}},createElement(){
        link={style:{},click(){if(denied) throw Error('denied');this.clicked=true;},remove(){this.removed=true;}};return link;
    }},
    setTimeout(fn,delay){assert.equal(delay,60000);timer=fn;}
});
vm.runInContext(`function download(filename,data,size,mime) { ${body[1]} }`,context);
for(const [extension,mime,text] of [
    ['json','application/json;charset=utf-8','{"type":"doc"}'],
    ['md','text/markdown;charset=utf-8','# 标题\n\n**draft**'],
    ['html','text/html;charset=utf-8','<h1>标题</h1>'],
    ['zip','application/zip','PK\x00\x01\xff']
]) {
    revoked=[];timer=null;
    const bytes=new TextEncoder().encode(text); context.HEAPU8.set(bytes,20);
    assert.equal(context.download(`文章.${extension}`,20,bytes.length,mime),1);
    assert.equal(blob.type,mime);assert.deepEqual(Array.from(blob.parts[0]),Array.from(bytes));
    assert.equal(link.download,`文章.${extension}`);assert.ok(link.clicked&&link.removed);
    assert.deepEqual(revoked,[]);timer();assert.deepEqual(revoked,['blob:fixture']);
}
denied=true;revoked=[];
assert.equal(context.download('failed.html',20,12,'text/html;charset=utf-8'),0);
assert.ok(link.removed);assert.deepEqual(revoked,['blob:fixture']);
console.log('Export bridge: JSON/Markdown/HTML bytes, MIME, filenames and failed download cleanup passed');
