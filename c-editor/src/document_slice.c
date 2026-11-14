#include "document_slice.h"
#include "cJSON.h"
#include "csl_json.h"
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct CeDocumentSlice { cJSON *root; size_t open_start, open_end; };
struct CeDocumentCutGuard { char *json; size_t start, end; };
typedef struct { cJSON *node; size_t start, end, depth; } Leaf;
typedef struct { cJSON *root; Leaf *leaves; size_t count, capacity; const CeDocument *document; } Tree;
typedef struct { char *data; size_t size, capacity; } Buffer;
typedef struct { char *error; size_t capacity; bool failed; } Error;

static bool fail(Error *e, const char *message) {
    if (!e->failed && e->error && e->capacity) snprintf(e->error,e->capacity,"Clipboard: %s",message);
    e->failed=true; return false;
}
static const cJSON *get(const cJSON *n,const char *key) { return cJSON_GetObjectItemCaseSensitive(n,key); }
static const char *string(const cJSON *n,const char *key) {
    const cJSON *v=get(n,key); return cJSON_IsString(v)?v->valuestring:"";
}
static bool type_is(const cJSON *n,const char *type) { return !strcmp(string(n,"type"),type); }
static bool paragraph(const cJSON *n) { return type_is(n,"paragraph") || type_is(n,"heading"); }
static bool list(const cJSON *n) { return type_is(n,"bulletList") || type_is(n,"orderedList"); }
static bool container(const cJSON *n) { return type_is(n,"doc") || list(n) || type_is(n,"listItem"); }
static bool leaf(const cJSON *n) {
    return paragraph(n) || type_is(n,"zoteroCitationBlock") || type_is(n,"zoteroEndnoteSection") ||
        (!get(n,"content") && !type_is(n,"doc"));
}
static bool attach(cJSON *parent,const char *key,cJSON *value,Error *e) {
    if (!value || !(key?cJSON_AddItemToObject(parent,key,value):cJSON_AddItemToArray(parent,value))) {
        cJSON_Delete(value); return fail(e,"out of memory attaching document data");
    }
    return true;
}
static bool set(cJSON *parent,const char *key,cJSON *value,Error *e) {
    if (!value) return fail(e,"out of memory constructing document data");
    cJSON_DeleteItemFromObjectCaseSensitive(parent,key);
    return attach(parent,key,value,e);
}
static char *copy_n(const char *s,size_t size,Error *e) {
    char *p=malloc(size+1);
    if (!p) { fail(e,"out of memory copying text"); return NULL; }
    memcpy(p,s,size);p[size]=0;return p;
}
static cJSON *duplicate(const cJSON *n,Error *e) {
    cJSON *result=cJSON_Duplicate(n,true);
    if (!result) fail(e,"out of memory copying a node");
    return result;
}
static cJSON *shell(const cJSON *n,Error *e) {
    cJSON *result=cJSON_CreateObject();
    if (!result) { fail(e,"out of memory creating a node");return NULL; }
    for (const cJSON *field=n->child;field && !e->failed;field=field->next)
        if (strcmp(field->string,"content")) attach(result,field->string,duplicate(field,e),e);
    if (!e->failed) attach(result,"content",cJSON_CreateArray(),e);
    if (e->failed) { cJSON_Delete(result);return NULL; }
    return result;
}
static cJSON *blank(Error *e) {
    cJSON *p=cJSON_CreateObject();
    if (!p) { fail(e,"out of memory creating an endpoint paragraph");return NULL; }
    if (!p || !attach(p,"type",cJSON_CreateString("paragraph"),e) ||
        !attach(p,"content",cJSON_CreateArray(),e)) { cJSON_Delete(p);return NULL; }
    return p;
}
static char *print(const cJSON *n,Error *e) {
    char *json=ce_csl_json_print_exact_with_limit(n,CE_DOCUMENT_SLICE_MAX_BYTES);
    if (!json) { fail(e,"JSON exceeds the clipboard limit or memory is unavailable");return NULL; }
    char *result=copy_n(json,strlen(json),e);cJSON_free(json);return result;
}
static bool append(Buffer *b,const char *s,size_t size,Error *e) {
    if (size>CE_DOCUMENT_SLICE_MAX_BYTES-b->size) return fail(e,"readable output exceeds 16 MiB");
    size_t need=b->size+size+1;
    if (need>b->capacity) {
        size_t capacity=b->capacity?b->capacity:128;
        while (capacity<need) capacity*=2;
        char *p=realloc(b->data,capacity);
        if (!p) return fail(e,"out of memory exporting clipboard text");
        b->data=p;b->capacity=capacity;
    }
    memcpy(b->data+b->size,s,size);b->size+=size;b->data[b->size]=0;return true;
}
static bool adds(Buffer *b,const char *s,Error *e) { return append(b,s,strlen(s),e); }
static bool escaped(Buffer *b,const char *s,Error *e) {
    for (const char *p=s;*p;p++) {
        const char *entity=*p=='&'?"&amp;":*p=='<'?"&lt;":*p=='>'?"&gt;":*p=='\"'?"&quot;":*p=='\''?"&#39;":*p=='\r'?"&#13;":NULL;
        if (!(entity?adds(b,entity,e):append(b,p,1,e))) return false;
    }
    return true;
}
static bool text_boundary(const char *s,size_t size,size_t offset) {
    return offset<=size && (offset==size || ((unsigned char)s[offset]&0xc0)!=0x80);
}
static bool bounds(const CeDocument *d,size_t from,size_t to,Error *e) {
    if (!d || from>to || !text_boundary(ce_document_text(d),ce_document_length(d),from) ||
        !text_boundary(ce_document_text(d),ce_document_length(d),to))
        return fail(e,"invalid UTF-8 selection boundary");
    return true;
}
static bool inspect(Tree *t,cJSON *node,size_t depth,Error *e) {
    if (depth>CE_DOCUMENT_SLICE_MAX_DEPTH) return fail(e,"document nesting exceeds 128");
    if (leaf(node)) {
        if (t->count==t->capacity) {
            size_t capacity=t->capacity?t->capacity*2:16;
            if (capacity>CE_DOCUMENT_SLICE_MAX_NODES) capacity=CE_DOCUMENT_SLICE_MAX_NODES;
            if (capacity<=t->count) return fail(e,"too many document blocks");
            Leaf *p=realloc(t->leaves,capacity*sizeof *p);
            if (!p) return fail(e,"out of memory indexing document blocks");
            t->leaves=p;t->capacity=capacity;
        }
        CeBlockInfo block;
        if (!ce_document_block_info(t->document,t->count,&block)) return fail(e,"document/tree indexes disagree");
        t->leaves[t->count++]=(Leaf){node,block.start,block.end,depth};
    } else for (cJSON *child=get(node,"content")?get(node,"content")->child:NULL;child;child=child->next)
        if (!inspect(t,child,depth+1,e)) return false;
    return true;
}
static void tree_free(Tree *t) { cJSON_Delete(t->root);free(t->leaves);memset(t,0,sizeof *t); }
static bool tree_load(const CeDocument *d,Tree *t,char **snapshot,Error *e) {
    char *json=ce_document_export_json(d);
    if (!json) return fail(e,"unable to snapshot source document");
    bool ok=ce_csl_json_parse_strict(json,strlen(json),&t->root,e->error,e->capacity);
    if (!ok) e->failed=true;
    t->document=d;
    if (ok) ok=inspect(t,t->root,0,e) && t->count==ce_document_block_count(d);
    if (ok && snapshot) { *snapshot=json;json=NULL; }
    free(json);if (!ok) tree_free(t);return ok;
}
static size_t block_at(const Tree *t,size_t p) {
    size_t lo=0,hi=t->count;
    while (lo<hi) { size_t mid=lo+(hi-lo)/2;if (t->leaves[mid].end<p) lo=mid+1;else hi=mid; }
    return lo<t->count?lo:t->count?t->count-1:0;
}
static size_t leaves_in(const cJSON *n) {
    if (leaf(n)) return 1;
    size_t count=0;for (const cJSON *c=get(n,"content")?get(n,"content")->child:NULL;c;c=c->next) count+=leaves_in(c);
    return count;
}
static cJSON *inline_clip(const cJSON *n,size_t from,size_t to,Error *e) {
    cJSON *result=shell(n,e);size_t position=0;
    if (!result) return NULL;
    cJSON *out=cJSON_GetObjectItemCaseSensitive(result,"content");
    for (const cJSON *c=get(n,"content")?get(n,"content")->child:NULL;c;c=c->next) {
        if (!type_is(c,"text") && !type_is(c,"hardBreak")) {
            fail(e,"partial selection through an unknown inline node is unsupported");break;
        }
        size_t size=type_is(c,"text")?strlen(string(c,"text")):1;
        if (position<to && position+size>from) {
            cJSON *part=duplicate(c,e);
            if (part && type_is(c,"text")) {
                size_t start=from>position?from-position:0,end=to<position+size?to-position:size;
                char *text=copy_n(string(c,"text")+start,end-start,e);
                if (text) { set(part,"text",cJSON_CreateString(text),e);free(text); }
            }
            if (e->failed) { cJSON_Delete(part);break; }
            if (!attach(out,NULL,part,e)) break;
        }
        position+=size;
    }
    if (e->failed) { cJSON_Delete(result);return NULL; }
    return result;
}
/* The endpoint leaf is kept even when its selected text is empty: A\nB at
 * [1,2) is two open empty paragraphs, not a zero-length fragment. */
