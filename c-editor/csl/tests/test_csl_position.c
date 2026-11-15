#include "csl_position.h"
#include <assert.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define F CE_CSL_POSITION_FIRST
#define S CE_CSL_POSITION_SUBSEQUENT
#define I (S | CE_CSL_POSITION_IBID)
#define L (I | CE_CSL_POSITION_IBID_WITH_LOCATOR)
#define N CE_CSL_POSITION_NEAR_NOTE

typedef struct ClusterSpec { uint32_t note; const char *cites; } ClusterSpec;
static char error[256];

static CeCslInput *input_from(const ClusterSpec *clusters, size_t count) {
    cJSON *root = cJSON_CreateObject();
    assert(root);
    cJSON *items = cJSON_Parse("[{\"id\":\"a\",\"first-reference-note-number\":999},"
                              "{\"id\":\"b\"},{\"id\":\"c\"}]");
    assert(items && cJSON_AddItemToObject(root, "items", items));
    cJSON *citations = cJSON_AddArrayToObject(root, "citations");
    assert(citations);
    for (size_t i = 0; i < count; ++i) {
        cJSON *cluster = cJSON_CreateObject();
        assert(cluster && cJSON_AddItemToArray(citations, cluster));
        char id[48];
        snprintf(id, sizeof(id), "cluster-%zu", i);
        assert(cJSON_AddStringToObject(cluster, "citationID", id));
        cJSON *cites = cJSON_Parse(clusters[i].cites);
        assert(cites && cJSON_AddItemToObject(cluster, "citationItems", cites));
        cJSON *properties = cJSON_AddObjectToObject(cluster, "properties");
        assert(properties && cJSON_AddNumberToObject(properties, "noteIndex", clusters[i].note));
    }
    char *json = cJSON_PrintUnformatted(root);
    assert(json);
    CeCslInput *input = NULL;
    bool ok = ce_csl_input_load(&input, json, strlen(json), error, sizeof(error));
    if (!ok) fprintf(stderr, "Input: %s\n", error);
    assert(ok);
    cJSON_free(json);
    cJSON_Delete(root);
    return input;
}

static void check(const CeCslPositions *p, size_t cluster, size_t cite,
                  unsigned flags, uint32_t first_note) {
    const CeCslPositionResult *r = ce_csl_positions_at(p, cluster, cite);
    assert(r);
    if (r->position != flags || r->first_reference_note_number != first_note)
        fprintf(stderr, "Position %zu/%zu: %u/%" PRIu32 " expected %u/%" PRIu32 "\n",
            cluster, cite, r->position, r->first_reference_note_number, flags, first_note);
    assert(r->position == flags && r->first_reference_note_number == first_note);
}

static CeCslPositions derive(const CeCslInput *input, const CeCslPositionOptions *options) {
    CeCslPositions p = {0};
    assert(ce_csl_positions_derive(input, options, NULL, &p, error, sizeof(error)));
    assert(!*error);
    return p;
}

static void streams_and_footnotes(void) {
    const ClusterSpec clusters[] = {
        {0,"[{\"id\":\"a\",\"position\":3,\"near-note\":true}]"},
        {1,"[{\"id\":\"a\"}]"}, {0,"[{\"id\":\"a\"}]"},
        {2,"[{\"id\":\"a\"}]"}, {2,"[{\"id\":\"b\"}]"},
        {3,"[{\"id\":\"b\"}]"}, {4,"[{\"id\":\"b\"}]"},
        {6,"[{\"id\":\"b\"}]"}, {12,"[{\"id\":\"b\"}]"}
    };
    CeCslInput *input = input_from(clusters, sizeof(clusters)/sizeof(*clusters));
    const cJSON *before = ce_csl_input_data(input);
    CeCslPositionOptions options = {0}; options.note_style = true;
    CeCslPositions p = derive(input, &options);
    check(&p,0,0,F,0); check(&p,1,0,F,0); check(&p,2,0,I,0);
    check(&p,3,0,I|N,1); check(&p,4,0,F,0);
    check(&p,5,0,S|N,2); /* Entire prior note has two singleton clusters. */
    check(&p,6,0,I|N,2); check(&p,7,0,S|N,2); check(&p,8,0,S,2);
    assert(ce_csl_input_data(input) == before);
    assert(cJSON_GetObjectItemCaseSensitive(ce_csl_input_cluster(input,0)->items[0].data,
                                          "position")->valueint == 3);
    assert(ce_csl_input_item(input,0)->first_citation == 0);
    options.note_style = false;
    CeCslPositions q = derive(input, &options);
    for (size_t i=0; i<p.cite_count; ++i) {
        assert(q.cites[i].position == p.cites[i].position);
        assert(q.cites[i].first_reference_note_number == 0);
    }
    ce_csl_input_free(input); /* Results contain no borrowed input pointers. */
    check(&p,3,0,I|N,1);
    ce_csl_positions_free(&p); ce_csl_positions_free(&q);
}

