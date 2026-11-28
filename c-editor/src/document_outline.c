#include "document_outline.h"
#include "cJSON.h"
#include "csl_json.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static bool problem(char *error,size_t capacity,const char *message) {
    if(error && capacity) snprintf(error,capacity,"%s",message);
    return false;
}
void ce_outline_free(CeOutline *outline) {
    if(outline) { free(outline->entries); *outline=(CeOutline){0}; }
}
bool ce_outline_build(const CeDocument *doc,CeOutline *out) {
    if(!doc || !out) return false;
    size_t count=0; CeBlockInfo info;
    for(size_t i=0;i<ce_document_block_count(doc);i++)
        if(ce_document_block_info(doc,i,&info) && info.kind==CE_BLOCK_HEADING) count++;
    CeOutline next={0};
    if(count) {
        if(count>SIZE_MAX/sizeof *next.entries) return false;
        next.entries=calloc(count,sizeof *next.entries);
        if(!next.entries) return false;
    }
    size_t stack[6],depth=0;
    for(size_t i=0;i<ce_document_block_count(doc);i++) {
        if(!ce_document_block_info(doc,i,&info) || info.kind!=CE_BLOCK_HEADING) continue;
        unsigned level=info.level<1?1:info.level>6?6:info.level;
        while(depth && next.entries[stack[depth-1]].level>=level) {
            next.entries[stack[--depth]].section_end=info.start;
        }
        CeOutlineEntry *entry=&next.entries[next.count];
        *entry=(CeOutlineEntry){i,info.start,info.end,ce_document_length(doc),
            depth?stack[depth-1]:SIZE_MAX,level,UINT64_C(14695981039346656037),false};
        char *json=ce_document_block_json(doc,i);
        if(!json) { ce_outline_free(&next); return false; }
        for(const unsigned char *p=(const unsigned char *)json;*p;p++) {
            entry->key^=*p; entry->key*=UINT64_C(1099511628211);
        }
        free(json);
        if(depth) next.entries[stack[depth-1]].children=true;
        stack[depth++]=next.count++;
    }
    ce_outline_free(out); *out=next; return true;
}