static cJSON *clip_node(const cJSON *n,const Tree *t,size_t *ordinal,size_t first,size_t last,
    size_t from,size_t to,Error *e) {
    size_t count=leaves_in(n),begin=*ordinal;
    if (!count) {
        if (!container(n)) fail(e,"cannot partially edit a hidden unknown structure");
        return NULL;
    }
    size_t finish=begin+count-1;
    if (finish<first || begin>last) { *ordinal+=count;return NULL; }
    if (leaf(n)) {
        const Leaf *info=&t->leaves[(*ordinal)++];
        size_t a=from>info->start?from-info->start:0,b=to<info->end?to-info->start:info->end-info->start;
        if (a>info->end-info->start || b>info->end-info->start || a>b) return NULL;
        if (paragraph(n)) {
            if (!a && b==info->end-info->start) return duplicate(n,e);
            return inline_clip(n,a,b,e);
        }
        if (!a && b==info->end-info->start) return duplicate(n,e);
        if (a==b) return blank(e); /* Only the adjacent separator was selected. */
        fail(e,"atomic nodes must be copied in full");return NULL;
    }
    if (!container(n)) {
        if (from>t->leaves[begin].start || to<t->leaves[finish].end) {
            fail(e,"partial selection through an unknown structural wrapper is unsupported");return NULL;
        }
        *ordinal+=count;return duplicate(n,e);
    }
    cJSON *result=shell(n,e);if (!result) return NULL;
    cJSON *out=cJSON_GetObjectItemCaseSensitive(result,"content");
    size_t skipped=0;bool kept=false;
    for (const cJSON *c=get(n,"content")?get(n,"content")->child:NULL;c && !e->failed;c=c->next) {
        cJSON *part=clip_node(c,t,ordinal,first,last,from,to,e);
        if (part) {
            if (!kept && skipped && type_is(n,"orderedList")) {
                cJSON *attrs=cJSON_GetObjectItemCaseSensitive(result,"attrs");
                const cJSON *old=get(attrs,"start");double value=old && cJSON_IsNumber(old)?old->valuedouble:1;
                if ((old && !cJSON_IsNumber(old)) || floor(value)!=value || fabs(value)>9007199254740991.0 || value+(double)skipped>9007199254740991.0) {
                    cJSON_Delete(part);fail(e,"cannot preserve the clipped ordered-list start");break;
                }
                if (!attrs) {
                    if (!attach(result,"attrs",cJSON_CreateObject(),e)) { cJSON_Delete(part);break; }
                    attrs=cJSON_GetObjectItemCaseSensitive(result,"attrs");
                }
                if (!set(attrs,"start",cJSON_CreateNumber(value+(double)skipped),e)) { cJSON_Delete(part);break; }
            }
            attach(out,NULL,part,e);kept=true;
        } else if (!kept) ++skipped;
    }
    if (e->failed || !out->child) { cJSON_Delete(result);return NULL; }
    return result;
}
static cJSON *clip(const Tree *t,size_t from,size_t to,Error *e) {
    cJSON *root=shell(t->root,e);if (!root) return NULL;
    cJSON *out=cJSON_GetObjectItemCaseSensitive(root,"content");
    if (t->count) {
        size_t ordinal=0,first=block_at(t,from),last=block_at(t,to);
        for (const cJSON *c=get(t->root,"content")->child;c && !e->failed;c=c->next) {
            cJSON *part=clip_node(c,t,&ordinal,first,last,from,to,e);
            if (part) attach(out,NULL,part,e);
        }
    }
    if (e->failed) { cJSON_Delete(root);return NULL; }
    return root;
}
static const cJSON *edge(const cJSON *n,bool last) {
    const cJSON *a=get(n,"content"),*c=a?a->child:NULL;
    if (c && last) while (c->next) c=c->next;
    return c;
}
static size_t open_depth(const cJSON *root,bool last) {
    size_t depth=0;const cJSON *n=edge(root,last);
    while (n) {
        ++depth;
        if (paragraph(n)) return depth;
        if (!list(n) && !type_is(n,"listItem")) return 0;
        n=edge(n,last);
    }
    return 0;
}
static size_t selected_open(const cJSON *n,const Tree *t,size_t *ordinal,size_t wanted,
    size_t from,size_t to,size_t depth) {
    size_t count=leaves_in(n),begin=*ordinal;
    if (!count || wanted<begin || wanted>=begin+count) { *ordinal+=count;return SIZE_MAX; }
    if ((list(n) || type_is(n,"listItem")) && from<=t->leaves[begin].start && to>=t->leaves[begin+count-1].end)
        return depth?depth-1:0;
    if (leaf(n)) return paragraph(n)?depth:0;
    if (!container(n)) return 0;
    for (const cJSON *c=get(n,"content")?get(n,"content")->child:NULL;c;c=c->next) {
        size_t result=selected_open(c,t,ordinal,wanted,from,to,depth+1);
        if (result!=SIZE_MAX) return result;
    }
    return 0;
}
static cJSON *envelope(cJSON *content,size_t start,size_t end,Error *e) {
    cJSON *root=cJSON_CreateObject();
    if (!root) { cJSON_Delete(content);fail(e,"out of memory creating slice");return NULL; }
    if (!attach(root,"content",content,e) ||
        !attach(root,"type",cJSON_CreateString("notelab-slice"),e) ||
        !attach(root,"version",cJSON_CreateNumber(1),e) ||
        !attach(root,"openStart",cJSON_CreateNumber((double)start),e) ||
        !attach(root,"openEnd",cJSON_CreateNumber((double)end),e)) { cJSON_Delete(root);return NULL; }
    return root;
}
void ce_document_slice_free(CeDocumentSlice *slice) { if (slice) { cJSON_Delete(slice->root);free(slice); } }
void ce_document_cut_guard_free(CeDocumentCutGuard *guard) { if (guard) { free(guard->json);free(guard); } }

bool ce_document_slice_capture(const CeDocument *d,size_t from,size_t to,CeDocumentSlice **out,
    CeDocumentCutGuard **guard,char *error,size_t capacity) {
    Error e={error,capacity,false};if (error && capacity) *error=0;
    if (!out || !bounds(d,from,to,&e)) return fail(&e,"missing slice output");
    Tree t={0};char *snapshot=NULL;cJSON *cropped=NULL;
    CeDocumentSlice *result=calloc(1,sizeof *result);CeDocumentCutGuard *token=guard?calloc(1,sizeof *token):NULL;
    bool ok=result && (!guard || token) && tree_load(d,&t,guard?&snapshot:NULL,&e);
    if (ok && from!=to) {
        cropped=clip(&t,from,to,&e);ok=cropped!=NULL;
        if (ok && t.count) {
            size_t first=block_at(&t,from),last=block_at(&t,to),ordinal=0;
            result->open_start=selected_open(t.root,&t,&ordinal,first,from,to,0);ordinal=0;
            result->open_end=selected_open(t.root,&t,&ordinal,last,from,to,0);
            if (!paragraph(t.leaves[first].node) && from==t.leaves[first].end) result->open_start=open_depth(cropped,false);
            if (!paragraph(t.leaves[last].node) && to==t.leaves[last].start) result->open_end=open_depth(cropped,true);
        }
    }
    if (ok) {
        cJSON *content=cropped?cJSON_DetachItemFromObjectCaseSensitive(cropped,"content"):cJSON_CreateArray();
        result->root=envelope(content,result->open_start,result->open_end,&e);ok=result->root!=NULL;
    }
    if (ok) {
        char *validation=print(result->root,&e);cJSON *checked=NULL;ok=validation!=NULL;
        if (ok && !ce_csl_json_parse_strict(validation,strlen(validation),&checked,error,capacity)) { e.failed=true;ok=false; }
        cJSON_Delete(checked);free(validation);
    }
    if (ok && token) { token->json=snapshot;snapshot=NULL;token->start=from;token->end=to; }
    tree_free(&t);cJSON_Delete(cropped);free(snapshot);
    if (!ok) { ce_document_slice_free(result);ce_document_cut_guard_free(token);return fail(&e,"unable to capture slice"); }
    ce_document_slice_free(*out);*out=result;
    if (guard) { ce_document_cut_guard_free(*guard);*guard=token; }
    return true;
}