static void adjacency_and_permutation(void) {
    const ClusterSpec clusters[] = {
        {1,"[{\"id\":\"a\"},{\"id\":\"b\"},{\"id\":\"a\"}]"},
        {1,"[{\"id\":\"a\"}]"}, {2,"[{\"id\":\"a\"}]"},
        {0,"[{\"id\":\"b\"},{\"id\":\"a\"}]"},
        {0,"[{\"id\":\"a\"}]"},
        {0,"[{\"id\":\"a\"},{\"id\":\"a\"},{\"id\":\"b\"}]"}
    };
    CeCslInput *input = input_from(clusters, sizeof(clusters)/sizeof(*clusters));
    CeCslPositionOptions options = {0}; options.note_style = true;
    options.near_note_distance_set = true; options.near_note_distance = 0;
    CeCslPositions p = derive(input, &options);
    check(&p,0,0,F,0); check(&p,0,1,F,0); check(&p,0,2,S|N,0);
    check(&p,1,0,I|N,0); check(&p,2,0,S,1);
    check(&p,3,0,F,0); check(&p,3,1,F,0); check(&p,4,0,S,0);
    check(&p,5,0,I,0); check(&p,5,1,I,0); check(&p,5,2,S,0);
    const size_t order[] = {2,0,1,3,4,5,6,7,8,9,10};
    options.cite_order = order; options.cite_order_count = sizeof(order)/sizeof(*order);
    assert(ce_csl_positions_derive(input,&options,NULL,&p,error,sizeof(error)));
    check(&p,0,0,I|N,0); check(&p,0,1,F,0); check(&p,0,2,F,0);
    check(&p,1,0,S|N,0); /* Last rendered cite is now b, not original a. */
    assert(p.cluster_offsets[0]==0 && p.cluster_offsets[1]==3 && p.cite_count==11);
    assert(!ce_csl_positions_at(&p,0,3) && !ce_csl_positions_at(&p,6,0));
    assert(!ce_csl_positions_at(NULL,0,0));
    ce_csl_positions_free(&p); ce_csl_input_free(input);
}

static void locators(void) {
    const ClusterSpec clusters[] = {{1,
        "[{\"id\":\"a\"},"
        "{\"id\":\"a\",\"locator\":\"7\"},"
        "{\"id\":\"a\",\"locator\":\"7\",\"label\":\"page\"},"
        "{\"id\":\"a\",\"locator\":\"7\",\"label\":\"chapter\"},"
        "{\"id\":\"a\",\"locator\":\"7\",\"label\":\"chapter\",\"prefix\":\"see\"},"
        "{\"id\":\"a\",\"locator\":\"\"},"
        "{\"id\":\"a\",\"label\":\"chapter\",\"suffix\":\"!\"},"
        "{\"id\":\"a\",\"locator\":\"1\",\"label\":\"23\"},"
        "{\"id\":\"a\",\"locator\":\"12\",\"label\":\"3\"},"
        "{\"id\":\"a\",\"locator\":\"12\",\"label\":\"\"},"
        "{\"id\":\"a\",\"locator\":\"12\"},"
        "{\"id\":\"a\",\"locator\":\"12 \"},"
        "{\"id\":\"a\",\"locator\":\"章节一\"},"
        "{\"id\":\"a\",\"locator\":\"章节一\",\"author-only\":true}]"}};
    const unsigned expected[] = {F,L|N,I|N,L|N,I|N,S|N,I|N,L|N,L|N,L|N,I|N,L|N,L|N,I|N};
    CeCslInput *input = input_from(clusters,1);
    CeCslPositions p = derive(input,NULL);
    for (size_t i=0;i<sizeof(expected)/sizeof(*expected);++i) check(&p,0,i,expected[i],0);
    ce_csl_positions_free(&p); ce_csl_input_free(input);

    const ClusterSpec across[] = {
        {3,"[{\"id\":\"a\",\"locator\":\"1\"},{\"id\":\"a\",\"locator\":\"2\"}]"},
        {3,"[{\"id\":\"a\",\"locator\":\"2\",\"label\":\"page\"}]"},
        {4,"[{\"id\":\"a\",\"locator\":\"2\"}]"}
    };
    input=input_from(across,3); p=derive(input,NULL);
    check(&p,0,0,F,0); check(&p,0,1,L|N,0);
    check(&p,1,0,I|N,0); /* Compare the previous cluster's LAST locator. */
    check(&p,2,0,S|N,0); /* Even repeated same-item cites make a note non-solo. */
    ce_csl_positions_free(&p); ce_csl_input_free(input);
}

