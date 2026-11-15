#include "bibliography_archive.h"
#include "csl_json.h"
#include "csl_unicode.h"
#include <inttypes.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef union Allocation { long double alignment; void *pointer; size_t bytes; unsigned char pad[32]; } Allocation;
struct CeBibliographyArchive {
    CeCslOutputAllocator allocator;
    size_t workspace, payload, run_count, row_count, count;
    unsigned diagnostics;
    CeBibliographyArchiveEntry *entries;
};
static bool fail(char *error,size_t cap,const char *message) {
    if(error && cap) snprintf(error,cap,"%s",message);
    return false;
}
static void *normal_alloc(void *context,size_t size) { (void)context; return malloc(size); }
static void normal_free(void *context,void *memory) { (void)context; free(memory); }
static void *allocate(void *context,size_t size) {
    CeBibliographyArchive *a=context;
    if(size>CE_BIBLIOGRAPHY_ARCHIVE_MAX_WORKSPACE-sizeof(Allocation) ||
       a->workspace>CE_BIBLIOGRAPHY_ARCHIVE_MAX_WORKSPACE-sizeof(Allocation)-size) return NULL;
    Allocation *p=a->allocator.allocate(a->allocator.context,sizeof *p+size);
    if(!p) return NULL;
    p->bytes=sizeof *p+size; a->workspace+=p->bytes; return p+1;
}
static void release(void *context,void *memory) {
    if(!memory) return;
    CeBibliographyArchive *a=context; Allocation *p=(Allocation *)memory-1;
    a->workspace-=p->bytes; a->allocator.deallocate(a->allocator.context,p);
}
static const cJSON *field(const cJSON *node,const char *name) { return cJSON_GetObjectItemCaseSensitive(node,name); }
static const char *string(const cJSON *node) { return cJSON_IsString(node)?node->valuestring:NULL; }
static bool bounded(const char *text,size_t limit,size_t *size) {
    *size=0;
    if(!text) return false;
    while(*size<=limit && text[*size]) ++*size;
    return *size<=limit;
}
static char *copy(CeBibliographyArchive *a,const char *text,size_t size) {
    char *p=allocate(a,size+1);
    if(p) { memcpy(p,text,size); p[size]=0; }
    return p;
}
bool ce_bibliography_archive_eligible(const cJSON *atom) {
    const char *type=string(field(atom,"type")); const cJSON *attrs=field(atom,"attrs");
    return type && !strcmp(type,"zoteroEndnoteSection") && cJSON_IsObject(attrs) &&
           !field(attrs,"bibliographyMode") && cJSON_IsString(field(attrs,"entriesJson"));
}
static CeCslOutput *output(CeBibliographyArchive *a,char *error,size_t cap) {
    CeCslOutputAllocator allocator={a,allocate,release};
    return ce_csl_output_new(&allocator,error,cap);
}
static void clear_entry(CeBibliographyArchive *a,CeBibliographyArchiveEntry *entry,bool metadata) {
    for(size_t i=0;i<entry->row_count;i++) {
        ce_csl_output_free((CeCslOutput *)entry->rows[i].left);
        ce_csl_output_free((CeCslOutput *)entry->rows[i].body);
    }
    release(a,(void *)entry->rows); entry->rows=NULL; entry->row_count=0;
    if(metadata) {
        ce_csl_output_free((CeCslOutput *)entry->label);
        release(a,(void *)entry->citation_id); release(a,(void *)entry->key);
        release(a,(void *)entry->item_uri); release(a,(void *)entry->pdf_uri);
    }
}
void ce_bibliography_archive_free(CeBibliographyArchive *a) {
    if(!a) return;
    for(size_t i=0;i<a->count;i++) clear_entry(a,&a->entries[i],true);
    release(a,a->entries); a->allocator.deallocate(a->allocator.context,a);
}
size_t ce_bibliography_archive_count(const CeBibliographyArchive *a) { return a?a->count:0; }
unsigned ce_bibliography_archive_diagnostics(const CeBibliographyArchive *a) { return a?a->diagnostics:0; }
const CeBibliographyArchiveEntry *ce_bibliography_archive_entry(const CeBibliographyArchive *a,size_t index) {
    return a && index<a->count?&a->entries[index]:NULL;
}
static bool add_row(CeBibliographyArchive *a,CeBibliographyArchiveEntry *entry,size_t *capacity,
                    CeCslBibliographyRowKind kind,CeCslOutput **left,CeCslOutput **body) {
    if(entry->row_count>=CE_BIBLIOGRAPHY_ARCHIVE_MAX_ROWS-a->row_count) return false;
    if(entry->row_count==*capacity) {
        size_t next=*capacity?*capacity*2:4;
        if(next>CE_BIBLIOGRAPHY_ARCHIVE_MAX_ROWS) next=CE_BIBLIOGRAPHY_ARCHIVE_MAX_ROWS;
        CeBibliographyArchiveRow *rows=allocate(a,next*sizeof *rows);
        if(!rows) return false;
        if(entry->row_count) memcpy(rows,entry->rows,entry->row_count*sizeof *rows);
        release(a,(void *)entry->rows); entry->rows=rows; *capacity=next;
    }
    CeBibliographyArchiveRow *row=(CeBibliographyArchiveRow *)&entry->rows[entry->row_count++];
    *row=(CeBibliographyArchiveRow){kind,left?*left:NULL,*body};
    if(left) *left=NULL;
    *body=NULL; return true;
}
static bool space(unsigned char c) { return c==' ' || c=='\t' || c=='\r' || c=='\n' || c=='\f'; }
static bool white(const char *text,size_t size) {
    for(size_t i=0;i<size;i++) if(!space((unsigned char)text[i])) return false;
    return true;
}
static unsigned lower(unsigned c) { return c>='A' && c<='Z'?c+('a'-'A'):c; }
static bool same(const char *text,size_t size,const char *name) {
    if(strlen(name)!=size) return false;
    for(size_t i=0;i<size;i++) if(lower((unsigned char)text[i])!=(unsigned char)name[i]) return false;
    return true;
}
/* Return 0 for unsupported entities, never recursively decode produced '&'. */
static size_t entity(const char *text,size_t size,char out[4],size_t *used) {
    size_t end=1;
    while(end<size && end<=16 && text[end]!=';') ++end;
    if(end>=size || end>16 || text[end]!=';') return 0;
    uint32_t cp=0;
    if(text[1]=='#') {
        size_t i=2; unsigned base=10;
        if(i<end && (text[i]=='x' || text[i]=='X')) { base=16; ++i; }
        if(i==end) return 0;
        for(;i<end;i++) {
            unsigned c=lower((unsigned char)text[i]);
            unsigned digit=c>='0' && c<='9'?c-'0':c>='a' && c<='f'?c-'a'+10:99;
            if(digit>=base || cp>(0x10ffff-digit)/base) return 0;
            cp=cp*base+digit;
        }
    } else {
        static const struct { const char *name; uint32_t cp; } named[]={
            {"amp",38},{"lt",60},{"gt",62},{"quot",34},{"apos",39},{"nbsp",160},
            {"ndash",0x2013},{"mdash",0x2014},{"lsquo",0x2018},{"rsquo",0x2019},
            {"ldquo",0x201c},{"rdquo",0x201d},{"hellip",0x2026},{"copy",169},{"reg",174}};
        for(size_t i=0;i<sizeof named/sizeof *named;i++)
            if(strlen(named[i].name)==end-1 && !memcmp(text+1,named[i].name,end-1)) { cp=named[i].cp; break; }
    }
    if(!cp || cp==127 || (cp<32 && cp!=9 && cp!=10 && cp!=13) || (cp>=0xd800 && cp<=0xdfff)) return 0;
    *used=end+1; return ce_csl_unicode_encode(cp,out);
}
typedef enum Parse { PARSE_OK, PARSE_FALLBACK, PARSE_ERROR } Parse;
static Parse decode_text(CeBibliographyArchive *a,const char *text,size_t size,char **out) {
    char *p=allocate(a,size+1);
    if(!p) return PARSE_ERROR;
    size_t used=0;
    for(size_t i=0;i<size;) {
        if(text[i]!='&') { p[used++]=text[i++]; continue; }
        size_t consumed=0; char bytes[4]; size_t n=entity(text+i,size-i,bytes,&consumed);
        if(!n) { release(a,p); return PARSE_FALLBACK; }
        memcpy(p+used,bytes,n); used+=n; i+=consumed;
    }
    p[used]=0; *out=p; return PARSE_OK;
}
typedef enum Tag { TAG_B,TAG_STRONG,TAG_I,TAG_EM,TAG_U,TAG_SUP,TAG_SUB,TAG_A,TAG_SPAN,TAG_DIV,TAG_BR } Tag;
typedef enum Display { DISPLAY_NONE,DISPLAY_WRAP,DISPLAY_BLOCK,DISPLAY_INDENT,DISPLAY_LEFT,DISPLAY_RIGHT } Display;
typedef struct Frame { Tag tag; unsigned format; Display display; char *href; } Frame;
typedef struct Token { Tag tag; bool close,self; const char *href,*style,*class_name; size_t href_size,style_size,class_size; } Token;
static bool token(const char *text,size_t size,size_t *used,Token *out) {
    size_t i=1; Token t={0};
    if(i<size && text[i]=='/') { t.close=true; ++i; }
    size_t begin=i;
    while(i<size && ((lower((unsigned char)text[i])>='a' && lower((unsigned char)text[i])<='z') || text[i]=='-')) ++i;
    static const char *names[]={"b","strong","i","em","u","sup","sub","a","span","div","br"};
    size_t name;
    for(name=0;name<sizeof names/sizeof *names;name++) if(same(text+begin,i-begin,names[name])) break;
    if(name==sizeof names/sizeof *names) return false;
    t.tag=(Tag)name;
    while(i<size) {
        bool separated=false;
        while(i<size && space((unsigned char)text[i])) { ++i; separated=true; }
        if(i<size && text[i]=='>') { *used=i+1; *out=t; return true; }
        if(!t.close && i+1<size && text[i]=='/' && text[i+1]=='>') { t.self=true; *used=i+2; *out=t; return true; }
        if(t.close || !separated) return false;
        begin=i;
        while(i<size && ((lower((unsigned char)text[i])>='a' && lower((unsigned char)text[i])<='z') || text[i]=='-')) ++i;
        size_t length=i-begin;
        while(i<size && space((unsigned char)text[i])) ++i;
        if(!length || i>=size || text[i++]!='=') return false;
        while(i<size && space((unsigned char)text[i])) ++i;
        if(i>=size || (text[i]!='\'' && text[i]!='"')) return false;
        char quote=text[i++]; size_t value=i;
        while(i<size && text[i]!=quote) ++i;
        if(i==size) return false;
        const char **target=NULL; size_t *target_size=NULL;
        if(t.tag==TAG_A && same(text+begin,length,"href")) { target=&t.href; target_size=&t.href_size; }
        if(t.tag==TAG_SPAN && same(text+begin,length,"style")) { target=&t.style; target_size=&t.style_size; }
        if(t.tag==TAG_DIV && same(text+begin,length,"class")) { target=&t.class_name; target_size=&t.class_size; }
        if(!target || *target) return false;
        *target=text+value; *target_size=i-value; ++i;
    }
    return false;
}
static bool css(const char *text,size_t size,unsigned *format) {
    size_t i=0; unsigned seen=0;
    while(i<size) {
        while(i<size && space((unsigned char)text[i])) ++i;
        if(i==size) break;
        size_t begin=i; while(i<size && text[i]!=':' && text[i]!=';') ++i;
        if(i==size || text[i]!=':') return false;
        size_t end=i++; while(end>begin && space((unsigned char)text[end-1])) --end;
        while(i<size && space((unsigned char)text[i])) ++i;
        size_t value=i; while(i<size && text[i]!=';') ++i;
        size_t last=i; while(last>value && space((unsigned char)text[last-1])) --last;
        unsigned bit=0;
        if(same(text+begin,end-begin,"font-style")) {
            bit=1;
            if(same(text+value,last-value,"italic")) *format|=CE_CSL_ITALIC;
            else if(same(text+value,last-value,"normal")) *format&=~CE_CSL_ITALIC;
            else return false;
        } else if(same(text+begin,end-begin,"font-weight")) {
            bit=2;
            if(same(text+value,last-value,"bold")) *format|=CE_CSL_BOLD;
            else if(same(text+value,last-value,"normal")) *format&=~CE_CSL_BOLD;
            else return false;
        } else if(same(text+begin,end-begin,"text-decoration")) {
            bit=4;
            if(same(text+value,last-value,"underline")) *format|=CE_CSL_UNDERLINE;
            else if(same(text+value,last-value,"none")) *format&=~CE_CSL_UNDERLINE;
            else return false;
        } else if(same(text+begin,end-begin,"vertical-align")) {
            bit=8; *format&=~(CE_CSL_SUPERSCRIPT|CE_CSL_SUBSCRIPT);
            if(same(text+value,last-value,"sup" ) || same(text+value,last-value,"super")) *format|=CE_CSL_SUPERSCRIPT;
            else if(same(text+value,last-value,"sub")) *format|=CE_CSL_SUBSCRIPT;
            else if(!same(text+value,last-value,"baseline")) return false;
        } else return false;
        if(seen&bit) return false;
        seen|=bit; if(i<size) ++i;
    }
    return true;
}
static Display display(const Token *t) {
    if(t->tag!=TAG_DIV) return DISPLAY_NONE;
    if(!t->class_name) return DISPLAY_BLOCK;
    if(same(t->class_name,t->class_size,"csl-entry") || same(t->class_name,t->class_size,"csl-bib-body")) return DISPLAY_WRAP;
    if(same(t->class_name,t->class_size,"csl-block")) return DISPLAY_BLOCK;
    if(same(t->class_name,t->class_size,"csl-indent")) return DISPLAY_INDENT;
    if(same(t->class_name,t->class_size,"csl-left-margin")) return DISPLAY_LEFT;
    if(same(t->class_name,t->class_size,"csl-right-inline")) return DISPLAY_RIGHT;
    return DISPLAY_NONE;
}
static Parse parse_html(CeBibliographyArchive *a,CeBibliographyArchiveEntry *entry,const char *html,size_t size,
                        char *error,size_t cap) {
    Frame stack[64]; size_t depth=0,capacity=0,at=0; Display active=DISPLAY_NONE;
    CeCslOutput *current=NULL,*left=NULL; Parse result=PARSE_OK;
    while(at<size) {
        if(html[at]!='<') {
            size_t begin=at; while(at<size && html[at]!='<') ++at;
            if(active==DISPLAY_NONE && white(html+begin,at-begin) && (!current || left)) continue;
            if(left && active!=DISPLAY_RIGHT) { result=PARSE_FALLBACK; break; }
            char *decoded=NULL; result=decode_text(a,html+begin,at-begin,&decoded);
            if(result!=PARSE_OK) break;
            if(!current) current=output(a,error,cap);
            const char *href=NULL;
            for(size_t i=depth;i>0;i--) if(stack[i-1].href) { href=stack[i-1].href; break; }
            bool ok=current && ce_csl_output_append(current,decoded,strlen(decoded),depth?stack[depth-1].format:0,href,error,cap);
            release(a,decoded);
            if(!ok) { result=PARSE_ERROR; break; }
            continue;
        }
        Token t; size_t used=0;
        if(!token(html+at,size-at,&used,&t)) { result=PARSE_FALLBACK; break; }
        at+=used;
        if(t.close) {
            if(!depth || stack[depth-1].tag!=t.tag) { result=PARSE_FALLBACK; break; }
            Frame *f=&stack[depth-1];
            if(f->display>=DISPLAY_BLOCK) {
                if(!current) current=output(a,error,cap);
                if(!current) { result=PARSE_ERROR; break; }
                if(f->display==DISPLAY_LEFT) { left=current; current=NULL; }
                else {
                    CeCslBibliographyRowKind kind=f->display==DISPLAY_RIGHT?CE_CSL_BIBLIOGRAPHY_TWO_COLUMNS:
                        f->display==DISPLAY_INDENT?CE_CSL_BIBLIOGRAPHY_INDENT:CE_CSL_BIBLIOGRAPHY_BLOCK;
                    if(!add_row(a,entry,&capacity,kind,f->display==DISPLAY_RIGHT?&left:NULL,&current)) { result=PARSE_ERROR; break; }
                }
                active=DISPLAY_NONE;
            }
            release(a,f->href); --depth; continue;
        }
        if(t.tag==TAG_BR) {
            if(left && active!=DISPLAY_RIGHT) { result=PARSE_FALLBACK; break; }
            if(!current) current=output(a,error,cap);
            if(!current || !ce_csl_output_append(current,"\n",1,depth?stack[depth-1].format:0,NULL,error,cap)) { result=PARSE_ERROR; break; }
            continue;
        }
        if(t.self || depth==64) { result=PARSE_FALLBACK; break; }
        Frame f={t.tag,depth?stack[depth-1].format:0,display(&t),NULL};
        if(t.tag==TAG_DIV) {
            if(f.display==DISPLAY_NONE || active!=DISPLAY_NONE) { result=PARSE_FALLBACK; break; }
            for(size_t i=0;i<depth;i++) if(stack[i].tag!=TAG_DIV) result=PARSE_FALLBACK;
            if(result!=PARSE_OK) break;
            if(f.display!=DISPLAY_WRAP) {
                if(current && !add_row(a,entry,&capacity,CE_CSL_BIBLIOGRAPHY_FLOW,NULL,&current)) { result=PARSE_ERROR; break; }
                if((f.display==DISPLAY_RIGHT)!=(left!=NULL)) { result=PARSE_FALLBACK; break; }
                active=f.display;
            } else if(left) { result=PARSE_FALLBACK; break; }
        }
        if(t.tag==TAG_B || t.tag==TAG_STRONG) f.format|=CE_CSL_BOLD;
        if(t.tag==TAG_I || t.tag==TAG_EM) f.format|=CE_CSL_ITALIC;
        if(t.tag==TAG_U) f.format|=CE_CSL_UNDERLINE;
        if(t.tag==TAG_SUP) f.format=(f.format&~CE_CSL_SUBSCRIPT)|CE_CSL_SUPERSCRIPT;
        if(t.tag==TAG_SUB) f.format=(f.format&~CE_CSL_SUPERSCRIPT)|CE_CSL_SUBSCRIPT;
        if(t.style && !css(t.style,t.style_size,&f.format)) { result=PARSE_FALLBACK; break; }
        if(t.tag==TAG_A) {
            if(!t.href || !t.href_size || t.href_size>CE_CSL_OUTPUT_MAX_LINK_BYTES) { result=PARSE_FALLBACK; break; }
            for(size_t i=0;i<depth;i++) if(stack[i].tag==TAG_A) result=PARSE_FALLBACK;
            if(result!=PARSE_OK) break;
            result=decode_text(a,t.href,t.href_size,&f.href);
            if(result!=PARSE_OK) break;
            /* append validates the link even for an empty text run. */
            CeCslOutput *probe=output(a,error,cap);
            if(!probe) { release(a,f.href); result=PARSE_ERROR; break; }
            bool valid=ce_csl_output_append(probe,"",0,0,f.href,error,cap);
            ce_csl_output_free(probe);
            if(!valid) { release(a,f.href); result=PARSE_FALLBACK; break; }
        }
        stack[depth++]=f;
    }
    if(result==PARSE_OK && (depth || left)) result=PARSE_FALLBACK;
    if(result==PARSE_OK && current && !add_row(a,entry,&capacity,CE_CSL_BIBLIOGRAPHY_FLOW,NULL,&current)) result=PARSE_ERROR;
    while(depth) release(a,stack[--depth].href);
    ce_csl_output_free(current); ce_csl_output_free(left);
    return result;
}
static bool metadata(CeBibliographyArchive *a,CeBibliographyArchiveEntry *entry,const cJSON *node,
                     const char *key,const char **out) {
    const cJSON *value=field(node,key); const char *text=string(value); size_t size=0;
    if(value && (!bounded(text,8192,&size) || ce_csl_unicode_validate(text,size,NULL)!=CE_CSL_UNICODE_OK)) {
        entry->diagnostics|=CE_BIBLIOGRAPHY_ARCHIVE_INVALID_METADATA; text=""; size=0;
    }
    if(!text) text="";
    *out=copy(a,text,size); return *out!=NULL;
}
static bool count_output(CeBibliographyArchive *a,const CeCslOutput *out) {
    for(size_t i=0;i<ce_csl_output_run_count(out);i++) {
        const CeCslOutputRun *run=ce_csl_output_run(out,i);
        size_t bytes=run->byte_length+(run->href?strlen(run->href):0);
        if(a->run_count==CE_BIBLIOGRAPHY_ARCHIVE_MAX_RUNS || bytes>CE_BIBLIOGRAPHY_ARCHIVE_MAX_BYTES-a->payload) return false;
        a->run_count++; a->payload+=bytes;
    }
    return true;
}
static bool read_entry(CeBibliographyArchive *a,CeBibliographyArchiveEntry *entry,const cJSON *node,
                       bool exact,char *error,size_t cap) {
    if(!cJSON_IsObject(node)) return fail(error,cap,"Archived bibliography entry is not an object");
    if(!metadata(a,entry,node,"citationId",&entry->citation_id) || !metadata(a,entry,node,"key",&entry->key) ||
       !metadata(a,entry,node,"itemUri",&entry->item_uri) || !metadata(a,entry,node,"pdfUri",&entry->pdf_uri)) return false;
    const cJSON *number=field(node,"number");
    if(number) {
        if(exact && cJSON_IsNumber(number) && number->valuedouble>=0 && number->valuedouble<=9007199254740991.0 &&
           floor(number->valuedouble)==number->valuedouble) {
            char label[32]; int length=snprintf(label,sizeof label,"[%.0f]",number->valuedouble);
            CeCslOutput *out=output(a,error,cap); entry->label=out;
            if(!out || length<0 || (size_t)length>=sizeof label ||
               !ce_csl_output_append(out,label,(size_t)length,0,NULL,error,cap)) return false;
        } else entry->diagnostics|=CE_BIBLIOGRAPHY_ARCHIVE_INVALID_METADATA;
    }
    const cJSON *html_node=field(node,"entryHtml"),*text_node=field(node,"entryText");
    const char *html=string(html_node),*text=string(text_node); size_t html_size=0,text_size=0;
    if((html && !bounded(html,CE_CSL_OUTPUT_MAX_BYTES,&html_size)) ||
       (text && !bounded(text,CE_CSL_OUTPUT_MAX_BYTES,&text_size))) return fail(error,cap,"Archived bibliography field exceeds 1 MiB");
    if((html_node && !html) || (text_node && !text)) entry->diagnostics|=CE_BIBLIOGRAPHY_ARCHIVE_INVALID_METADATA;
    Parse parsed=PARSE_FALLBACK;
    if(html) parsed=parse_html(a,entry,html,html_size,error,cap);
    if(parsed==PARSE_ERROR) return fail(error,cap,"Archived bibliography allocation or output limit failed");
    if(parsed==PARSE_FALLBACK) {
        clear_entry(a,entry,false);
        if(html || !text) entry->diagnostics|=CE_BIBLIOGRAPHY_ARCHIVE_FALLBACK_TEXT;
        const char *plain=text?text:html?html:"[unreadable archived entry]";
        size_t size=text?text_size:html?html_size:strlen(plain),capacity=0;
        CeCslOutput *body=output(a,error,cap);
        bool ok=body && ce_csl_output_append(body,plain,size,0,NULL,error,cap) &&
            add_row(a,entry,&capacity,CE_CSL_BIBLIOGRAPHY_FLOW,NULL,&body);
        ce_csl_output_free(body);
        if(!ok) return false;
    }
    if(!count_output(a,entry->label)) return fail(error,cap,"Archive output budget exceeded");
    for(size_t i=0;i<entry->row_count;i++)
        if(!count_output(a,entry->rows[i].left) || !count_output(a,entry->rows[i].body)) return fail(error,cap,"Archive output budget exceeded");
    a->row_count+=entry->row_count; a->diagnostics|=entry->diagnostics; return true;
}
bool ce_bibliography_archive_decode(const cJSON *atom,const CeCslOutputAllocator *allocator,
                                    CeBibliographyArchive **out,char *error,size_t cap) {
    if(error && cap) *error=0;
    if(!out || !ce_bibliography_archive_eligible(atom)) return fail(error,cap,"Only unmarked legacy bibliography sections are archives");
    const char *json=string(field(field(atom,"attrs"),"entriesJson")); size_t size=0;
    if(!bounded(json,CE_BIBLIOGRAPHY_ARCHIVE_MAX_BYTES,&size)) return fail(error,cap,"Archive JSON exceeds 16 MiB");
    cJSON *array=NULL; bool exact=false;
    if(!ce_csl_json_parse_strict_with_numbers(json,size,&array,&exact,error,cap)) return false;
    if(!cJSON_IsArray(array) || cJSON_GetArraySize(array)>(int)CE_BIBLIOGRAPHY_ARCHIVE_MAX_ENTRIES) {
        cJSON_Delete(array); return fail(error,cap,"Archive needs an array of at most 10000 entries");
    }
    CeCslOutputAllocator alloc={NULL,normal_alloc,normal_free}; if(allocator) alloc=*allocator;
    if(!alloc.allocate || !alloc.deallocate) { cJSON_Delete(array); return fail(error,cap,"Archive allocator needs both callbacks"); }
    CeBibliographyArchive *a=alloc.allocate(alloc.context,sizeof *a);
    if(!a) { cJSON_Delete(array); return fail(error,cap,"Out of memory creating archive"); }
    memset(a,0,sizeof *a); a->allocator=alloc; a->workspace=sizeof *a;
    size_t count=(size_t)cJSON_GetArraySize(array);
    if(count) {
        a->entries=allocate(a,count*sizeof *a->entries);
        if(!a->entries) goto bad;
        memset(a->entries,0,count*sizeof *a->entries);
    }
    const cJSON *node=array->child;
    for(size_t i=0;i<count;i++,node=node->next) {
        a->count=i+1; a->entries[i].source_index=i;
        if(!read_entry(a,&a->entries[i],node,exact,error,cap)) goto bad;
    }
    cJSON_Delete(array); CeBibliographyArchive *old=*out; *out=a; ce_bibliography_archive_free(old);
    if(error && cap) *error=0;
    return true;
bad:
    cJSON_Delete(array); ce_bibliography_archive_free(a);
    if(error && cap && !*error) fail(error,cap,"Archive allocation or workspace limit failed");
    return false;
}
static bool plain_copy(const CeCslOutput *out,char *dest,size_t *used) {
    for(size_t i=0;i<ce_csl_output_run_count(out);i++) {
        const CeCslOutputRun *run=ce_csl_output_run(out,i);
        if(run->byte_length>CE_BIBLIOGRAPHY_ARCHIVE_MAX_BYTES-*used) return false;
        if(dest) memcpy(dest+*used,run->text,run->byte_length);
        *used+=run->byte_length;
    }
    return true;
}
static bool plain_char(char *dest,size_t *used,char c) {
    if(*used==CE_BIBLIOGRAPHY_ARCHIVE_MAX_BYTES) return false;
    if(dest) dest[*used]=c;
    ++*used; return true;
}
static bool plain_build(const CeBibliographyArchive *a,char *dest,size_t *used) {
    *used=0;
    for(size_t i=0;i<a->count;i++) {
        const CeBibliographyArchiveEntry *e=&a->entries[i];
        if(i && !plain_char(dest,used,'\n')) return false;
        if(!plain_copy(e->label,dest,used)) return false;
        if(ce_csl_output_run_count(e->label) && e->row_count && !plain_char(dest,used,' ')) return false;
        for(size_t j=0;j<e->row_count;j++) {
            const CeBibliographyArchiveRow *row=&e->rows[j];
            if(j && !plain_char(dest,used,'\n')) return false;
            if(row->left && (!plain_copy(row->left,dest,used) || !plain_char(dest,used,'\t'))) return false;
            if(!plain_copy(row->body,dest,used)) return false;
        }
    }
    return true;
}
bool ce_bibliography_archive_plain(const CeBibliographyArchive *a,CeCslOutputString *out,char *error,size_t cap) {
    if(!a || !out) return fail(error,cap,"Missing archive/plain destination");
    size_t size=0;
    if(!plain_build(a,NULL,&size)) return fail(error,cap,"Archive plain text exceeds 16 MiB");
    char *text=malloc(size+1);
    if(!text) return fail(error,cap,"Out of memory exporting archive text");
    size_t written=0;
    if(!plain_build(a,text,&written)) { free(text); return fail(error,cap,"Archive plain text limit failed"); }
    text[written]=0;
    CeCslOutputString value={text,written,normal_free,NULL};
    ce_csl_output_string_free(out); *out=value;
    if(error && cap) *error=0;
    return true;
}