bool ce_document_cut_guard_matches(const CeDocumentCutGuard *guard,const CeDocument *d,
    size_t from,size_t to,bool *matches,char *error,size_t capacity) {
    Error e={error,capacity,false};if (error && capacity) *error=0;
    if (!guard || !d || !matches) return fail(&e,"invalid Cut guard arguments");
    if (from!=guard->start || to!=guard->end) { *matches=false;return true; }
    char *json=ce_document_export_json(d);
    if (!json) return fail(&e,"unable to validate Cut source");
    bool result=!strcmp(json,guard->json);free(json);*matches=result;return true;
}

static bool only_keys(const cJSON *node,const char *const *keys,size_t count) {
    for (const cJSON *v=node->child;v;v=v->next) {
        size_t i=0;for (;i<count;i++) if (!strcmp(v->string,keys[i])) break;
        if (i==count) return false;
    }
    return true;
}
static bool natural(const cJSON *v,size_t maximum,size_t *out) {
    if (!cJSON_IsNumber(v) || v->valuedouble<0 || v->valuedouble>(double)maximum ||
        floor(v->valuedouble)!=v->valuedouble) return false;
    *out=(size_t)v->valuedouble;return true;
}
static cJSON *document_root(const cJSON *content,Error *e) {
    cJSON *doc=cJSON_CreateObject();
    if (!doc || !attach(doc,"type",cJSON_CreateString("doc"),e) ||
        !attach(doc,"content",duplicate(content,e),e)) { cJSON_Delete(doc);return NULL; }
    return doc;
}
static CeDocument *import_tree(const cJSON *root,Error *e) {
    char *json=print(root,e);cJSON *checked=NULL;
    if (!json) return NULL;
    if (!ce_csl_json_parse_strict(json,strlen(json),&checked,e->error,e->capacity)) {
        free(json);e->failed=true;return NULL;
    }
    cJSON_Delete(checked);
    CeDocument *document=ce_document_new();
    if (!document || !ce_document_import(document,json,CE_FORMAT_JSON,e->error,e->capacity)) {
        ce_document_free(document);document=NULL;e->failed=true;
    }
    free(json);return document;
}
bool ce_document_slice_parse(const char *data,size_t size,CeDocumentSlice **out,
    char *error,size_t capacity) {
    Error e={error,capacity,false};if (error && capacity) *error=0;
    if (!out) return fail(&e,"missing slice output");
    CeDocumentSlice *result=calloc(1,sizeof *result);
    if (!result) return fail(&e,"out of memory creating slice");
    bool ok=ce_csl_json_parse_strict(data,size,&result->root,error,capacity);
    if (!ok) e.failed=true;
    static const char *const keys[]={"type","version","openStart","openEnd","content"};size_t version=0;
    if (ok && (!cJSON_IsObject(result->root) || !only_keys(result->root,keys,5) ||
        !type_is(result->root,"notelab-slice") || !natural(get(result->root,"version"),1,&version) || version!=1 ||
        !natural(get(result->root,"openStart"),CE_DOCUMENT_SLICE_MAX_DEPTH,&result->open_start) ||
        !natural(get(result->root,"openEnd"),CE_DOCUMENT_SLICE_MAX_DEPTH,&result->open_end) ||
        !cJSON_IsArray(get(result->root,"content")) ||
        result->open_start>open_depth(result->root,false) || result->open_end>open_depth(result->root,true)))
        ok=fail(&e,"invalid slice envelope or open boundary");
    if (ok) {
        for (const cJSON *n=get(result->root,"content")->child;n;n=n->next)
            if (type_is(n,"text") || type_is(n,"hardBreak") || type_is(n,"listItem") || type_is(n,"doc")) {
                ok=fail(&e,"slice content must contain block nodes");break;
            }
    }
    if (ok) {
        cJSON *doc=document_root(get(result->root,"content"),&e);
        CeDocument *validated=doc?import_tree(doc,&e):NULL;
        ok=validated!=NULL;ce_document_free(validated);cJSON_Delete(doc);
    }
    if (!ok) { ce_document_slice_free(result);return fail(&e,"unable to parse clipboard slice"); }
    ce_document_slice_free(*out);*out=result;return true;
}
bool ce_document_slice_json(const CeDocumentSlice *slice,char **out,char *error,size_t capacity) {
    Error e={error,capacity,false};if (error && capacity) *error=0;
    if (!slice || !out) return fail(&e,"invalid slice export arguments");
    char *text=print(slice->root,&e);if (!text) return false;
    free(*out);*out=text;return true;
}
static bool utf8_text(const char *text,Error *e) {
    /* The strict JSON helper supplies the same UTF-8/NUL checks as source
     * documents, without interpreting callback text as markup or a cache. */
    cJSON *v=cJSON_CreateString(text);if (!v) return fail(e,"out of memory validating readable text");
    char *json=print(v,e);cJSON_Delete(v);if (!json) return false;
    cJSON *parsed=NULL;bool ok=ce_csl_json_parse_strict(json,strlen(json),&parsed,e->error,e->capacity);
    cJSON_Delete(parsed);free(json);if (!ok) e->failed=true;return ok;
}
static bool atom_label(const cJSON *node,CeDocumentSliceAtomText provider,void *context,char **out,Error *e) {
    char *provided=NULL;
    if (provider && !provider(context,node,&provided,e->error,e->capacity)) {
        free(provided);e->failed=true;return false;
    }
    if (provided) {
        if (strlen(provided)>CE_DOCUMENT_SLICE_MAX_BYTES || !utf8_text(provided,e)) { free(provided);return false; }
        *out=provided;return true;
    }
    Buffer text={0};const cJSON *attrs=get(node,"attrs");
    if (type_is(node,"zoteroCitationBlock")) {
        adds(&text,"[引用] ",e);
        const char *title=string(attrs,"title");if (!*title) title=string(attrs,"citeKey");if (!*title) title=string(attrs,"itemKey");
        adds(&text,*title?title:"未命名文献",e);
        const char *keys[]={"author","year","page"};
        for (size_t i=0;i<3;i++) if (*string(attrs,keys[i])) { adds(&text," · ",e);adds(&text,string(attrs,keys[i]),e); }
        if (*string(attrs,"note")) { adds(&text,"\n摘录：",e);adds(&text,string(attrs,"note"),e); }
    } else if (type_is(node,"zoteroEndnoteSection")) {
        adds(&text,"[参考文献]",e);
        const char *serialized=string(attrs,"entriesJson");cJSON *entries=NULL;
        if (*serialized && !ce_csl_json_parse_strict(serialized,strlen(serialized),&entries,e->error,e->capacity)) e->failed=true;
        if (entries && !cJSON_IsArray(entries)) fail(e,"bibliography entries are not an array");
        for (const cJSON *entry=entries?entries->child:NULL;entry && !e->failed;entry=entry->next) {
            adds(&text,"\n",e);
            const char *label=string(entry,"entryHtml");
            /* Preserve the literal compatibility field. It is never parsed as
             * executable markup, nor mistaken for fresh processor output. */
            if (!*label) label=string(entry,"key");
            adds(&text,*label?label:"未命名文献",e);
        }
        cJSON_Delete(entries);
    } else { adds(&text,"[",e);adds(&text,string(node,"type"),e);adds(&text,"]",e); }
    if (e->failed) { free(text.data);return false; }
    *out=text.data;return true;
}
static bool plain_inline(const cJSON *node,Buffer *text,Error *e) {
    if (type_is(node,"text")) return adds(text,string(node,"text"),e);
    if (type_is(node,"hardBreak")) return adds(text,"\n",e);
    for (const cJSON *n=get(node,"content")?get(node,"content")->child:NULL;n;n=n->next)
        if (!plain_inline(n,text,e)) return false;
    return true;
}
static bool plain_node(const cJSON *node,Buffer *text,bool *have_block,
    CeDocumentSliceAtomText provider,void *context,Error *e) {
    if (leaf(node)) {
        if (*have_block && !adds(text,"\n",e)) return false;
        *have_block=true;
        if (paragraph(node)) return plain_inline(node,text,e);
        char *label=NULL;bool ok=atom_label(node,provider,context,&label,e);
        if (ok) ok=adds(text,label,e);free(label);return ok;
    }
    for (const cJSON *n=get(node,"content")?get(node,"content")->child:NULL;n;n=n->next)
        if (!plain_node(n,text,have_block,provider,context,e)) return false;
    return true;
}
bool ce_document_slice_plain(const CeDocumentSlice *slice,CeDocumentSliceAtomText provider,
    void *context,char **out,char *error,size_t capacity) {
    Error e={error,capacity,false};if (error && capacity) *error=0;
    if (!slice || !out) return fail(&e,"invalid plain export arguments");
    Buffer text={0};bool have=false;
    bool ok=plain_node(slice->root,&text,&have,provider,context,&e) && append(&text,"",0,&e);
    if (!ok) { free(text.data);return false; }
    free(*out);*out=text.data;return true;
}