static void reset_and_boundaries(void) {
    const ClusterSpec clusters[] = {
        {UINT32_MAX-1,"[{\"id\":\"a\"}]"},
        {0,"[{\"id\":\"a\"}]"},
        {UINT32_MAX,"[{\"id\":\"a\"}]"},
        {1,"[{\"id\":\"a\"}]"},
        {0,"[{\"id\":\"a\"}]"},
        {2,"[{\"id\":\"a\"}]"}
    };
    CeCslInput *input = input_from(clusters,sizeof(clusters)/sizeof(*clusters));
    CeCslPositions p = {0};
    assert(!ce_csl_positions_derive(input,NULL,NULL,&p,error,sizeof(error)));
    assert(strstr(error,"Unsupported") && !p.cites && !p.cluster_offsets);
    CeCslPositionOptions options = {0}; options.note_style = true;
    options.allow_note_number_reset = true;
    p = derive(input,&options);
    check(&p,0,0,F,0); check(&p,1,0,F,0); check(&p,2,0,I|N,UINT32_MAX-1);
    check(&p,3,0,F,0); check(&p,4,0,I,0); check(&p,5,0,I|N,1);
    ce_csl_positions_free(&p); ce_csl_input_free(input);

    const ClusterSpec distances[] = {
        {1,"[{\"id\":\"a\"}]"}, {6,"[{\"id\":\"a\"}]"},
        {12,"[{\"id\":\"a\"}]"}, {UINT32_MAX,"[{\"id\":\"a\"}]"}
    };
    input=input_from(distances,4); p=derive(input,NULL);
    check(&p,1,0,S|N,0); check(&p,2,0,S,0); check(&p,3,0,S,0);
    options.near_note_distance_set = true; options.near_note_distance = UINT32_MAX;
    assert(ce_csl_positions_derive(input,&options,NULL,&p,error,sizeof(error)));
    check(&p,3,0,S|N,1);
    ce_csl_positions_free(&p); ce_csl_input_free(input);
}

typedef union AllocationHeader {
    long double alignment;
    void *pointer;
    struct { size_t size; unsigned magic; } record;
} AllocationHeader;
typedef struct AllocationState { size_t calls, fail_at, live; } AllocationState;
static void *allocate(void *context,size_t size) {
    AllocationState *s=context;
    if (++s->calls==s->fail_at) return NULL;
    AllocationHeader *h=malloc(sizeof(*h)+size);
    assert(h); h->record.size=size; h->record.magic=0x504f5349;
    ++s->live; return h+1;
}
static void deallocate(void *context,void *memory) {
    AllocationState *s=context;
    AllocationHeader *h=(AllocationHeader *)memory-1;
    assert(h->record.magic==0x504f5349 && s->live);
    h->record.magic=0; --s->live; free(h);
}

