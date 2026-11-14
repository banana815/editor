#include "csl_input.h"
#include "csl_unicode.h"
#include <inttypes.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct Numeric {
    const cJSON *node;
    int64_t integer;
    bool exact;
} Numeric;
typedef struct Reader {
    const char *source;
    size_t size, at, nodes, number_count, number_capacity;
    Numeric *numbers;
} Reader;
struct CeCslInput {
    cJSON *root;
    CeCslInputItem *items;
    CeCslInputItem **sorted;
    CeCslInputCluster *clusters;
    size_t item_count, cluster_count;
};
static bool fail(char *error,size_t capacity,const char *message) {
    if(error && capacity) snprintf(error,capacity,"%s",message);
    return false;
}
static void whitespace(Reader *r) {
    while(r->at<r->size && strchr(" \t\r\n",r->source[r->at])) ++r->at;
}
static bool digit(char c) { return c>='0' && c<='9'; }
static int hex(char c) {
    if(digit(c)) return c-'0';
    if(c>='a' && c<='f') return c-'a'+10;
    if(c>='A' && c<='F') return c-'A'+10;
    return -1;
}
static bool hex4(Reader *r,unsigned *value) {
    if(r->size-r->at<4) return false;
    *value=0;
    for(unsigned i=0;i<4;i++) {
        int h=hex(r->source[r->at++]); if(h<0) return false;
        *value=(*value<<4)|(unsigned)h;
    }
    return true;
}
static bool string(Reader *r) {
    if(r->at==r->size || r->source[r->at++]!='"') return false;
    while(r->at<r->size) {
        unsigned char c=(unsigned char)r->source[r->at++];
        if(c=='"') return true;
        if(c<0x20) return false;
        if(c!='\\') continue;
        if(r->at==r->size) return false;
        c=(unsigned char)r->source[r->at++];
        if(strchr("\"\\/bfnrt",c)) continue;
        unsigned u,v;
        if(c!='u' || !hex4(r,&u) || u==0) return false;
        if(u>=0xdc00 && u<=0xdfff) return false;
        if(u>=0xd800 && u<=0xdbff) {
            if(r->size-r->at<6 || r->source[r->at++]!='\\' || r->source[r->at++]!='u' ||
               !hex4(r,&v) || v<0xdc00 || v>0xdfff) return false;
        }
    }
    return false;
}
/* Parse exact decimal integers without relying on double rounding. A token has
 * already passed JSON's number grammar. Saturating exponents are sufficient
 * because input length is bounded and accepted identities have <=16 digits. */