static bool color(const char *s) {
    size_t n=strlen(s);if (*s!='#' || (n!=4 && n!=7 && n!=9)) return false;
    for (size_t i=1;i<n;i++) if (!((s[i]>='0' && s[i]<='9') || (s[i]>='a' && s[i]<='f') || (s[i]>='A' && s[i]<='F'))) return false;
    return true;
}
static bool href(const char *s) {
    if (strncmp(s,"https://",8) && strncmp(s,"http://",7) && strncmp(s,"mailto:",7)) return false;
    for (const unsigned char *p=(const unsigned char *)s;*p;p++) if (*p<=32 || *p==127 || *p=='\\') return false;
    return true;
}
static bool html_inline(const cJSON *node,Buffer *text,Error *e) {
    if (type_is(node,"hardBreak")) return adds(text,"<br>",e);
    if (!type_is(node,"text")) {
        for (const cJSON *c=get(node,"content")?get(node,"content")->child:NULL;c;c=c->next)
            if (!html_inline(c,text,e)) return false;
        return true;
    }
    const cJSON *marks=get(node,"marks");size_t count=(size_t)cJSON_GetArraySize(marks),used=0;
    const char **closing=count?calloc(count,sizeof *closing):NULL;
    if (count && !closing) return fail(e,"out of memory exporting marks");
    for (const cJSON *mark=marks?marks->child:NULL;mark && !e->failed;mark=mark->next) {
        const cJSON *attrs=get(mark,"attrs");const char *end=NULL;
        if (type_is(mark,"bold")) { adds(text,"<strong>",e);end="</strong>"; }
        else if (type_is(mark,"italic")) { adds(text,"<em>",e);end="</em>"; }
        else if (type_is(mark,"underline")) { adds(text,"<u>",e);end="</u>"; }
        else if (type_is(mark,"link") && href(string(attrs,"href"))) {
            adds(text,"<a href=\"",e);escaped(text,string(attrs,"href"),e);adds(text,"\" rel=\"noopener noreferrer\">",e);end="</a>";
        } else if ((type_is(mark,"textStyle") || type_is(mark,"highlight")) && color(string(attrs,"color"))) {
            adds(text,type_is(mark,"highlight")?"<span style=\"background-color:":"<span style=\"color:",e);
            adds(text,string(attrs,"color"),e);adds(text,"\">",e);end="</span>";
        }
        if (end) closing[used++]=end;
    }
    if (!e->failed) escaped(text,string(node,"text"),e);
    while (used && !e->failed) adds(text,closing[--used],e);
    free(closing);return !e->failed;
}
static bool html_node(const cJSON *node,Buffer *text,CeDocumentSliceAtomText provider,void *context,Error *e) {
    const char *tag="div";char heading[3]="h1";
    if (type_is(node,"paragraph")) tag="p";
    else if (type_is(node,"heading")) {
        size_t level=0;if (natural(get(get(node,"attrs"),"level"),6,&level) && level) { heading[1]=(char)('0'+level);tag=heading; } else tag="p";
    } else if (type_is(node,"bulletList")) tag="ul";
    else if (type_is(node,"orderedList")) tag="ol";
    else if (type_is(node,"listItem")) tag="li";
    adds(text,"<",e);adds(text,tag,e);
    if (type_is(node,"orderedList")) {
        const cJSON *start=get(get(node,"attrs"),"start");
        if (cJSON_IsNumber(start) && floor(start->valuedouble)==start->valuedouble && fabs(start->valuedouble)<=9007199254740991.0) {
            char number[64];snprintf(number,sizeof number," start=\"%.0f\"",start->valuedouble);adds(text,number,e);
        }
    }
    adds(text,">",e);
    if (paragraph(node)) html_inline(node,text,e);
    else if (leaf(node)) {
        char *label=NULL;
        if (atom_label(node,provider,context,&label,e)) escaped(text,label,e);
        free(label);
    } else for (const cJSON *c=get(node,"content")?get(node,"content")->child:NULL;c && !e->failed;c=c->next)
        html_node(c,text,provider,context,e);
    adds(text,"</",e);adds(text,tag,e);adds(text,">",e);return !e->failed;
}
bool ce_document_slice_html(const CeDocumentSlice *slice,CeDocumentSliceAtomText provider,
    void *context,char **out,char *error,size_t capacity) {
    Error e={error,capacity,false};if (error && capacity) *error=0;
    if (!slice || !out) return fail(&e,"invalid HTML export arguments");
    char *json=print(slice->root,&e);if (!json) return false;
    Buffer html={0};adds(&html,"<div data-notelab-slice=\"",&e);escaped(&html,json,&e);free(json);
    adds(&html,"\" style=\"white-space:pre-wrap\">",&e);
    for (const cJSON *n=get(slice->root,"content")->child;n && !e.failed;n=n->next) html_node(n,&html,provider,context,&e);
    adds(&html,"</div>",&e);
    if (e.failed) { free(html.data);return false; }
    free(*out);*out=html.data;return true;
}

