#include "document_publish.h"
#include "cJSON.h"
#include <ctype.h>
#include <stdio.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#define LIMIT ((size_t)16*1024*1024)
typedef struct { char *data; size_t size, capacity; bool failed; } PublishBuffer;
typedef struct { CePublishAtomHtml atom; void *context; CePublishImageUrl image; void *image_context; char *error; size_t capacity; } Publisher;
static bool publish_fail(Publisher *p,const char *message) {
    if(p->error && p->capacity) snprintf(p->error,p->capacity,"%s",message);
    return false;
}
static bool putn(PublishBuffer *b,const char *s,size_t n) {
    if(b->failed || n>LIMIT-b->size) { b->failed=true; return false; }
    size_t needed=b->size+n+1;
    if(needed>b->capacity) {
        size_t cap=b->capacity?b->capacity:128;
        while(cap<needed) cap=cap>LIMIT/2?LIMIT+1:cap*2;
        char *next=realloc(b->data,cap);
        if(!next) { b->failed=true; return false; }
        b->data=next; b->capacity=cap;
    }
    memcpy(b->data+b->size,s,n); b->size+=n; b->data[b->size]=0; return true;
}
static bool puts_b(PublishBuffer *b,const char *s) { return putn(b,s,strlen(s)); }
static const cJSON *item(const cJSON *n,const char *key) { return cJSON_GetObjectItemCaseSensitive(n,key); }
static const char *value(const cJSON *n,const char *key) {
    const cJSON *v=item(n,key); return cJSON_IsString(v)?v->valuestring:"";
}
static bool escape(PublishBuffer *b,const char *s,bool markdown) {
    for(;*s;s++) {
        if(*s=='&') puts_b(b,"&amp;");
        else if(*s=='<') puts_b(b,"&lt;");
        else if(*s=='>') puts_b(b,"&gt;");
        else if(*s=='\"') puts_b(b,"&quot;");
        else if(*s=='\n') puts_b(b,markdown?"  \n":"<br>");
        else {
            if(markdown && strchr("\\`*_{}[]()#+-.!|~=",*s)) puts_b(b,"\\");
            putn(b,s,1);
        }
    }
    return !b->failed;
}
static bool prefix_ci(const char *s,const char *prefix) {
    while(*prefix) if(!*s || tolower((unsigned char)*s++)!=*prefix++) return false;
    return true;
}
static bool safe_link(const char *s) {
    if(!*s) return false;
    for(const unsigned char *v=(const unsigned char *)s;*v;v++)
        if(*v<0x20 || *v==0x7f || *v=='\\') return false;
    if(prefix_ci(s,"https://") || prefix_ci(s,"http://") || prefix_ci(s,"mailto:")) return true;
    /* Relative paths/fragments are safe only when they cannot conceal a scheme. */
    return !strchr(s,':') && *s!=' ';
}
static bool safe_color(const char *s) {
    size_t n=strlen(s);
    if(*s=='#') {
        if(n!=4 && n!=7 && n!=9) return false;
        for(size_t i=1;i<n;i++) if(!isxdigit((unsigned char)s[i])) return false;
        return true;
    }
    if(!n || n>24) return false;
    for(;*s;s++) if(!isalpha((unsigned char)*s)) return false;
    return true;
}
static bool html_node(Publisher *,PublishBuffer *,const cJSON *,unsigned);
static bool valid_block_child(Publisher *p,const cJSON *parent,const cJSON *child) {
    const char *type=value(parent,"type"),*ct=value(child,"type");
    if(!strcmp(type,"bulletList") || !strcmp(type,"orderedList"))
        return !strcmp(ct,"listItem") || publish_fail(p,"列表结构无效");
    if(!strcmp(ct,"listItem") || !strcmp(ct,"doc") || !strcmp(ct,"text") || !strcmp(ct,"hardBreak"))
        return publish_fail(p,"文档块结构无效，请使用 JSON 保留原文");
    return true;
}
static bool inline_node(Publisher *p,PublishBuffer *b,const cJSON *node,bool markdown,unsigned depth) {
    const char *type=value(node,"type");
    if(depth>64) return publish_fail(p,"文档层级过深，无法发布");
    if(!strcmp(type,"hardBreak")) return puts_b(b,markdown?"  \n":"<br>");
    if(strcmp(type,"text") || !cJSON_IsString(item(node,"text")))
        return publish_fail(p,"包含不支持的行内内容，请使用 JSON 保留原文");
    const cJSON *marks=item(node,"marks"),*mark;
    if(marks && !cJSON_IsArray(marks)) return publish_fail(p,"无效的文本格式");
    bool html_marks=false;
    cJSON_ArrayForEach(mark,marks) {
        const char *mt=value(mark,"type");
        if(strcmp(mt,"bold") && strcmp(mt,"italic")) html_marks=true;
    }
    const char *text=value(node,"text"); size_t len=strlen(text);
    bool md_marks=markdown && !html_marks && len && !isspace((unsigned char)text[0]) && !isspace((unsigned char)text[len-1]);
    cJSON_ArrayForEach(mark,marks) {
        const char *mt=value(mark,"type"); const cJSON *a=item(mark,"attrs");
        if(!strcmp(mt,"bold")) puts_b(b,md_marks?"**":"<strong>");
        else if(!strcmp(mt,"italic")) puts_b(b,md_marks?"*":"<em>");
        else if(!strcmp(mt,"underline")) puts_b(b,"<u>");
        else if(!strcmp(mt,"highlight") || !strcmp(mt,"textStyle")) {
            const char *color=value(a,"color");
            if(!strcmp(mt,"highlight") && !*color) puts_b(b,"<mark>");
            else {
                if(!safe_color(color)) return publish_fail(p,"包含无法安全发布的颜色格式");
                puts_b(b,!strcmp(mt,"highlight")?"<mark style=\"background-color:":"<span style=\"color:");
                escape(b,color,false); puts_b(b,"\">");
            }
        } else if(!strcmp(mt,"link")) {
            const char *href=value(a,"href");
            if(!safe_link(href)) return publish_fail(p,"包含不支持或不安全的链接，请先修改链接");
            puts_b(b,"<a href=\""); escape(b,href,false); puts_b(b,"\">");
        } else return publish_fail(p,"包含不支持的文本格式，请使用 JSON 保留原文");
    }
    escape(b,text,markdown);
    for(int i=cJSON_GetArraySize(marks)-1;i>=0;i--) {
        const char *mt=value(cJSON_GetArrayItem(marks,i),"type");
        puts_b(b,!strcmp(mt,"bold")?(md_marks?"**":"</strong>"):
            !strcmp(mt,"italic")?(md_marks?"*":"</em>"):
            !strcmp(mt,"underline")?"</u>":!strcmp(mt,"highlight")?"</mark>":
            !strcmp(mt,"link")?"</a>":"</span>");
    }
    return !b->failed;
}
static bool inline_children(Publisher *p,PublishBuffer *b,const cJSON *node,bool md,unsigned depth) {
    const cJSON *content=item(node,"content"),*child;
    if(content && !cJSON_IsArray(content)) return publish_fail(p,"无效的段落内容");
    cJSON_ArrayForEach(child,content) if(!inline_node(p,b,child,md,depth+1)) return false;
    return !b->failed;
}
static bool atom(Publisher *p,PublishBuffer *b,const cJSON *node) {
    if(!p->atom) return publish_fail(p,"引用尚未重新计算，无法发布；请检查文献来源");
    char *html=p->atom(p->context,node,p->error,p->capacity);
    if(!html) return false;
    bool ok=puts_b(b,html); free(html); return ok;
}
static bool image_node(Publisher *p,PublishBuffer *b,const cJSON *node) {
    const cJSON *attrs=item(node,"attrs"),*width=item(attrs,"width");
    const char *alt=value(attrs,"alt"),*caption=value(attrs,"caption");
    if(!p->image) return publish_fail(p,"图片需要本地资源解析器，未导出");
    if((item(attrs,"alt") && !cJSON_IsString(item(attrs,"alt"))) ||
       (item(attrs,"caption") && !cJSON_IsString(item(attrs,"caption"))) ||
       (width && (!cJSON_IsNumber(width) || !isfinite(width->valuedouble) || width->valuedouble<0 ||
       width->valuedouble>2400 || width->valuedouble!=width->valueint))) return publish_fail(p,"图片属性无效");
    char *url=p->image(p->image_context,node,p->error,p->capacity); if(!url) return false;
    /* Resolver owns trusted generated data/relative URLs. Arbitrary imported
       image URLs never reach this point without explicit resolution. */
    bool safe=!strncmp(url,"data:image/png;base64,",22) || !strncmp(url,"data:image/jpeg;base64,",23) ||
        !strncmp(url,"data:image/bmp;base64,",22) || (!strncmp(url,"assets/",7) && safe_link(url));
    if(!safe) { free(url); return publish_fail(p,"图片发布地址无效"); }
    puts_b(b,"<figure><img src=\""); escape(b,url,false); free(url); puts_b(b,"\" alt=\""); escape(b,alt,false);
    puts_b(b,"\" style=\"max-width:100%;height:auto\"");
    if(width && width->valueint) { char n[40]; snprintf(n,sizeof n," width=\"%d\"",width->valueint); puts_b(b,n); }
    puts_b(b,">");
    if(*caption) { puts_b(b,"<figcaption>"); escape(b,caption,false); puts_b(b,"</figcaption>"); }
    return puts_b(b,"</figure>\n");
}
static bool code_text(Publisher *p,PublishBuffer *b,const cJSON *node,bool html) {
    const cJSON *children=item(node,"content"),*child;
    if(children && !cJSON_IsArray(children)) return publish_fail(p,"代码块内容无效");
    cJSON_ArrayForEach(child,children) {
        const char *type=value(child,"type"),*text=value(child,"text");
        if(!strcmp(type,"hardBreak")) { puts_b(b,"\n"); continue; }
        if(strcmp(type,"text") || !cJSON_IsString(item(child,"text")) || cJSON_GetArraySize(item(child,"marks"))) return publish_fail(p,"代码块格式无效");
        if(!html) puts_b(b,text);
        else for(;*text;text++) { if(*text=='\n') puts_b(b,"\n"); else { char one[]={*text,0}; escape(b,one,false); } }
    }
    return !b->failed;
}
static bool html_node(Publisher *p,PublishBuffer *b,const cJSON *node,unsigned depth) {
    if(depth>64) return publish_fail(p,"文档层级过深，无法发布");
    const char *type=value(node,"type"),*tag=NULL; char heading[8];
    const cJSON *attrs=item(node,"attrs"),*content=item(node,"content"),*child;
    if(!strcmp(type,"zoteroCitationBlock") || !strcmp(type,"zoteroEndnoteSection")) return atom(p,b,node);
    if(!strcmp(type,"image")) return image_node(p,b,node);
    if(!strcmp(type,"horizontalRule")) return puts_b(b,"<hr>\n");
    if(!strcmp(type,"codeBlock")) return puts_b(b,"<pre><code>") && code_text(p,b,node,true) && puts_b(b,"</code></pre>\n");
    if(!strcmp(type,"blockquote")) tag="blockquote";
    else if(!strcmp(type,"paragraph")) tag="p";
    else if(!strcmp(type,"heading")) {
        const cJSON *level=item(attrs,"level");
        if(!cJSON_IsNumber(level) || level->valuedouble<1 || level->valuedouble>6 || level->valueint!=level->valuedouble)
            return publish_fail(p,"标题级别无效");
        snprintf(heading,sizeof heading,"h%d",level->valueint); tag=heading;
    } else if(!strcmp(type,"bulletList")) tag="ul";
    else if(!strcmp(type,"orderedList")) tag="ol";
    else if(!strcmp(type,"listItem")) tag="li";
    else if(strcmp(type,"doc") || depth) return publish_fail(p,"包含不支持的文档结构，请使用 JSON 保留原文");
    if(tag) {
        puts_b(b,"<"); puts_b(b,tag);
        const char *align=value(attrs,"textAlign");
        if(*align) {
            if(strcmp(align,"left") && strcmp(align,"right") && strcmp(align,"center") && strcmp(align,"justify"))
                return publish_fail(p,"不支持的对齐方式");
            puts_b(b," style=\"text-align:"); puts_b(b,align); puts_b(b,"\"");
        }
        if(!strcmp(type,"orderedList") && item(attrs,"start")) {
            const cJSON *start=item(attrs,"start"); char n[40];
            if(!cJSON_IsNumber(start) || start->valuedouble<0 || start->valuedouble>999999999 || start->valueint!=start->valuedouble)
                return publish_fail(p,"列表起始编号无效");
            snprintf(n,sizeof n," start=\"%d\"",start->valueint); puts_b(b,n);
        }
        puts_b(b,">");
    }
    if(!strcmp(type,"paragraph") || !strcmp(type,"heading")) {
        if(!inline_children(p,b,node,false,depth)) return false;
    } else {
        if(content && !cJSON_IsArray(content)) return publish_fail(p,"无效的文档内容");
        cJSON_ArrayForEach(child,content) {
            if(!valid_block_child(p,node,child)) return false;
            if(!html_node(p,b,child,depth+1)) return false;
        }
    }
    if(tag) { puts_b(b,"</"); puts_b(b,tag); puts_b(b,">\n"); }
    return !b->failed;
}
static bool markdown_node(Publisher *p,PublishBuffer *b,const cJSON *node,unsigned depth) {
    if(depth>64) return publish_fail(p,"文档层级过深，无法发布");
    const char *type=value(node,"type"); const cJSON *attrs=item(node,"attrs"),*content=item(node,"content"),*child;
    if(!strcmp(type,"image")) return image_node(p,b,node) && puts_b(b,"\n");
    if(!strcmp(type,"horizontalRule")) return puts_b(b,"---\n\n");
    if(!strcmp(type,"codeBlock")) {
        const char *lang=value(attrs,"language"); size_t language_length=strlen(lang);
        if(language_length>64) return publish_fail(p,"代码语言标识过长");
        for(const char *c=lang;*c;c++) if(!isalnum((unsigned char)*c) && !strchr("_+-.",*c)) return publish_fail(p,"代码语言标识无效");
        PublishBuffer raw={0}; if(!code_text(p,&raw,node,false)) { free(raw.data); return false; }
        size_t longest=0,run=0; for(size_t i=0;i<raw.size;i++) { run=raw.data[i]=='`'?run+1:0; if(run>longest) longest=run; }
        size_t fence=longest<3?3:longest+1;
        for(size_t i=0;i<fence;i++) puts_b(b,"`"); puts_b(b,lang); puts_b(b,"\n");
        if(raw.size) putn(b,raw.data,raw.size);
        if(!raw.size || raw.data[raw.size-1]!='\n') puts_b(b,"\n");
        free(raw.data); for(size_t i=0;i<fence;i++) puts_b(b,"`"); return puts_b(b,"\n\n");
    }
    if(!strcmp(type,"blockquote")) {
        PublishBuffer quote={0}; bool ok=true;
        cJSON_ArrayForEach(child,content) if(!valid_block_child(p,node,child) || !markdown_node(p,&quote,child,depth+1)) { ok=false; break; }
        if(ok) { puts_b(b,"> "); for(size_t i=0;i<quote.size;i++) { putn(b,quote.data+i,1); if(quote.data[i]=='\n' && i+1<quote.size) puts_b(b,"> "); } puts_b(b,"\n"); }
        free(quote.data); return ok && !b->failed;
    }
    if(!strcmp(type,"zoteroCitationBlock") || !strcmp(type,"zoteroEndnoteSection") || *value(attrs,"textAlign"))
        return html_node(p,b,node,depth) && puts_b(b,"\n");
    if(!strcmp(type,"paragraph") || !strcmp(type,"heading")) {
        if(!strcmp(type,"heading")) {
            const cJSON *level=item(attrs,"level");
            if(!cJSON_IsNumber(level) || level->valuedouble<1 || level->valuedouble>6 || level->valueint!=level->valuedouble)
                return publish_fail(p,"标题级别无效");
            for(int i=0;i<level->valueint;i++) puts_b(b,"#");
            puts_b(b," ");
        }
        return inline_children(p,b,node,true,depth) && puts_b(b,"\n\n");
    }
    if(content && !cJSON_IsArray(content)) return publish_fail(p,"无效的文档内容");
    if(!strcmp(type,"bulletList") || !strcmp(type,"orderedList")) {
        bool ordered=!strcmp(type,"orderedList"); int number=1;
        const cJSON *start=item(attrs,"start");
        if(ordered && start) {
            if(!cJSON_IsNumber(start) || start->valuedouble<0 || start->valuedouble>999999999 || start->valueint!=start->valuedouble)
                return publish_fail(p,"列表起始编号无效");
            number=start->valueint;
        }
        cJSON_ArrayForEach(child,content) {
            if(strcmp(value(child,"type"),"listItem")) return publish_fail(p,"列表结构无效");
            PublishBuffer nested={0};
            bool ok=markdown_node(p,&nested,child,depth+1);
            if(!ok) { free(nested.data); return false; }
            char marker[32]; snprintf(marker,sizeof marker,ordered?"%d. ":"- ",number);
            size_t pad=strlen(marker),end=nested.size;
            while(end && nested.data[end-1]=='\n') --end;
            puts_b(b,marker);
            for(size_t i=0;i<end;i++) {
                putn(b,nested.data+i,1);
                if(nested.data[i]=='\n' && i+1<end) for(size_t j=0;j<pad;j++) puts_b(b," ");
            }
            puts_b(b,"\n\n"); free(nested.data);
            if(ordered && number==999999999 && child->next) return publish_fail(p,"列表编号过大");
            ++number;
        }
        return !b->failed;
    }
    if(strcmp(type,"listItem") && (strcmp(type,"doc") || depth))
        return publish_fail(p,"包含不支持的文档结构，请使用 JSON 保留原文");
    cJSON_ArrayForEach(child,content)
        if(!valid_block_child(p,node,child) || !markdown_node(p,b,child,depth+1)) return false;
    return !b->failed;
}
char *ce_document_publish(const CeDocument *document,CePublishFormat format,CePublishAtomHtml resolver,
                          void *context,char *error,size_t capacity) {
    return ce_document_publish_images(document,format,resolver,context,NULL,NULL,error,capacity);
}
char *ce_document_publish_images(const CeDocument *document,CePublishFormat format,CePublishAtomHtml resolver,
                                 void *context,CePublishImageUrl images,void *image_context,char *error,size_t capacity) {
    Publisher p={resolver,context,images,image_context,error,capacity}; PublishBuffer out={0};
    if(error && capacity) *error=0;
    if(!document) { publish_fail(&p,"没有可发布的文档"); return NULL; }
    char *json=ce_document_export_json(document);
    if(!json || strlen(json)>LIMIT) { free(json); publish_fail(&p,"文档过大或内存不足，未发布"); return NULL; }
    cJSON *root=cJSON_Parse(json); free(json);
    bool ok=root && !strcmp(value(root,"type"),"doc") &&
        (format==CE_PUBLISH_HTML?html_node(&p,&out,root,0):
         format==CE_PUBLISH_MARKDOWN && markdown_node(&p,&out,root,0));
    cJSON_Delete(root);
    if(ok) ok=puts_b(&out,"");
    if(!ok) {
        free(out.data);
        if(!error || !capacity || !*error) publish_fail(&p,"内存不足、输出过大或格式无效，未发布");
        return NULL;
    }
    return out.data;
}