static void atomicity_and_errors(void) {
    const ClusterSpec clusters[] = {{1,"[{\"id\":\"a\"},{\"id\":\"b\"}]"},
                                   {2,"[{\"id\":\"a\"}]"}};
    CeCslInput *input=input_from(clusters,2);
    AllocationState state={0};
    CeCslOutputAllocator allocator={&state,allocate,deallocate};
    CeCslPositions p={0};
    assert(ce_csl_positions_derive(input,NULL,&allocator,&p,error,sizeof(error)));
    assert(state.calls==3 && state.live==2);
    CeCslPositionResult *original=p.cites;
    size_t *offsets=p.cluster_offsets;
    for(size_t failure=1;failure<=3;++failure) {
        state.calls=0; state.fail_at=failure;
        assert(!ce_csl_positions_derive(input,NULL,&allocator,&p,error,sizeof(error)));
        assert(p.cites==original && p.cluster_offsets==offsets && state.live==2 && *error);
        check(&p,0,0,F,0);
    }
    state.calls=0; state.fail_at=0;
    CeCslPositionOptions options={0};
    size_t order[]={0,0,2}; options.cite_order=order; options.cite_order_count=3;
    assert(!ce_csl_positions_derive(input,&options,&allocator,&p,error,sizeof(error)));
    order[1]=2;
    assert(!ce_csl_positions_derive(input,&options,&allocator,&p,error,sizeof(error)));
    order[1]=SIZE_MAX;
    assert(!ce_csl_positions_derive(input,&options,&allocator,&p,error,sizeof(error)));
    options.cite_order_count=2;
    assert(!ce_csl_positions_derive(input,&options,&allocator,&p,error,sizeof(error)));
    options.cite_order=NULL;
    assert(!ce_csl_positions_derive(input,&options,&allocator,&p,error,sizeof(error)));
    assert(p.cites==original && p.cluster_offsets==offsets && state.live==2);
    state.calls=0;
    assert(!ce_csl_positions_derive(NULL,NULL,&allocator,&p,error,sizeof(error)));
    assert(!ce_csl_positions_derive(input,NULL,&allocator,NULL,error,sizeof(error)));
    CeCslOutputAllocator bad={&state,allocate,NULL};
    assert(!ce_csl_positions_derive(input,NULL,&bad,&p,error,sizeof(error)));
    assert(state.calls==0);
    assert(ce_csl_positions_derive(input,NULL,&allocator,&p,NULL,0));
    assert(state.live==2);
    ce_csl_positions_free(&p); assert(state.live==0);
    ce_csl_positions_free(&p); ce_csl_positions_free(NULL);
    ce_csl_input_free(input);

    input=input_from(NULL,0); p=derive(input,NULL);
    assert(!p.cites && !p.cite_count && !p.cluster_count && p.cluster_offsets[0]==0);
    assert(!ce_csl_positions_at(&p,0,0));
    ce_csl_positions_free(&p); ce_csl_input_free(input);
    input=NULL;
    const char empty[]="{\"items\":[],\"citations\":[]}";
    assert(ce_csl_input_load(&input,empty,sizeof(empty)-1,error,sizeof(error)));
    p=derive(input,NULL);
    assert(!p.cites && p.cluster_offsets[0]==0);
    ce_csl_positions_free(&p); ce_csl_input_free(input);
}

static void many_sections(void) {
    const size_t count=6000;
    ClusterSpec *clusters=malloc(count*sizeof(*clusters)); assert(clusters);
    for(size_t i=0;i<count;++i) {
        clusters[i].note=(uint32_t)(i%2+1); clusters[i].cites="[{\"id\":\"a\"}]";
    }
    CeCslInput *input=input_from(clusters,count); free(clusters);
    CeCslPositionOptions options={0}; options.note_style=true; options.allow_note_number_reset=true;
    CeCslPositions p=derive(input,&options);
    for(size_t i=0;i<count;++i) check(&p,i,0,i%2?I|N:F,i%2?1:0);
    ce_csl_positions_free(&p); ce_csl_input_free(input);
}

int main(void) {
    streams_and_footnotes(); adjacency_and_permutation(); locators();
    reset_and_boundaries(); atomicity_and_errors(); many_sections();
    puts("CSL immutable citation positions, note policies, permutations and 3 allocator failure points passed");
    return 0;
}