static bool ascii_equal(const char *a,size_t length,const char *b) {
    if (strlen(b)!=length) return false;
    for (size_t i=0;i<length;i++) {
        unsigned char c=(unsigned char)a[i];if (c>='A' && c<='Z') c=(unsigned char)(c-'A'+'a');
        if (c!=(unsigned char)b[i]) return false;
    }
    return true;
}
static bool html_space(char c) { return c==' ' || c=='\t' || c=='\n' || c=='\r' || c=='\f'; }
static bool name_char(char c) {
    return (c>='a' && c<='z') || (c>='A' && c<='Z') || (c>='0' && c<='9') || c=='-' || c==':' || c=='_';
}
static bool scalar_append(Buffer *b,uint32_t scalar,Error *e) {
    if (!scalar || scalar>0x10ffff || (scalar>=0xd800 && scalar<=0xdfff)) return fail(e,"invalid HTML entity scalar");
    char text[4];size_t size;
    if (scalar<0x80) { text[0]=(char)scalar;size=1; }
    else if (scalar<0x800) { text[0]=(char)(0xc0|(scalar>>6));text[1]=(char)(0x80|(scalar&63));size=2; }
    else if (scalar<0x10000) { text[0]=(char)(0xe0|(scalar>>12));text[1]=(char)(0x80|((scalar>>6)&63));text[2]=(char)(0x80|(scalar&63));size=3; }
    else { text[0]=(char)(0xf0|(scalar>>18));text[1]=(char)(0x80|((scalar>>12)&63));text[2]=(char)(0x80|((scalar>>6)&63));text[3]=(char)(0x80|(scalar&63));size=4; }
    return append(b,text,size,e);
}
static char *decode_attribute(const char *text,size_t size,Error *e) {
    Buffer out={0};
    for (size_t i=0;i<size && !e->failed;) {
        if (text[i]!='&') { append(&out,text+i,1,e);++i;continue; }
        size_t begin=++i,end=begin;while (end<size && text[end]!=';' && end-begin<16) ++end;
        if (end==size || text[end]!=';') { fail(e,"unterminated HTML entity in slice envelope");break; }
        uint32_t value=0;size_t length=end-begin;
        if (length && text[begin]=='#') {
            size_t p=begin+1;unsigned base=10;
            if (p<end && (text[p]=='x' || text[p]=='X')) { ++p;base=16; }
            if (p==end) { fail(e,"empty numeric HTML entity");break; }
            for (;p<end;p++) {
                unsigned digit=text[p]>='0' && text[p]<='9'?(unsigned)(text[p]-'0'):
                    text[p]>='a' && text[p]<='f'?(unsigned)(text[p]-'a'+10):
                    text[p]>='A' && text[p]<='F'?(unsigned)(text[p]-'A'+10):99;
                if (digit>=base || value>(0x10ffff-digit)/base) { fail(e,"invalid numeric HTML entity");break; }
                value=value*base+digit;
            }
        } else if (length==3 && !memcmp(text+begin,"amp",3)) value='&';
        else if (length==2 && !memcmp(text+begin,"lt",2)) value='<';
        else if (length==2 && !memcmp(text+begin,"gt",2)) value='>';
        else if (length==4 && !memcmp(text+begin,"quot",4)) value='"';
        else if (length==4 && !memcmp(text+begin,"apos",4)) value='\'';
        else if (length==4 && !memcmp(text+begin,"nbsp",4)) value=160;
        else { fail(e,"unsupported HTML entity in slice envelope");break; }
        if (!e->failed) scalar_append(&out,value,e);i=end+1;
    }
    if (!e->failed) append(&out,"",0,e);
    if (e->failed) { free(out.data);return NULL; }
    return out.data;
}
bool ce_document_slice_from_html(const char *data,size_t size,CeDocumentSlice **out,
    bool *found,char *error,size_t capacity) {
    Error e={error,capacity,false};if (error && capacity) *error=0;
    if (!out || !found || (!data && size) || size>CE_DOCUMENT_SLICE_MAX_BYTES)
        return fail(&e,"invalid or oversized HTML clipboard input");
    /* No private spelling anywhere: hand ordinary HTML to the caller's own
     * restricted importer without accepting or executing any part of it. */
    const char needle[]="data-notelab-slice";bool possible=false;
    for (size_t i=0;i+sizeof needle-1<=size;i++) if (ascii_equal(data+i,sizeof needle-1,needle)) { possible=true;break; }
    if (!possible) { *found=false;return true; }
    if (memchr(data,0,size)) return fail(&e,"NUL in HTML clipboard input");
    char *payload=NULL;size_t envelopes=0,tags=0,at=0,depth=0;
    char stack[CE_DOCUMENT_SLICE_MAX_DEPTH][16];bool root_closed=false,private_seen=false;
    while (at<size && !e.failed) {
        if (data[at]!='<') {
            if (!depth && !html_space(data[at])) { fail(&e,"text outside the private HTML envelope");break; }
            ++at;continue;
        }
        if (size-at>=4 && !memcmp(data+at,"<!--",4)) {
            size_t end=at+4;while (end+3<=size && memcmp(data+end,"-->",3)) ++end;
            if (end+3>size) { fail(&e,"unterminated HTML comment");break; }
            at=end+3;continue;
        }
        ++at;bool closing=at<size && data[at]=='/';if (closing) ++at;
        size_t name=at;while (at<size && name_char(data[at])) ++at;
        if (name==at) { fail(&e,"malformed HTML around slice envelope");break; }
        size_t tag_length=at-name;char tag[16];
        if (tag_length>=sizeof tag) { fail(&e,"unsupported HTML tag around slice envelope");break; }
        for (size_t i=0;i<tag_length;i++) { char c=data[name+i];tag[i]=c>='A' && c<='Z'?(char)(c-'A'+'a'):c; }
        tag[tag_length]=0;
        bool is_div=!strcmp(tag,"div"),finished=false,self_closing=false;++tags;
        while (at<size && !e.failed) {
            while (at<size && html_space(data[at])) ++at;
            if (at<size && data[at]=='>') { ++at;finished=true;break; }
            if (at+1<size && data[at]=='/' && data[at+1]=='>') { at+=2;finished=true;self_closing=true;break; }
            if (closing || at==size) { fail(&e,"malformed HTML closing tag");break; }
            size_t attr=at;while (at<size && name_char(data[at])) ++at;
            if (attr==at) { fail(&e,"malformed HTML attribute");break; }
            bool private=ascii_equal(data+attr,at-attr,needle);
            while (at<size && html_space(data[at])) ++at;
            bool has_value=at<size && data[at]=='=';
            if (has_value) { ++at;while (at<size && html_space(data[at])) ++at; }
            char quote=has_value && at<size && (data[at]=='\'' || data[at]=='"')?data[at++]:0;
            size_t begin=at;
            if (has_value) {
                if (quote) while (at<size && data[at]!=quote) ++at;
                else while (at<size && !html_space(data[at]) && data[at]!='>') ++at;
            }
            size_t length=at-begin;
            if (quote && at==size) { fail(&e,"unterminated HTML attribute");break; }
            if (quote) ++at;
            if (private) {
                private_seen=true;
                if (!quote || !is_div || tags!=1 || ++envelopes!=1) {
                    fail(&e,"slice envelope must be one quoted attribute on the outer div");break;
                }
                payload=decode_attribute(data+begin,length,&e);
            }
        }
        if (!e.failed && !finished) fail(&e,"unterminated HTML tag around slice envelope");
        if (!e.failed) {
            bool void_tag=!strcmp(tag,"br") || !strcmp(tag,"hr") || !strcmp(tag,"img") || !strcmp(tag,"input") || !strcmp(tag,"meta") || !strcmp(tag,"link");
            if (closing) {
                if (!depth || strcmp(stack[depth-1],tag) || self_closing) fail(&e,"mismatched private HTML envelope");
                else { --depth;if (!depth) root_closed=true; }
            } else if (!self_closing && !void_tag) {
                if (root_closed || depth==CE_DOCUMENT_SLICE_MAX_DEPTH) fail(&e,"multiple or overly nested HTML envelopes");
                else { memcpy(stack[depth],tag,tag_length+1);++depth; }
            } else if (tags==1 && is_div) fail(&e,"private HTML div must have an explicit closing tag");
        }
        if (tags>CE_DOCUMENT_SLICE_MAX_NODES) fail(&e,"too many HTML tags");
    }
    if (!e.failed && envelopes && (depth || !root_closed)) fail(&e,"unclosed private HTML envelope");
    if (!private_seen) { free(payload);*found=false;if (error && capacity) *error=0;return true; }
    CeDocumentSlice *parsed=NULL;
    bool ok=!e.failed && payload && ce_document_slice_parse(payload,strlen(payload),&parsed,error,capacity);
    free(payload);
    if (!ok) { ce_document_slice_free(parsed);return fail(&e,"invalid private HTML slice"); }
    ce_document_slice_free(*out);*out=parsed;*found=true;return true;
}

