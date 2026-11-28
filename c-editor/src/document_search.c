#include "document_search.h"
#include "cJSON.h"
#include "csl_json.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct Match { size_t start, end; } Match;
typedef struct Matches { Match *items; size_t count, capacity; bool failed; } Matches;
static bool problem(char *error,size_t capacity,const char *message) {
    if(error && capacity) snprintf(error,capacity,"%s",message);
    return false;
}
static unsigned char fold(unsigned char c) { return c>='A' && c<='Z' ? (unsigned char)(c+32) : c; }
static bool text_valid(const char *s,bool empty) {
    if(!s || (!empty && !*s)) return false;
    const unsigned char *p=(const unsigned char *)s;
    while(*p) {
        uint32_t c=*p++; unsigned n=0;
        if(c<0x80) { if(c=='\r' || c=='\n') return false; continue; }
        if(c>=0xc2 && c<=0xdf) { c&=31; n=1; }
        else if(c>=0xe0 && c<=0xef) { c&=15; n=2; }
        else if(c>=0xf0 && c<=0xf4) { c&=7; n=3; }
        else return false;
        for(unsigned i=0;i<n;i++) { if((*p&0xc0)!=0x80) return false; c=(c<<6)|(*p++&63); }
        if((n==1 && c<0x80) || (n==2 && c<0x800) || (n==3 && c<0x10000) ||
           c>0x10ffff || (c>=0xd800 && c<=0xdfff) || c==0xfffc || c==0x2028 || c==0x2029) return false;
    }
    return true;
}
bool ce_document_search_query_valid(const char *query) { return text_valid(query,false); }
static bool append_match(Matches *matches,size_t start,size_t end) {
    if(matches->count==matches->capacity) {
        size_t capacity=matches->capacity?matches->capacity*2:16;
        if(capacity<matches->capacity || capacity>SIZE_MAX/sizeof(Match)) return false;
        Match *items=realloc(matches->items,capacity*sizeof *items);
        if(!items) return false;
        matches->items=items; matches->capacity=capacity;
    }
    matches->items[matches->count++]=(Match){start,end}; return true;
}
/* KMP keeps scanning linear even for repeated text and long near-matches. */
static bool collect(const CeDocument *doc,const char *query,bool overlapping,Matches *matches) {
    size_t n=strlen(query);
    if(n>ce_document_length(doc)) return true;
    if(n>SIZE_MAX/sizeof(size_t)) return false;
    size_t *prefix=malloc(n*sizeof *prefix);
    if(!prefix) return false;
    prefix[0]=0;
    for(size_t i=1,k=0;i<n;i++) {
        while(k && fold((unsigned char)query[i])!=fold((unsigned char)query[k])) k=prefix[k-1];
        if(fold((unsigned char)query[i])==fold((unsigned char)query[k])) ++k;
        prefix[i]=k;
    }
    const char *text=ce_document_text(doc);
    for(size_t b=0;b<ce_document_block_count(doc);b++) {
        CeBlockInfo block;
        if(!ce_document_block_info(doc,b,&block) || block.kind==CE_BLOCK_CITATION ||
           block.kind==CE_BLOCK_ENDNOTES || block.kind==CE_BLOCK_UNKNOWN || block.kind==CE_BLOCK_IMAGE || block.kind==CE_BLOCK_RULE) continue;
        for(size_t i=block.start,k=0;i<block.end;i++) {
            while(k && fold((unsigned char)text[i])!=fold((unsigned char)query[k])) k=prefix[k-1];
            if(fold((unsigned char)text[i])==fold((unsigned char)query[k])) ++k;
            if(k==n) {
                if(!append_match(matches,i+1-n,i+1)) { free(prefix); return false; }
                k=overlapping?prefix[k-1]:0;
            }
        }
    }
    free(prefix); return true;
}
bool ce_document_search_next(const CeDocument *doc,const char *query,size_t from,
                             bool wrap,size_t *start,size_t *end) {
    if(!doc || !text_valid(query,false)) return false;
    /* No allocation is needed for article lookup. A validated single-line
     * query cannot match the LF separators or U+FFFC atomic placeholders. */
    size_t found_start,found_end;
    if(!ce_document_find(doc,query,from,false,&found_start,&found_end) || (!wrap && found_start<from)) return false;
    if(start) *start=found_start;
    if(end) *end=found_end;
    return true;
}

