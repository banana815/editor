#include "csl_registry.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char error[256];
static const char fixture[] =
    "{\"items\":[{\"id\":\"unused\"},{\"id\":\"a\",\"citation-number\":999},"
    "{\"id\":\"b\",\"citation-number\":1},{\"id\":\"c\"},{\"id\":\"extra\"}],"
    "\"citations\":[{\"citationID\":\"one\",\"citationItems\":[{\"id\":\"b\"},"
    "{\"id\":\"b\"},{\"id\":\"a\"}],\"properties\":{\"noteIndex\":8}},"
    "{\"citationID\":\"two\",\"citationItems\":[{\"id\":\"c\"},{\"id\":\"a\"}],"
    "\"properties\":{\"noteIndex\":0}},"
    "{\"citationID\":\"three\",\"citationItems\":[{\"id\":\"b\"}],"
    "\"properties\":{\"noteIndex\":1}}]}";

static CeCslInput *load(const char *json) {
    CeCslInput *input=NULL;
    assert(ce_csl_input_load(&input,json,strlen(json),error,sizeof(error)));
    return input;
}

static CeCslRegistry *build(const CeCslInput *input,const CeCslRegistryOptions *options) {
    CeCslRegistry *registry=NULL;
    assert(ce_csl_registry_build(input,options,NULL,&registry,error,sizeof(error)));
    assert(!*error);
    return registry;
}

static void check(const CeCslRegistry *registry,const size_t *items,size_t count) {
    assert(ce_csl_registry_count(registry)==count);
    for(size_t i=0;i<count;++i) {
        const CeCslRegistryRow *row=ce_csl_registry_row(registry,i);
        assert(row && row->item_index==items[i] && row->citation_number==(uint64_t)i+1);
        assert(ce_csl_registry_item_row(registry,items[i])==i);
        assert(ce_csl_registry_citation_number(registry,items[i])==(uint64_t)i+1);
    }
    assert(!ce_csl_registry_row(registry,count));
    assert(!ce_csl_registry_row(registry,SIZE_MAX));
    assert(ce_csl_registry_item_row(registry,SIZE_MAX)==SIZE_MAX);
    assert(!ce_csl_registry_citation_number(registry,SIZE_MAX));
}

static void eligibility_order_and_numbering(void) {
    CeCslInput *input=load(fixture);
    const cJSON *root=ce_csl_input_data(input);
    CeCslRegistry *registry=build(input,NULL);
    const size_t initial[]={2,1,3};
    check(registry,initial,3);
    assert(ce_csl_registry_item_row(registry,0)==SIZE_MAX);
    assert(ce_csl_registry_citation_number(registry,0)==0);
    assert(ce_csl_registry_item_row(registry,4)==SIZE_MAX);
    assert(ce_csl_registry_citation_number(registry,4)==0);
    assert(ce_csl_registry_item_row(registry,5)==SIZE_MAX);
    assert(ce_csl_registry_citation_number(registry,5)==0);
    const size_t include[]={4,0,4,1,0,4,0}; /* Count may exceed input item count. */
    CeCslRegistryOptions options={include,7,NULL,0};
    assert(ce_csl_registry_build(input,&options,NULL,&registry,error,sizeof(error)));
    const size_t eligible[]={2,1,3,4,0};
    check(registry,eligible,5);
    const size_t order[]={4,2,0,3,1};
    options.row_order=order; options.row_order_count=5;
    assert(ce_csl_registry_build(input,&options,NULL,&registry,error,sizeof(error)));
    const size_t sorted[]={0,3,2,4,1};
    check(registry,sorted,5);
    assert(ce_csl_input_data(input)==root);
    assert(ce_csl_input_item(input,1)->first_citation==0);
    assert(ce_csl_input_item(input,1)->first_position==2);
    assert(cJSON_GetObjectItemCaseSensitive(ce_csl_input_item(input,1)->data,
                                          "citation-number")->valueint==999);

    /* Borrowed data in the old result can supply options until atomic commit. */
    const CeCslRegistryRow *old_row=ce_csl_registry_row(registry,0);
    options=(CeCslRegistryOptions){&old_row->item_index,1,NULL,0};
    assert(ce_csl_registry_build(input,&options,NULL,&registry,error,sizeof(error)));
    const size_t aliased[]={2,1,3,0};
    check(registry,aliased,4);
    ce_csl_input_free(input);
    check(registry,aliased,4); /* No input pointers escape. */
    ce_csl_registry_free(registry);
}