static bool equal_json(const cJSON *a,const cJSON *b,size_t *work,Error *e) {
    if (++*work>8000000) { fail(e,"metadata comparison work limit exceeded");return false; }
    if (!a || !b) return a==b;
    if ((a->type&255)!=(b->type&255)) return false;
    if (cJSON_IsString(a)) return !strcmp(a->valuestring,b->valuestring);
    if (cJSON_IsNumber(a)) return a->valuedouble==b->valuedouble;
    if (cJSON_IsArray(a)) {
        const cJSON *x=a->child,*y=b->child;
        for (;x && y;x=x->next,y=y->next) if (!equal_json(x,y,work,e)) return false;
        return !x && !y;
    }
    if (cJSON_IsObject(a)) {
        size_t count_a=0,count_b=0;
        for (const cJSON *x=a->child;x;x=x->next) {
            ++count_a;const cJSON *y=b->child;
            for (;y && strcmp(x->string,y->string);y=y->next) if (++*work>8000000) return fail(e,"metadata comparison work limit exceeded");
            if (!y || !equal_json(x,y,work,e)) return false;
        }
        for (const cJSON *y=b->child;y;y=y->next) ++count_b;
        return count_a==count_b;
    }
    return true;
}
static size_t inline_length(const cJSON *n) {
    if (type_is(n,"text")) return strlen(string(n,"text"));
    if (type_is(n,"hardBreak")) return 1;
    size_t size=0;for (const cJSON *c=get(n,"content")?get(n,"content")->child:NULL;c;c=c->next) size+=inline_length(c);
    return size;
}
typedef struct { cJSON *node; size_t offset; } Position;
static cJSON *edge_leaf(cJSON *node,bool last) {
    while (node && !leaf(node)) node=(cJSON *)edge(node,last);
    return node;
}
static bool compatible(const cJSON *a,const cJSON *b,size_t *work,Error *e) {
    for (const cJSON *field=b?b->child:NULL;field;field=field->next)
        if (strcmp(field->string,"type") && strcmp(field->string,"content") && strcmp(field->string,"attrs") &&
            !equal_json(get(a,field->string),field,work,e)) return false;
    if (paragraph(a) && paragraph(b)) {
        const cJSON *attrs=get(b,"attrs");
        for (const cJSON *field=attrs?attrs->child:NULL;field;field=field->next)
            if (strcmp(field->string,"level") && strcmp(field->string,"textAlign") &&
                !equal_json(get(get(a,"attrs"),field->string),field,work,e)) return false;
        return true;
    }
    if ((!list(a) && !type_is(a,"listItem")) || strcmp(string(a,"type"),string(b,"type"))) return false;
    /* Open container boundaries adopt the destination's numbering, while
     * custom attributes must match rather than being silently discarded. */
    const cJSON *aa=get(a,"attrs"),*bb=get(b,"attrs");
    if (type_is(a,"orderedList")) {
        cJSON *x=aa?duplicate(aa,e):cJSON_CreateObject(),*y=bb?duplicate(bb,e):cJSON_CreateObject();
        if (!x || !y) { cJSON_Delete(x);cJSON_Delete(y);return fail(e,"out of memory comparing lists"); }
        cJSON_DeleteItemFromObjectCaseSensitive(x,"start");cJSON_DeleteItemFromObjectCaseSensitive(y,"start");
        bool same=equal_json(x,y,work,e);cJSON_Delete(x);cJSON_Delete(y);return same;
    }
    if ((!aa || !aa->child) && (!bb || !bb->child)) return true;
    return equal_json(aa,bb,work,e);
}
static bool can_join(const cJSON *left,const cJSON *right,size_t depth,size_t *work,Error *e) {
    while (depth) {
        if (!left || !right || !compatible(left,right,work,e)) return false;
        if (paragraph(left)) return paragraph(right);
        --depth;if (!depth) return true;
        left=edge(left,true);right=edge(right,false);
    }
    return true;
}
static bool merge_content(cJSON *left,cJSON *right,size_t depth,Position *position,size_t *work,Error *e);
static bool merge_node(cJSON *left,cJSON *right,size_t depth,Position *position,size_t *work,Error *e) {
    if (!depth || !compatible(left,right,work,e)) return false;
    cJSON *a=cJSON_GetObjectItemCaseSensitive(left,"content"),*b=cJSON_GetObjectItemCaseSensitive(right,"content");
    if (!a || !b) return false;
    if (paragraph(left)) {
        size_t prefix=inline_length(left);
        if (position->node==right) { position->node=left;position->offset+=prefix; }
        return merge_content(a,b,0,position,work,e);
    }
    return merge_content(a,b,depth-1,position,work,e);
}
static bool merge_content(cJSON *left,cJSON *right,size_t depth,Position *position,size_t *work,Error *e) {
    cJSON *last=left->child;if (last) while (last->next) last=last->next;
    if (last && right->child && depth) {
        cJSON *first=right->child;
        bool merged=merge_node(last,first,depth,position,work,e);
        if (e->failed) return false;
        if (merged) cJSON_Delete(cJSON_DetachItemViaPointer(right,first));
    }
    while (right->child) if (!attach(left,NULL,cJSON_DetachItemViaPointer(right,right->child),e)) return false;
    return true;
}
static bool trivial_empty(const cJSON *n) {
    if (!type_is(n,"paragraph") || (get(n,"content") && get(n,"content")->child)) return false;
    for (const cJSON *v=n->child;v;v=v->next) if (strcmp(v->string,"type") && strcmp(v->string,"content")) return false;
    return true;
}
static void trim_empty(cJSON *root,bool last,Position *position) {
    cJSON *array=cJSON_GetObjectItemCaseSensitive(root,"content"),*n=(cJSON *)edge(root,last);
    if (n && trivial_empty(n)) {
        if (position->node==n) position->node=NULL;
        cJSON_Delete(cJSON_DetachItemViaPointer(array,n));
    }
}
static bool single_chain(const cJSON *root) {
    const cJSON *n=root;
    while (!leaf(n)) {
        const cJSON *c=edge(n,false);if (!c || c->next) return false;n=c;
    }
    return paragraph(n);
}
static bool unwrap_inline(cJSON *root,size_t *start,size_t *end,Error *e) {
    if (!*start || !*end || *start!=open_depth(root,false) || *end!=open_depth(root,true) || !single_chain(root)) return true;
    cJSON *n=edge_leaf(root,false);if (!n) return true;
    cJSON *copy=duplicate(n,e),*content=cJSON_CreateArray();
    if (!content) { cJSON_Delete(copy);return fail(e,"out of memory fitting inline slice"); }
    if (!attach(content,NULL,copy,e)) { cJSON_Delete(content);return false; }
    if (!set(root,"content",content,e)) return false;
    *start=*end=1;return true;
}
static bool wrap_context(const cJSON *left,cJSON *insert,size_t *start,size_t *end,Error *e) {
    if (!*start || !*end || !paragraph(edge(insert,false)) || !paragraph(edge(insert,true))) return true;
    const cJSON *path[CE_DOCUMENT_SLICE_MAX_DEPTH];size_t count=0;
    const cJSON *n=edge(left,true);
    while (n && (list(n) || type_is(n,"listItem"))) { path[count++]=n;n=edge(n,true); }
    if (!n || !paragraph(n) || !count) return true;
    cJSON *content=cJSON_DetachItemFromObjectCaseSensitive(insert,"content");
    for (size_t i=count;i && !e->failed;i--) {
        cJSON *parent=shell(path[i-1],e),*next=cJSON_CreateArray();
        if (!parent || !next) { cJSON_Delete(parent);cJSON_Delete(next);cJSON_Delete(content);return fail(e,"out of memory fitting list context"); }
        if (!set(parent,"content",content,e)) { cJSON_Delete(parent);cJSON_Delete(next);return false; }
        content=NULL;
        if (!attach(next,NULL,parent,e)) { cJSON_Delete(next);return false; }
        content=next;
    }
    if (!set(insert,"content",content,e)) return false;
    *start+=count;*end+=count;return true;
}