char *ce_document_publish_html_page(const char *body) {
    if(!body) return NULL;
    PublishBuffer b={0};
    const char *head="<!doctype html>\n<html><head><meta charset=\"utf-8\"><meta name=\"viewport\" content=\"width=device-width,initial-scale=1\"><style>"
        "*{box-sizing:border-box}html{color:#1e2430;background:#fff}body{max-width:768px;margin:0 auto;padding:24px;font:20px/1.5 system-ui,sans-serif;overflow-wrap:anywhere}"
        "h1,h2,h3,h4,h5,h6{line-height:1.25;margin:1.2em 0 .55em}p,ul,ol,pre,blockquote,figure{margin:0 0 1em}"
        "blockquote{border-left:3px solid #9bb4a7;padding:.4em 1em;background:#f3f6f4}blockquote p:last-child{margin-bottom:0}"
        "pre{background:#f0f3f7;padding:16px;border-radius:6px;white-space:pre-wrap;overflow-wrap:anywhere}code{font-family:ui-monospace,monospace}"
        "img{max-width:100%;height:auto}figure{margin-left:0;margin-right:0}figcaption{font-size:.8em;color:#59666d;margin-top:.4em}"
        "a{color:#1f5fc6;text-decoration:underline}hr{border:0;border-top:1px solid #aab7b1;margin:1.5em 0}"
        "@media(max-width:440px){body{padding:24px;font-size:20px}}"
        "</style></head><body>\n";
    if(!puts_b(&b,head) || !puts_b(&b,body) || !puts_b(&b,"</body></html>\n")) { free(b.data);return NULL; }
    return b.data;
}