typedef struct Transform {
    const CeDocument *doc;
    const Match *matches;
    size_t count, match, block;
    const char *replacement;
    size_t replacement_length;
} Transform;
static const char *node_type(const cJSON *node) {
    const cJSON *type=cJSON_GetObjectItemCaseSensitive(node,"type");
    return cJSON_IsString(type)?type->valuestring:"";
}
static bool transform_text(Transform *t,cJSON *node,size_t start,size_t length) {
    while(t->match<t->count && t->matches[t->match].end<=start) ++t->match;
    size_t first=t->match,end=start+length,new_length=length;
    for(size_t i=first;i<t->count && t->matches[i].start<end;i++) {
        size_t left=t->matches[i].start>start?t->matches[i].start:start;
        size_t right=t->matches[i].end<end?t->matches[i].end:end;
        new_length-=right-left;
        if(t->matches[i].start>=start) {
            if(t->replacement_length>SIZE_MAX-new_length-1) return false;
            new_length+=t->replacement_length;
        }
    }
    if(first==t->count || t->matches[first].start>=end) return true;
    char *out=malloc(new_length+1); if(!out) return false;
    const cJSON *field=cJSON_GetObjectItemCaseSensitive(node,"text");
    const char *text=field->valuestring; size_t read=0,written=0;
    for(size_t i=first;i<t->count && t->matches[i].start<end;i++) {
        size_t left=t->matches[i].start>start?t->matches[i].start-start:0;
        size_t right=t->matches[i].end<end?t->matches[i].end-start:length;
        memcpy(out+written,text+read,left-read); written+=left-read;
        if(t->matches[i].start>=start) { memcpy(out+written,t->replacement,t->replacement_length); written+=t->replacement_length; }
        read=right;
    }
    memcpy(out+written,text+read,length-read); written+=length-read; out[written]=0;
    cJSON *value=cJSON_CreateString(out); free(out);
    if(!value) return false;
    if(!cJSON_ReplaceItemInObjectCaseSensitive(node,"text",value)) { cJSON_Delete(value); return false; }
    return true;
}
static bool transform_inline(Transform *t,cJSON *node,size_t *offset,unsigned depth) {
    if(depth>128) return false;
    const char *type=node_type(node);
    if(!strcmp(type,"text")) {
        const cJSON *field=cJSON_GetObjectItemCaseSensitive(node,"text");
        if(!cJSON_IsString(field)) return false;
        size_t length=strlen(field->valuestring),start=*offset; *offset+=length;
        return transform_text(t,node,start,length);
    }
    if(!strcmp(type,"hardBreak")) { ++*offset; return true; }
    cJSON *child; cJSON_ArrayForEach(child,cJSON_GetObjectItemCaseSensitive(node,"content"))
        if(!transform_inline(t,child,offset,depth+1)) return false;
    return true;
}
static bool transform_blocks(Transform *t,cJSON *node,unsigned depth) {
    if(depth>128) return false;
    const char *type=node_type(node);
    bool editable=!strcmp(type,"paragraph") || !strcmp(type,"heading") || !strcmp(type,"codeBlock");
    bool leaf=editable || !strcmp(type,"zoteroCitationBlock") || !strcmp(type,"zoteroEndnoteSection") ||
        !strcmp(type,"image") || !strcmp(type,"horizontalRule") ||
        (!cJSON_GetObjectItemCaseSensitive(node,"content") && strcmp(type,"doc"));
    if(leaf) {
        CeBlockInfo block; if(!ce_document_block_info(t->doc,t->block++,&block)) return false;
        size_t offset=block.start;
        return !editable || (transform_inline(t,node,&offset,0) && offset==block.end);
    }
    cJSON *child; cJSON_ArrayForEach(child,cJSON_GetObjectItemCaseSensitive(node,"content"))
        if(!transform_blocks(t,child,depth+1)) return false;
    return true;
}
bool ce_document_search_replace(CeDocument *doc,const char *query,const char *replacement,
    size_t from,bool all,size_t *count,size_t *cursor,char *error,size_t capacity) {
    if(!doc || !text_valid(query,false) || !text_valid(replacement,true))
        return problem(error,capacity,"请输入有效的单行查找文字；替换内容可留空");
    Matches matches={0};
    if(!collect(doc,query,!all,&matches)) { free(matches.items); return problem(error,capacity,"内存不足，原文保持不变"); }
    if(!matches.count) { free(matches.items); if(count) *count=0; if(cursor) *cursor=from; return true; }
    if(!all) {
        size_t selected=0;
        for(size_t i=0;i<matches.count;i++) if(matches.items[i].start>=from) { selected=i; break; }
        matches.items[0]=matches.items[selected]; matches.count=1;
    }
    char *json=ce_document_export_json(doc); cJSON *root=NULL;
    bool ok=json && ce_csl_json_parse_strict(json,strlen(json),&root,error,capacity); free(json);
    Transform transform={doc,matches.items,matches.count,0,0,replacement,strlen(replacement)};
    if(ok) ok=transform_blocks(&transform,root,0) && transform.block==ce_document_block_count(doc);
    char *result=ok?ce_csl_json_print_exact(root):NULL;
    cJSON_Delete(root);
    if(!result) { free(matches.items); return problem(error,capacity,"无法准备替换结果，原文保持不变"); }
    ok=ce_document_apply_json(doc,result); cJSON_free(result);
    if(ok) {
        size_t position=matches.items[matches.count-1].end;
        for(size_t i=0;i<matches.count;i++) position-=matches.items[i].end-matches.items[i].start;
        position+=matches.count*transform.replacement_length;
        if(count) *count=matches.count; if(cursor) *cursor=position;
    } else problem(error,capacity,ce_document_error(doc));
    free(matches.items); return ok;
}