typedef struct { cJSON *node,*array; } MoveRef;
typedef struct {
    size_t block,from,to,work;
    MoveRef source,target;
    cJSON *track;
    size_t tracked_block;
} Walk;
static const char *type(const cJSON *node) {
    const cJSON *v=cJSON_GetObjectItemCaseSensitive(node,"type");
    return cJSON_IsString(v)?v->valuestring:"";
}
static cJSON *content(cJSON *node) { return cJSON_GetObjectItemCaseSensitive(node,"content"); }
static bool is_type(const cJSON *node,const char *name) { return !strcmp(type(node),name); }
static bool leaf(const cJSON *node) {
    const char *t=type(node);
    return !strcmp(t,"paragraph") || !strcmp(t,"heading") || !strcmp(t,"codeBlock") ||
        !strcmp(t,"horizontalRule") || !strcmp(t,"image") || !strcmp(t,"zoteroCitationBlock") ||
        !strcmp(t,"zoteroEndnoteSection") ||
        (!cJSON_GetObjectItemCaseSensitive(node,"content") && strcmp(t,"doc"));
}
static bool walk(cJSON *node,cJSON *array,cJSON *parent,cJSON *parent_array,Walk *w,unsigned depth) {
    if(depth>128 || ++w->work>250000) return false;
    if(leaf(node)) {
        if(node==w->track) w->tracked_block=w->block;
        if(is_type(node,"heading") && (w->block==w->from || w->block==w->to)) {
            MoveRef ref={node,array};
            if(parent && is_type(parent,"listItem") && array && array->child==node && parent_array)
                ref=(MoveRef){parent,parent_array};
            if(w->block==w->from) w->source=ref;
            if(w->block==w->to) w->target=ref;
        }
        w->block++; return true;
    }
    cJSON *children=content(node),*child;
    cJSON_ArrayForEach(child,children)
        if(!walk(child,children,node,array,w,depth+1)) return false;
    return true;
}
static cJSON *heading(cJSON *node) {
    if(is_type(node,"heading")) return node;
    if(is_type(node,"listItem")) {
        cJSON *first=cJSON_GetArrayItem(content(node),0);
        if(is_type(first,"heading")) return first;
    }
    return NULL;
}
static unsigned level(cJSON *node) {
    cJSON *h=heading(node),*attrs=cJSON_GetObjectItemCaseSensitive(h,"attrs");
    cJSON *value=cJSON_GetObjectItemCaseSensitive(attrs,"level");
    return cJSON_IsNumber(value) && value->valueint>=1 && value->valueint<=6?(unsigned)value->valueint:1;
}
static size_t index_of(cJSON *array,cJSON *node) {
    size_t i=0; cJSON *child;
    cJSON_ArrayForEach(child,array) { if(child==node) return i; i++; }
    return SIZE_MAX;
}
static size_t section_end(cJSON *node,size_t start) {
    unsigned current=level(node); size_t end=start+1;
    for(cJSON *next=node->next;next;next=next->next,end++)
        if(heading(next) && level(next)<=current) break;
    return end;
}
bool ce_outline_move(CeDocument *doc,size_t from_block,size_t to_block,bool after,
                     size_t *new_offset,char *error,size_t capacity) {
    if(!doc) return problem(error,capacity,"没有可移动的文章");
    char *source=ce_document_export_json(doc); cJSON *tree=NULL;
    if(!source) return problem(error,capacity,"内存不足，章节保持不变");
    bool ok=ce_csl_json_parse_strict(source,strlen(source),&tree,error,capacity);
    free(source); if(!ok) return false;
    Walk w={0}; w.from=from_block; w.to=to_block;
    ok=walk(tree,NULL,NULL,NULL,&w,0);
    if(!ok || !w.source.node || !w.target.node) {
        cJSON_Delete(tree); return problem(error,capacity,"章节位置已经改变，请重新拖动");
    }
    if(w.source.array!=w.target.array) {
        cJSON_Delete(tree); return problem(error,capacity,"请在同一层容器内移动章节；列表和引用的外壳会完整保留");
    }
    cJSON *array=w.source.array;
    size_t first=index_of(array,w.source.node),last=section_end(w.source.node,first);
    size_t target=index_of(array,w.target.node),insert=after?section_end(w.target.node,target):target;
    CeBlockInfo original;
    if(from_block==to_block) {
        ok=ce_document_block_info(doc,from_block,&original);
        if(ok && new_offset) *new_offset=original.start;
        cJSON_Delete(tree); return ok;
    }
    if((target>=first && target<last) || (insert>first && insert<last)) {
        cJSON_Delete(tree); return problem(error,capacity,"不能把章节移入它自己的子章节");
    }
    if(insert==first || insert==last) {
        ok=ce_document_block_info(doc,from_block,&original);
        if(ok && new_offset) *new_offset=original.start;
        cJSON_Delete(tree); return ok;
    }
    size_t count=last-first;
    if(count>SIZE_MAX/sizeof(cJSON *)) { cJSON_Delete(tree); return problem(error,capacity,"章节过大"); }
    cJSON **moving=malloc(count*sizeof *moving);
    if(!moving) { cJSON_Delete(tree); return problem(error,capacity,"内存不足，章节保持不变"); }
    cJSON *tracked=heading(w.source.node);
    for(size_t i=0;i<count;i++) moving[i]=cJSON_DetachItemFromArray(array,(int)first);
    if(insert>last) insert-=count;
    for(size_t i=0;i<count;i++) {
        if(!cJSON_InsertItemInArray(array,(int)(insert+i),moving[i])) {
            for(size_t j=i;j<count;j++) cJSON_Delete(moving[j]);
            free(moving); cJSON_Delete(tree); return problem(error,capacity,"内存不足，章节保持不变");
        }
    }
    free(moving);
    Walk final={0}; final.from=final.to=SIZE_MAX; final.track=tracked; final.tracked_block=SIZE_MAX;
    ok=walk(tree,NULL,NULL,NULL,&final,0) && final.tracked_block!=SIZE_MAX;
    char *json=ok?ce_csl_json_print_exact_with_limit(tree,16u*1024u*1024u):NULL; cJSON_Delete(tree);
    if(!json) return problem(error,capacity,"内存不足，章节保持不变");
    ok=ce_document_apply_json(doc,json); cJSON_free(json);
    if(!ok) return problem(error,capacity,ce_document_error(doc));
    if(new_offset && ce_document_block_info(doc,final.tracked_block,&original)) *new_offset=original.start;
    return true;
}