static void empty_and_uncited(void) {
    assert(!ce_csl_registry_count(NULL));
    assert(!ce_csl_registry_row(NULL,0));
    assert(ce_csl_registry_item_row(NULL,0)==SIZE_MAX);
    assert(!ce_csl_registry_citation_number(NULL,0));
    ce_csl_registry_free(NULL);
    CeCslInput *input=load("{\"items\":[],\"citations\":[]}");
    CeCslRegistry *registry=build(input,NULL);
    check(registry,NULL,0);
    const size_t unused=0;
    CeCslRegistryOptions options={&unused,0,&unused,0};
    assert(ce_csl_registry_build(input,&options,NULL,&registry,error,sizeof(error)));
    check(registry,NULL,0);
    options.include_count=1;
    assert(!ce_csl_registry_build(input,&options,NULL,&registry,error,sizeof(error)));
    check(registry,NULL,0);
    ce_csl_registry_free(registry); ce_csl_input_free(input);

    input=load("{\"items\":[{\"id\":\"one\"},{\"id\":\"two\"}],\"citations\":[]}");
    registry=build(input,NULL);
    check(registry,NULL,0);
    assert(ce_csl_registry_item_row(registry,0)==SIZE_MAX);
    const size_t include[]={1,1,0,1};
    options=(CeCslRegistryOptions){include,4,NULL,0};
    assert(ce_csl_registry_build(input,&options,NULL,&registry,error,sizeof(error)));
    const size_t expected[]={1,0}; check(registry,expected,2);
    assert(ce_csl_registry_build(input,NULL,NULL,&registry,NULL,0));
    check(registry,NULL,0);
    ce_csl_registry_free(registry); ce_csl_input_free(input);
}

typedef union AllocationHeader {
    long double alignment;
    void *pointer;
    struct { size_t size; unsigned magic; } record;
} AllocationHeader;
typedef struct AllocationState { size_t calls,fail_at,live; } AllocationState;
static void *allocate(void *context,size_t size) {
    AllocationState *s=context;
    assert(size);
    if(++s->calls==s->fail_at) return NULL;
    AllocationHeader *header=malloc(sizeof(*header)+size); assert(header);
    header->record.size=size; header->record.magic=0x52454749;
    ++s->live;
    return header+1;
}
static void deallocate(void *context,void *memory) {
    AllocationState *s=context;
    AllocationHeader *header=(AllocationHeader *)memory-1;
    assert(header->record.magic==0x52454749 && s->live);
    header->record.magic=0; --s->live; free(header);
}

static size_t oom_sweep(const CeCslInput *input,const CeCslRegistryOptions *options) {
    AllocationState state={0}; CeCslOutputAllocator memory={&state,allocate,deallocate};
    CeCslRegistry *registry=NULL;
    assert(ce_csl_registry_build(input,options,&memory,&registry,error,sizeof(error)));
    size_t calls=state.calls;
    ce_csl_registry_free(registry); assert(!state.live);

    CeCslInput *old_input=load(fixture);
    registry=build(old_input,NULL);
    CeCslRegistry *original=registry;
    const CeCslRegistryRow *row=ce_csl_registry_row(registry,0);
    const size_t initial[]={2,1,3};
    for(size_t failure=1;failure<=calls;++failure) {
        state.calls=0; state.fail_at=failure;
        assert(!ce_csl_registry_build(input,options,&memory,&registry,error,sizeof(error)));
        assert(registry==original && ce_csl_registry_row(registry,0)==row && !state.live && *error);
        check(registry,initial,3);
    }
    state.calls=0; state.fail_at=0;
    assert(ce_csl_registry_build(input,options,&memory,&registry,error,sizeof(error)));
    assert(state.live);
    /* Replacing an object also releases it using its own stored allocator. */
    assert(ce_csl_registry_build(old_input,NULL,NULL,&registry,error,sizeof(error)));
    assert(!state.live); check(registry,initial,3);
    ce_csl_registry_free(registry); ce_csl_input_free(old_input);
    return calls;
}