typedef struct { cJSON *node; char *id,*identity; bool inserted,repair; size_t ordinal; } Citation;
typedef struct { Citation *data; size_t count,capacity,bibliographies,inserted_citations; } Citations;
static bool collect(Citations *v,cJSON *n,bool inserted,Error *e) {
    if (type_is(n,"zoteroEndnoteSection")) { if (inserted) ++v->bibliographies;return true; }
    if (type_is(n,"zoteroCitationBlock")) {
        if (v->count==v->capacity) {
            size_t capacity=v->capacity?v->capacity*2:16;
            if (capacity>CE_DOCUMENT_SLICE_MAX_NODES) capacity=CE_DOCUMENT_SLICE_MAX_NODES;
            if (capacity<=v->count) return fail(e,"too many citations in clipboard operation");
            Citation *data=realloc(v->data,capacity*sizeof *data);
            if (!data) return fail(e,"out of memory indexing citation identities");
            v->data=data;v->capacity=capacity;
        }
        const cJSON *attrs=get(n,"attrs"),*id=get(attrs,"citationId");
        if (id && !cJSON_IsString(id)) return fail(e,"citationId must be a string");
        char *text=copy_n(string(attrs,"citationId"),strlen(string(attrs,"citationId")),e);if (!text) return false;
        size_t ordinal=v->count;v->data[v->count++]=(Citation){n,text,NULL,inserted,inserted && !*text,ordinal};
        if (inserted) ++v->inserted_citations;
        return true;
    }
    for (cJSON *child=get(n,"content")?get(n,"content")->child:NULL;child;child=child->next)
        if (!collect(v,child,inserted,e)) return false;
    return true;
}
static void citations_free(Citations *v) {
    for (size_t i=0;i<v->count;i++) { free(v->data[i].id);free(v->data[i].identity); }
    free(v->data);
}
static int id_order(const void *a,const void *b) {
    const Citation *x=*(const Citation *const *)a,*y=*(const Citation *const *)b;
    int order=strcmp(x->id,y->id);if (order) return order;
    return x->ordinal<y->ordinal?-1:x->ordinal>y->ordinal?1:0;
}
static int id_key(const void *key,const void *value) {
    return strcmp(*(const char *const *)key,(*(const Citation *const *)value)->id);
}
static int identity_order(const void *a,const void *b) {
    return strcmp((*(const Citation *const *)a)->identity,(*(const Citation *const *)b)->identity);
}
static char *scalar(const cJSON *v,Error *e) {
    if (!v || cJSON_IsNull(v)) return copy_n("",0,e);
    if (cJSON_IsString(v)) return copy_n(v->valuestring,strlen(v->valuestring),e);
    if (cJSON_IsNumber(v) && fabs(v->valuedouble)<=9007199254740991.0 && floor(v->valuedouble)==v->valuedouble) {
        char text[64];snprintf(text,sizeof text,"%.0f",v->valuedouble);return copy_n(text,strlen(text),e);
    }
    fail(e,"invalid citation source/library identity");return NULL;
}
static char *identity(const cJSON *n,Error *e) {
    const cJSON *attrs=get(n,"attrs");const char *fields[]={"itemUri","itemKey","citeKey"};
    for (size_t i=0;i<3;i++) if (get(attrs,fields[i]) && !cJSON_IsString(get(attrs,fields[i])) && !cJSON_IsNull(get(attrs,fields[i]))) {
        fail(e,"invalid citation identity field");return NULL;
    }
    const char *tag="uri",*value=string(attrs,"itemUri");char *source=NULL,*library=NULL;
    if (!*value) {
        source=scalar(get(attrs,"sourceId"),e);library=scalar(get(attrs,"libraryId"),e);
        if (!source || !library) { free(source);free(library);return NULL; }
        if (*source) { tag=*library?"library-source":"source";value=source; }
        else if (*string(attrs,"itemKey")) { tag=*library?"library":"key";value=string(attrs,"itemKey"); }
        else if (*string(attrs,"citeKey")) { tag="cite";value=string(attrs,"citeKey");free(library);library=NULL; }
        else { tag="id";value=string(attrs,"citationId");free(library);library=NULL; }
    }
    size_t size=strlen(tag)+strlen(value)+(library?strlen(library):0)+64;
    char *result=malloc(size);
    if (result) {
        if (library && *library) snprintf(result,size,"%s:%zu:%s:%s",tag,strlen(library),library,value);
        else snprintf(result,size,"%s:%s",tag,value);
    } else fail(e,"out of memory copying citation identity");
    free(source);free(library);return result;
}
static bool snapshots_equal(const char *a,const char *b,size_t *work,Error *e) {
    if (!strcmp(a,b)) return true;
    cJSON *x=NULL,*y=NULL;bool exact_x=false,exact_y=false;
    bool ok=ce_csl_json_parse_strict_with_numbers(a,strlen(a),&x,&exact_x,e->error,e->capacity) &&
        ce_csl_json_parse_strict_with_numbers(b,strlen(b),&y,&exact_y,e->error,e->capacity);
    if (!ok) e->failed=true;
    if (ok && (!cJSON_IsObject(x) || !cJSON_IsObject(y))) ok=fail(e,"CSL snapshots must be objects");
    if (ok) {
        cJSON_DeleteItemFromObjectCaseSensitive(x,"id");cJSON_DeleteItemFromObjectCaseSensitive(y,"id");
        ok=exact_x && exact_y && equal_json(x,y,work,e);
    }
    cJSON_Delete(x);cJSON_Delete(y);
    return ok || fail(e,"conflicting or not losslessly comparable CSL snapshots for one source identity");
}
static bool incoming_source(Citation **sorted,size_t count,const char *id,const char *source,size_t *work,Error *e) {
    size_t lo=0,hi=count;
    while (lo<hi) { size_t mid=lo+(hi-lo)/2;if (strcmp(sorted[mid]->id,id)<0) lo=mid+1;else hi=mid; }
    for (size_t i=lo;i<count && !strcmp(sorted[i]->id,id);i++) {
        if (++*work>8000000) return fail(e,"bibliography source comparison work limit exceeded");
        if (sorted[i]->inserted && sorted[i]->identity && !strcmp(sorted[i]->identity,source)) return true;
    }
    return false;
}
static bool derived_section(const cJSON *section,Citation **sorted,size_t count,size_t *work,Error *e) {
    static const char *const node_keys[]={"type","attrs"};
    static const char *const attr_keys[]={"style","entriesJson","cslRenderJson","bibliographyMode"};
    static const char *const entry_keys[]={"citationId","number","entryHtml","key","itemUri","pdfUri","formatting","sourceItemId"};
    const cJSON *attrs=get(section,"attrs");
    if (!only_keys(section,node_keys,2) || !cJSON_IsObject(attrs) || !only_keys(attrs,attr_keys,4))
        return fail(e,"derived bibliography contains custom metadata; typed paste retained the original document");
    const cJSON *render=get(attrs,"cslRenderJson");
    if (!cJSON_IsString(get(attrs,"style")) || !cJSON_IsString(get(attrs,"entriesJson")) || (render && !cJSON_IsString(render)))
        return fail(e,"derived bibliography has unsupported attribute types");
    cJSON *entries=NULL;const char *json=string(attrs,"entriesJson");
    if (!ce_csl_json_parse_strict(json,strlen(json),&entries,e->error,e->capacity)) { e->failed=true;return false; }
    bool ok=cJSON_IsArray(entries);
    for (const cJSON *entry=entries?entries->child:NULL;ok && entry;entry=entry->next) {
        size_t number=0;
        if (!cJSON_IsObject(entry) || !only_keys(entry,entry_keys,8) || strcmp(string(entry,"formatting"),"csl") ||
            !*string(entry,"citationId") || !*string(entry,"sourceItemId") || !cJSON_IsString(get(entry,"entryHtml")) ||
            !natural(get(entry,"number"),50000,&number) || !number) { ok=false;break; }
        const char *optional[]={"key","itemUri","pdfUri"};
        for (size_t i=0;i<3;i++) if (get(entry,optional[i]) && !cJSON_IsString(get(entry,optional[i]))) ok=false;
        if (ok) ok=incoming_source(sorted,count,string(entry,"citationId"),string(entry,"sourceItemId"),work,e);
    }
    cJSON_Delete(entries);
    return ok || fail(e,"derived bibliography has custom metadata or a source absent from the copied slice");
}
static bool prune_incoming_bibliography(cJSON *node,Citation **sorted,size_t count,size_t *work,Error *e) {
    if (type_is(node,"zoteroCitationBlock")) return true;
    cJSON *content=cJSON_GetObjectItemCaseSensitive(node,"content");
    for (cJSON *child=content?content->child:NULL;child;) {
        cJSON *next=child->next;
        if (type_is(child,"zoteroEndnoteSection")) {
            const cJSON *mode=get(get(child,"attrs"),"bibliographyMode");
            if (mode && (!cJSON_IsString(mode) || strcmp(mode->valuestring,"citations")))
                return fail(e,"unsupported bibliography mode in copied slice");
            if (mode) {
                if (!derived_section(child,sorted,count,work,e)) return false;
                cJSON_Delete(cJSON_DetachItemViaPointer(content,child));
            }
        } else if (!prune_incoming_bibliography(child,sorted,count,work,e)) return false;
        child=next;
    }
    return true;
}
static bool prepare_citations(cJSON *left,cJSON *right,cJSON *insert,Error *e) {
    Citations v={0};Citation **sorted=NULL;
    bool ok=collect(&v,left,false,e) && collect(&v,right,false,e) && collect(&v,insert,true,e);
    if (ok && v.bibliographies && !v.inserted_citations)
        ok=fail(e,"a bibliography alone cannot be pasted as a derived section; paste readable text instead");
    if (ok && v.count) {
        sorted=malloc(v.count*sizeof *sorted);
        if (!sorted) ok=fail(e,"out of memory sorting citation identities");
    }
    for (size_t i=0;ok && i<v.count;i++) sorted[i]=&v.data[i];
    if (ok && v.count>1) qsort(sorted,v.count,sizeof *sorted,id_order);
    if (ok && v.bibliographies) {
        for (size_t i=0;ok && i<v.count;i++) if (v.data[i].inserted) {
            v.data[i].identity=identity(v.data[i].node,e);ok=v.data[i].identity!=NULL;
        }
        size_t work=0;
        if (ok) ok=prune_incoming_bibliography(insert,sorted,v.count,&work,e);
    }
    for (size_t i=0;ok && i<v.count;) {
        size_t end=i+1;while (end<v.count && !strcmp(sorted[i]->id,sorted[end]->id)) ++end;
        bool original=false;for (size_t j=i;j<end;j++) original|=!sorted[j]->inserted;
        bool kept=original;
        for (size_t j=i;j<end;j++) if (sorted[j]->inserted && *sorted[j]->id) {
            if (kept) sorted[j]->repair=true;else kept=true;
        }
        i=end;
    }
    size_t sequence=0;
    for (size_t i=0;ok && i<v.count;i++) {
        Citation *r=&v.data[i];
        if (r->repair) {
            char id[64];const char *key=id;
            do { snprintf(id,sizeof id,"paste-c-%zu",++sequence); }
            while (bsearch(&key,sorted,v.count,sizeof *sorted,id_key));
            cJSON *attrs=cJSON_GetObjectItemCaseSensitive(r->node,"attrs");
            if (!attrs) {
                if (!attach(r->node,"attrs",cJSON_CreateObject(),e)) { ok=false;break; }
                attrs=cJSON_GetObjectItemCaseSensitive(r->node,"attrs");
            }
            ok=set(attrs,"citationId",cJSON_CreateString(id),e);
        }
        if (ok) { free(r->identity);r->identity=identity(r->node,e);ok=r->identity!=NULL; }
    }
    if (ok && v.count>1) qsort(sorted,v.count,sizeof *sorted,identity_order);
    size_t work=0;
    for (size_t i=0;ok && i<v.count;) {
        size_t end=i+1;while (end<v.count && !strcmp(sorted[i]->identity,sorted[end]->identity)) ++end;
        const char *snapshot=NULL;
        for (size_t j=i;ok && j<end;j++) {
            const cJSON *attrs=get(sorted[j]->node,"attrs"),*field=get(attrs,"cslJson");
            if (field && !cJSON_IsString(field) && !cJSON_IsNull(field)) { ok=fail(e,"cslJson must be a string");break; }
            const char *current=string(attrs,"cslJson");
            if (*current) { if (snapshot) ok=snapshots_equal(snapshot,current,&work,e);else snapshot=current; }
        }
        i=end;
    }
    free(sorted);citations_free(&v);return ok;
}
static bool locate_position(const cJSON *n,const Position *position,size_t *index,size_t *found,Error *e) {
    if (leaf(n)) {
        if (n==position->node) { *found=*index;return true; }
        ++*index;return false;
    }
    for (const cJSON *c=get(n,"content")?get(n,"content")->child:NULL;c;c=c->next)
        if (locate_position(c,position,index,found,e)) return true;
    (void)e;return false;
}
bool ce_document_slice_prepare_replace(const CeDocument *document,size_t from,size_t to,
    const CeDocumentSlice *slice,CeDocument **out,CeDocumentSliceAnchor *anchor,char *error,size_t capacity) {
    Error e={error,capacity,false};if (error && capacity) *error=0;
    if (!out || *out==document || !bounds(document,from,to,&e)) return fail(&e,"candidate output must not alias the source document");
    Tree original={0};cJSON *left=NULL,*right=NULL,*insert=NULL;CeDocument *result=NULL;
    Position position={0};CeDocumentSliceAnchor resolved={0};size_t work=0;
    bool ok=tree_load(document,&original,NULL,&e);
    if (ok && from==to && (!slice || !get(slice->root,"content")->child)) {
        result=import_tree(original.root,&e);
        if (result) {
            size_t index=block_at(&original,from);CeBlockInfo block;
            if (ce_document_block_info(result,index,&block)) resolved=(CeDocumentSliceAnchor){from,index,from-block.start};
        }
        tree_free(&original);
        if (!result) return fail(&e,"unable to prepare an unchanged document");
        ce_document_free(*out);*out=result;if (anchor) *anchor=resolved;return true;
    }
    if (ok) { left=clip(&original,0,from,&e);right=clip(&original,to,ce_document_length(document),&e);ok=left && right; }
    if (ok) {
        cJSON *content=slice?(cJSON *)get(slice->root,"content"):cJSON_CreateArray();
        insert=content?document_root(content,&e):NULL;
        if (!slice) cJSON_Delete(content);
        ok=insert!=NULL;
    }
    if (ok) ok=prepare_citations(left,right,insert,&e);
    size_t start=slice?slice->open_start:0,end=slice?slice->open_end:0;
    bool nonempty=ok && get(insert,"content")->child!=NULL;
    if (ok && nonempty) ok=unwrap_inline(insert,&start,&end,&e) && wrap_context(left,insert,&start,&end,&e);
    if (ok) {
        if (nonempty) {
            position.node=edge_leaf(insert,true);
            position.offset=position.node?(paragraph(position.node)?inline_length(position.node):3):0;
            /* Closed blocks at the start/end of a plain paragraph do not add
             * synthetic blank paragraphs. Metadata-bearing empty nodes stay. */
            if (!start || (trivial_empty(edge(left,true)) && !paragraph(edge(insert,false)))) trim_empty(left,true,&position);
            if (!end || (trivial_empty(edge(right,false)) && !paragraph(edge(insert,true)))) trim_empty(right,false,&position);
            ok=merge_content(cJSON_GetObjectItemCaseSensitive(left,"content"),cJSON_GetObjectItemCaseSensitive(insert,"content"),start,&position,&work,&e);
        } else {
            if (edge(left,false)==edge(left,true)) trim_empty(left,true,&position);
            if (edge(right,false)==edge(right,true)) trim_empty(right,false,&position);
            position.node=edge_leaf(left,true);
            position.offset=position.node?(paragraph(position.node)?inline_length(position.node):3):0;
            end=open_depth(left,true);
            size_t other=open_depth(right,false);
            if (end && other && (end!=other || !can_join(edge(left,true),edge(right,false),end,&work,&e)))
                ok=fail(&e,"replacement crosses incompatible list or metadata boundaries");
            if (end>other) end=other;
        }
        if (ok) ok=merge_content(cJSON_GetObjectItemCaseSensitive(left,"content"),cJSON_GetObjectItemCaseSensitive(right,"content"),end,&position,&work,&e);
    }
    if (ok) {
        result=import_tree(left,&e);ok=result!=NULL;
        if (ok) {
            size_t index=0,found=0;CeBlockInfo block;
            if (position.node && locate_position(left,&position,&index,&found,&e) && ce_document_block_info(result,found,&block)) {
                resolved.block_index=found;resolved.block_offset=position.offset;
                if (position.offset>block.end-block.start) ok=fail(&e,"paste anchor exceeds its block");
                resolved.offset=block.start+position.offset;
            } else if (ce_document_block_count(result)) {
                ce_document_block_info(result,0,&block);resolved.offset=block.start;
            }
        }
    }
    tree_free(&original);cJSON_Delete(left);cJSON_Delete(right);cJSON_Delete(insert);
    if (!ok) { ce_document_free(result);return fail(&e,"unable to prepare pasted document"); }
    ce_document_free(*out);*out=result;if (anchor) *anchor=resolved;return true;
}