static bool exact_integer(const char *s,size_t n,int64_t *out) {
    bool negative=*s=='-'; size_t start=negative?1:0, end=start;
    while(end<n && s[end]!='e' && s[end]!='E') ++end;
    int64_t exponent=0;
    if(end<n) {
        size_t p=end+1; bool neg=p<n && s[p]=='-';
        if(p<n && (s[p]=='-' || s[p]=='+')) ++p;
        for(;p<n;p++) if(exponent<100000000) exponent=exponent*10+(s[p]-'0');
        if(neg) exponent=-exponent;
    }
    size_t first=SIZE_MAX,last=SIZE_MAX, digits_after=0, significant=0;
    bool fraction=false;
    for(size_t i=start;i<end;i++) {
        if(s[i]=='.') { fraction=true; continue; }
        if(fraction) ++digits_after;
        if(s[i]!='0') { if(first==SIZE_MAX) first=i; last=i; }
    }
    if(first==SIZE_MAX) { *out=0; return true; }
    size_t trailing=0;
    for(size_t i=last+1;i<end;i++) if(s[i]!='.') ++trailing;
    int64_t shift=exponent-(int64_t)digits_after+(int64_t)trailing;
    if(shift<0 || shift>16) return false;
    uint64_t value=0,limit=UINT64_C(9007199254740991);
    for(size_t i=first;i<=last;i++) if(s[i]!='.') {
        if(++significant>16 || value>(limit-(unsigned)(s[i]-'0'))/10) return false;
        value=value*10+(unsigned)(s[i]-'0');
    }
    for(int64_t i=0;i<shift;i++) { if(value>limit/10) return false; value*=10; }
    *out=negative?-(int64_t)value:(int64_t)value; return true;
}
static bool number(Reader *r) {
    size_t start=r->at;
    if(r->at<r->size && r->source[r->at]=='-') ++r->at;
    if(r->at==r->size || !digit(r->source[r->at])) return false;
    if(r->source[r->at]=='0') ++r->at;
    else while(r->at<r->size && digit(r->source[r->at])) ++r->at;
    if(r->at<r->size && r->source[r->at]=='.') {
        ++r->at; size_t first=r->at;
        while(r->at<r->size && digit(r->source[r->at])) ++r->at;
        if(r->at==first) return false;
    }
    if(r->at<r->size && (r->source[r->at]=='e' || r->source[r->at]=='E')) {
        ++r->at;
        if(r->at<r->size && (r->source[r->at]=='+' || r->source[r->at]=='-')) ++r->at;
        size_t first=r->at;
        while(r->at<r->size && digit(r->source[r->at])) ++r->at;
        if(r->at==first) return false;
    }
    if(r->number_count==r->number_capacity) {
        size_t capacity=r->number_capacity?r->number_capacity*2:32;
        Numeric *p=realloc(r->numbers,capacity*sizeof *p); if(!p) return false;
        r->numbers=p; r->number_capacity=capacity;
    }
    Numeric *v=&r->numbers[r->number_count++]; memset(v,0,sizeof *v);
    v->exact=exact_integer(r->source+start,r->at-start,&v->integer);
    return true;
}
static bool value(Reader *r,unsigned depth) {
    whitespace(r);
    if(r->at==r->size || ++r->nodes>CE_CSL_INPUT_MAX_NODES) return false;
    char c=r->source[r->at];
    if(c=='"') return string(r);
    if(c=='{' || c=='[') {
        if(depth>=128) return false;
        ++r->at; whitespace(r); char close=c=='{'?'}':']';
        if(r->at<r->size && r->source[r->at]==close) { ++r->at; return true; }
        for(;;) {
            if(c=='{') {
                if(!string(r)) return false;
                whitespace(r); if(r->at==r->size || r->source[r->at++]!=':') return false;
            }
            if(!value(r,depth+1)) return false;
            whitespace(r); if(r->at==r->size) return false;
            char next=r->source[r->at++]; if(next==close) return true;
            if(next!=',') return false;
            whitespace(r);
        }
    }
    const char *literal=c=='t'?"true":c=='f'?"false":c=='n'?"null":NULL;
    if(literal) {
        size_t len=strlen(literal);
        if(r->size-r->at<len || memcmp(r->source+r->at,literal,len)) return false;
        r->at+=len; return true;
    }
    return number(r);
}
static int member_compare(const void *a,const void *b) {
    const cJSON *const *x=a,*const *y=b; return strcmp((*x)->string,(*y)->string);
}
static bool verify_tree(const cJSON *node,Reader *r,size_t *number_index) {
    if(cJSON_IsNumber(node)) {
        if(!isfinite(node->valuedouble) || *number_index>=r->number_count) return false;
        r->numbers[(*number_index)++].node=node;
    }
    if(cJSON_IsObject(node)) {
        size_t count=0; for(const cJSON *c=node->child;c;c=c->next) ++count;
        if(count>1) {
            const cJSON **keys=malloc(count*sizeof *keys); if(!keys) return false;
            size_t i=0; for(const cJSON *c=node->child;c;c=c->next) keys[i++]=c;
            qsort(keys,count,sizeof *keys,member_compare);
            bool unique=true;
            for(i=1;i<count;i++) if(!strcmp(keys[i-1]->string,keys[i]->string)) { unique=false; break; }
            free(keys); if(!unique) return false;
        }
    }
    for(const cJSON *c=node->child;c;c=c->next) if(!verify_tree(c,r,number_index)) return false;
    return true;
}
static int numeric_compare(const void *a,const void *b) {
    uintptr_t x=(uintptr_t)((const Numeric *)a)->node,y=(uintptr_t)((const Numeric *)b)->node;
    return (x>y)-(x<y);
}
static const Numeric *numeric(const Reader *r,const cJSON *node) {
    Numeric key={0}; key.node=node;
    return r->number_count?bsearch(&key,r->numbers,r->number_count,sizeof key,numeric_compare):NULL;
}
static bool identity_string(const char *id) {
    if(!id || !*id || strlen(id)>4096) return false;
    for(const unsigned char *p=(const unsigned char *)id;*p;p++) if(*p<0x20 || *p==0x7f) return false;
    return true;
}
static char *identity(const Reader *r,const cJSON *node) {
    char buffer[32]; const char *id=NULL;
    if(cJSON_IsString(node)) id=node->valuestring;
    else if(cJSON_IsNumber(node)) {
        const Numeric *v=numeric(r,node); if(!v || !v->exact) return NULL;
        snprintf(buffer,sizeof buffer,"%" PRId64,v->integer); id=buffer;
    }
    if(!id || !identity_string(id)) return NULL;
    size_t n=strlen(id)+1; char *copy=malloc(n); if(copy) memcpy(copy,id,n); return copy;
}
static int item_compare(const void *a,const void *b) {
    const CeCslInputItem *const *x=a,*const *y=b; return strcmp((*x)->id,(*y)->id);
}
static int cluster_compare(const void *a,const void *b) {
    const CeCslInputCluster *const *x=a,*const *y=b; return strcmp((*x)->id,(*y)->id);
}
static const cJSON *get(const cJSON *object,const char *name) { return cJSON_GetObjectItemCaseSensitive(object,name); }
static bool optional_types(const cJSON *object,const char *const *keys,size_t count,bool strings) {
    for(size_t i=0;i<count;i++) { const cJSON *v=get(object,keys[i]); if(v && !(strings?cJSON_IsString(v):cJSON_IsBool(v))) return false; }
    return true;
}
static bool prepare(CeCslInput *input,Reader *r) {
    const cJSON *items=get(input->root,"items"),*citations=get(input->root,"citations");
    if(!cJSON_IsObject(input->root) || !cJSON_IsArray(items) || !cJSON_IsArray(citations)) return false;
    input->item_count=(size_t)cJSON_GetArraySize(items); input->cluster_count=(size_t)cJSON_GetArraySize(citations);
    if(input->item_count>CE_CSL_INPUT_MAX_ITEMS || input->cluster_count>CE_CSL_INPUT_MAX_CLUSTERS) return false;
    input->items=calloc(input->item_count?input->item_count:1,sizeof *input->items);
    input->sorted=calloc(input->item_count?input->item_count:1,sizeof *input->sorted);
    input->clusters=calloc(input->cluster_count?input->cluster_count:1,sizeof *input->clusters);
    if(!input->items || !input->sorted || !input->clusters) return false;
    size_t i=0;
    for(const cJSON *item=items->child;item;item=item->next,i++) {
        if(!cJSON_IsObject(item)) return false;
        CeCslInputItem *v=&input->items[i]; v->data=item; v->id=identity(r,get(item,"id"));
        if(!v->id) return false;
        v->first_citation=v->first_position=SIZE_MAX; input->sorted[i]=v;
    }
    qsort(input->sorted,input->item_count,sizeof *input->sorted,item_compare);
    for(i=1;i<input->item_count;i++) if(!strcmp(input->sorted[i-1]->id,input->sorted[i]->id)) return false;
    size_t total=0; i=0;
    const char *const string_keys[]={"locator","label","prefix","suffix"};
    const char *const bool_keys[]={"suppress-author","author-only"};
    for(const cJSON *cluster=citations->child;cluster;cluster=cluster->next,i++) {
        CeCslInputCluster *v=&input->clusters[i]; v->data=cluster;
        const cJSON *id=get(cluster,"citationID"),*cites=get(cluster,"citationItems"),*properties=get(cluster,"properties");
        if(!cJSON_IsObject(cluster) || !cJSON_IsString(id) || !identity_string(id->valuestring) || !cJSON_IsArray(cites)) return false;
        v->id=id->valuestring; v->item_count=(size_t)cJSON_GetArraySize(cites);
        if(!v->item_count || v->item_count>CE_CSL_INPUT_MAX_CITES-total) return false;
        total+=v->item_count;
        if(properties) {
            if(!cJSON_IsObject(properties)) return false;
            const cJSON *note=get(properties,"noteIndex");
            if(note) {
                const Numeric *n=numeric(r,note);
                if(!cJSON_IsNumber(note) || !n || !n->exact || n->integer<0 || (uint64_t)n->integer>UINT32_MAX) return false;
                v->note_index=(uint32_t)n->integer;
            }
        }
        CeCslInputCite *references=calloc(v->item_count,sizeof *references); if(!references) return false;
        v->items=references; size_t j=0;
        for(const cJSON *cite=cites->child;cite;cite=cite->next,j++) {
            if(!cJSON_IsObject(cite) || !optional_types(cite,string_keys,4,true) || !optional_types(cite,bool_keys,2,false) ||
               (cJSON_IsTrue(get(cite,"suppress-author")) && cJSON_IsTrue(get(cite,"author-only")))) return false;
            char *item_id=identity(r,get(cite,"id")); if(!item_id) return false;
            size_t index=ce_csl_input_find(input,item_id); free(item_id); if(index==SIZE_MAX) return false;
            references[j].item_index=index; references[j].data=cite;
            CeCslInputItem *item=&input->items[index];
            if(item->first_citation==SIZE_MAX) { item->first_citation=i; item->first_position=j; }
        }
    }
    if(input->cluster_count>1) {
        const CeCslInputCluster **sorted=malloc(input->cluster_count*sizeof *sorted); if(!sorted) return false;
        for(i=0;i<input->cluster_count;i++) sorted[i]=&input->clusters[i];
        qsort(sorted,input->cluster_count,sizeof *sorted,cluster_compare);
        bool unique=true;
        for(i=1;i<input->cluster_count;i++) if(!strcmp(sorted[i-1]->id,sorted[i]->id)) { unique=false; break; }
        free(sorted); if(!unique) return false;
    }
    return true;
}
bool ce_csl_input_load(CeCslInput **slot,const char *json,size_t size,char *error,size_t capacity) {
    if(error && capacity) *error=0;
    if(!slot || !json || !size || size>CE_CSL_INPUT_MAX_BYTES || memchr(json,0,size) ||
       ce_csl_unicode_validate(json,size,NULL)!=CE_CSL_UNICODE_OK)
        return fail(error,capacity,"Invalid or oversized CSL JSON input");
    Reader r={0}; r.source=json; r.size=size;
    bool valid=value(&r,0); whitespace(&r);
    if(!valid || r.at!=size) { free(r.numbers); return fail(error,capacity,"Malformed, excessive or unallocatable CSL JSON"); }
    CeCslInput *input=calloc(1,sizeof *input);
    if(!input) { free(r.numbers); return fail(error,capacity,"Out of memory loading CSL input"); }
    const char *end=NULL; input->root=cJSON_ParseWithLengthOpts(json,size,&end,0);
    size_t number_index=0;
    valid=input->root && verify_tree(input->root,&r,&number_index) && number_index==r.number_count;
    if(valid && r.number_count>1) qsort(r.numbers,r.number_count,sizeof *r.numbers,numeric_compare);
    valid=valid && prepare(input,&r); free(r.numbers);
    if(!valid) { ce_csl_input_free(input); return fail(error,capacity,"Invalid CSL identities, references, duplicate keys, values or allocation failure"); }
    CeCslInput *old=*slot; *slot=input; ce_csl_input_free(old); return true;
}
void ce_csl_input_free(CeCslInput *input) {
    if(!input) return;
    if(input->items) for(size_t i=0;i<input->item_count;i++) free((void *)input->items[i].id);
    if(input->clusters) for(size_t i=0;i<input->cluster_count;i++) free((void *)input->clusters[i].items);
    free(input->items); free(input->sorted); free(input->clusters); cJSON_Delete(input->root); free(input);
}
size_t ce_csl_input_item_count(const CeCslInput *input) { return input?input->item_count:0; }
size_t ce_csl_input_cluster_count(const CeCslInput *input) { return input?input->cluster_count:0; }
const CeCslInputItem *ce_csl_input_item(const CeCslInput *input,size_t index) { return input && index<input->item_count?&input->items[index]:NULL; }
const CeCslInputCluster *ce_csl_input_cluster(const CeCslInput *input,size_t index) { return input && index<input->cluster_count?&input->clusters[index]:NULL; }
size_t ce_csl_input_find(const CeCslInput *input,const char *id) {
    if(!input || !id || !input->item_count) return SIZE_MAX;
    CeCslInputItem key={0}; key.id=id; CeCslInputItem *pointer=&key;
    CeCslInputItem **found=bsearch(&pointer,input->sorted,input->item_count,sizeof *input->sorted,item_compare);
    return found?(size_t)(*found-input->items):SIZE_MAX;
}
const cJSON *ce_csl_input_data(const CeCslInput *input) { return input?input->root:NULL; }