static void errors_and_allocations(void) {
    CeCslInput *input=load(fixture);
    CeCslRegistry *registry=build(input,NULL), *original=registry;
    const CeCslRegistryRow *view=ce_csl_registry_row(registry,0);
    AllocationState state={0}; CeCslOutputAllocator memory={&state,allocate,deallocate};
    size_t include[]={SIZE_MAX};
    CeCslRegistryOptions options={include,1,NULL,0};
    assert(!ce_csl_registry_build(input,&options,&memory,&registry,error,sizeof(error)));
    include[0]=5;
    assert(!ce_csl_registry_build(input,&options,&memory,&registry,error,sizeof(error)));
    include[0]=0; options.include_count=CE_CSL_INPUT_MAX_ITEMS+1;
    assert(!ce_csl_registry_build(input,&options,&memory,&registry,error,sizeof(error)));
    options.include_count=1; options.include_items=NULL;
    assert(!ce_csl_registry_build(input,&options,&memory,&registry,error,sizeof(error)));
    options=(CeCslRegistryOptions){0}; options.row_order_count=1;
    assert(!ce_csl_registry_build(input,&options,&memory,&registry,error,sizeof(error)));
    options.row_order=include; options.row_order_count=CE_CSL_INPUT_MAX_ITEMS+1;
    assert(!ce_csl_registry_build(input,&options,&memory,&registry,error,sizeof(error)));
    assert(!ce_csl_registry_build(NULL,NULL,&memory,&registry,error,sizeof(error)));
    assert(!ce_csl_registry_build(input,NULL,&memory,NULL,error,sizeof(error)));
    CeCslOutputAllocator bad={&state,allocate,NULL};
    assert(!ce_csl_registry_build(input,NULL,&bad,&registry,error,sizeof(error)));
    bad.allocate=NULL; bad.deallocate=deallocate;
    assert(!ce_csl_registry_build(input,NULL,&bad,&registry,error,sizeof(error)));
    assert(!state.calls && !state.live);

    size_t order[]={0,0,2};
    options=(CeCslRegistryOptions){NULL,0,order,3};
    assert(!ce_csl_registry_build(input,&options,&memory,&registry,error,sizeof(error)));
    order[1]=SIZE_MAX;
    assert(!ce_csl_registry_build(input,&options,&memory,&registry,error,sizeof(error)));
    order[1]=3;
    assert(!ce_csl_registry_build(input,&options,&memory,&registry,error,sizeof(error)));
    options.row_order_count=2;
    assert(!ce_csl_registry_build(input,&options,&memory,&registry,error,sizeof(error)));
    options.row_order_count=0;
    assert(!ce_csl_registry_build(input,&options,&memory,&registry,error,sizeof(error)));
    assert(registry==original && ce_csl_registry_row(registry,0)==view && !state.live);
    const size_t initial[]={2,1,3}; check(registry,initial,3);
    ce_csl_registry_free(registry);

    assert(oom_sweep(input,NULL)==4);
    const size_t added[]={4,0,4}; const size_t sorted[]={4,2,0,3,1};
    options=(CeCslRegistryOptions){added,3,sorted,5};
    assert(oom_sweep(input,&options)==4);
    ce_csl_input_free(input);
    input=load("{\"items\":[],\"citations\":[]}");
    assert(oom_sweep(input,NULL)==1); ce_csl_input_free(input);
    input=load("{\"items\":[{\"id\":\"uncited\"}],\"citations\":[]}");
    assert(oom_sweep(input,NULL)==3); ce_csl_input_free(input);
}

static void item_limit(void) {
    size_t count=CE_CSL_INPUT_MAX_ITEMS;
    size_t capacity=count*32+256, used=0;
    char *json=malloc(capacity); assert(json);
    int written=snprintf(json,capacity,"{\"items\":["); assert(written>0); used=(size_t)written;
    for(size_t i=0;i<count;++i) {
        written=snprintf(json+used,capacity-used,"%s{\"id\":\"item-%zu\"}",i?",":"",i);
        assert(written>0 && (size_t)written<capacity-used); used+=(size_t)written;
    }
    written=snprintf(json+used,capacity-used,
        "],\"citations\":[{\"citationID\":\"only\",\"citationItems\":[{\"id\":\"item-17\"}]}]}");
    assert(written>0 && (size_t)written<capacity-used);
    CeCslInput *input=load(json); free(json);
    CeCslRegistry *registry=build(input,NULL);
    const size_t only[]={17}; check(registry,only,1);
    assert(!ce_csl_registry_citation_number(registry,count-1));

    size_t *include=malloc(count*sizeof(*include)), *order=malloc(count*sizeof(*order));
    assert(include && order);
    for(size_t i=0;i<count;++i) { include[i]=count-1-i; order[i]=count-1-i; }
    CeCslRegistryOptions options={include,count,order,count};
    assert(ce_csl_registry_build(input,&options,NULL,&registry,error,sizeof(error)));
    assert(ce_csl_registry_count(registry)==count);
    for(size_t row=0;row<count;++row) {
        size_t expected=row==count-1?17:row<17?row:row+1;
        const CeCslRegistryRow *r=ce_csl_registry_row(registry,row);
        assert(r->item_index==expected && r->citation_number==(uint64_t)row+1);
        assert(ce_csl_registry_item_row(registry,expected)==row);
        assert(ce_csl_registry_citation_number(registry,expected)==(uint64_t)row+1);
    }
    /* The include-entry cap permits repeated indices and still deduplicates. */
    for(size_t i=0;i<count;++i) include[i]=0;
    options.row_order=NULL; options.row_order_count=0;
    assert(ce_csl_registry_build(input,&options,NULL,&registry,error,sizeof(error)));
    const size_t repeated[]={17,0}; check(registry,repeated,2);
    free(include); free(order);
    ce_csl_registry_free(registry); ce_csl_input_free(input);
}

int main(void) {
    eligibility_order_and_numbering(); empty_and_uncited(); errors_and_allocations(); item_limit();
    puts("CSL eligible-item registry, stable numbers, reverse lookup and all 4 allocator failure points passed");
    return 0;
}
