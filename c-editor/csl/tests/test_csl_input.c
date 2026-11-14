#include "csl_input.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char error[256];
static const char valid[]="{\"items\":[{\"id\":\"未引用\",\"custom\":{\"bool\":true,\"array\":[null,3]}},{\"id\":1e0,\"type\":\"book\",\"title\":\"中文🙂\",\"author\":[{\"family\":\"Smith\"}]},{\"id\":\"02\"}],\"citations\":[{\"citationID\":\"first\",\"citationItems\":[{\"id\":\"02\",\"prefix\":\"see \",\"locator\":\"7–8\",\"label\":\"page\"},{\"id\":1,\"suppress-author\":true}],\"properties\":{\"noteIndex\":1}},{\"citationID\":\"second\",\"citationItems\":[{\"id\":\"1\",\"author-only\":true}]}]}";
static void rejected(CeCslInput **slot,const char *json) {
    CeCslInput *old=*slot; const CeCslInputItem *view=ce_csl_input_item(old,1);
    assert(!ce_csl_input_load(slot,json,strlen(json),error,sizeof error)); assert(*error);
    assert(*slot==old && ce_csl_input_item(*slot,1)==view);
    assert(!strcmp(view->id,"1"));
}
static void numeric_case(CeCslInput **slot,const char *token,const char *canonical,bool accepted) {
    char json[1024];
    snprintf(json,sizeof json,"{\"items\":[{\"id\":%s}],\"citations\":[{\"citationID\":\"c\",\"citationItems\":[{\"id\":\"%s\"}]}]}",token,canonical);
    CeCslInput *old=*slot;
    bool ok=ce_csl_input_load(slot,json,strlen(json),error,sizeof error);
    if(ok!=accepted) fprintf(stderr,"token %s: %s\n",token,error);
    assert(ok==accepted);
    if(ok) assert(!strcmp(ce_csl_input_item(*slot,0)->id,canonical)); else assert(*slot==old);
}
static size_t attempts,fail_at;
static void *json_allocate(size_t bytes) { ++attempts; return attempts==fail_at?NULL:malloc(bytes); }
int main(void) {
    CeCslInput *input=NULL; assert(ce_csl_input_load(&input,valid,strlen(valid),error,sizeof error));
    assert(ce_csl_input_item_count(input)==3 && ce_csl_input_cluster_count(input)==2);
    const CeCslInputItem *item=ce_csl_input_item(input,0);
    assert(item->first_citation==SIZE_MAX && item->first_position==SIZE_MAX);
    const cJSON *custom=cJSON_GetObjectItemCaseSensitive(item->data,"custom");
    assert(cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(custom,"bool")));
    assert(cJSON_IsNull(cJSON_GetArrayItem(cJSON_GetObjectItemCaseSensitive(custom,"array"),0)));
    assert(ce_csl_input_item(input,1)->first_citation==0 && ce_csl_input_item(input,1)->first_position==1);
    assert(ce_csl_input_find(input,"1")==1 && ce_csl_input_find(input,"02")==2 && ce_csl_input_find(input,"2")==SIZE_MAX);
    const CeCslInputCluster *cluster=ce_csl_input_cluster(input,0);
    assert(cluster->item_count==2 && cluster->note_index==1 && cluster->items[0].item_index==2);
    assert(!strcmp(cJSON_GetObjectItemCaseSensitive(cluster->items[0].data,"prefix")->valuestring,"see "));
    assert(ce_csl_input_cluster(input,1)->note_index==0 && !ce_csl_input_cluster(input,2));
    assert(!ce_csl_input_item(NULL,0) && !ce_csl_input_item(input,3));
    const char *bad[]={
        "", "null", "[]", "{\"items\":[],\"citations\":[]} trailing",
        "{\"items\":[],\"citations\":[],}", "{\"items\":[,],\"citations\":[]}",
        "{\"items\":[],\"items\":[],\"citations\":[]}",
        "{\"items\":[],\"\\u0069tems\":[],\"citations\":[]}",
        "{\"items\":[{\"id\":1},{\"id\":\"1\"}],\"citations\":[]}",
        "{\"items\":[{\"id\":true}],\"citations\":[]}",
        "{\"items\":[{\"id\":\"\"}],\"citations\":[]}",
        "{\"items\":[{\"id\":\"bad\\nID\"}],\"citations\":[]}",
        "{\"items\":[{\"id\":\"x\\u0000y\"}],\"citations\":[]}",
        "{\"items\":[{\"id\":\"\\ud800\"}],\"citations\":[]}",
        "{\"items\":[{\"id\":\"\\udc00\"}],\"citations\":[]}",
        "{\"items\":[{\"id\":\"\\ud800\\u0041\"}],\"citations\":[]}",
        "{\"items\":[{\"id\":\"raw\nnewline\"}],\"citations\":[]}",
        "{\"items\":[{\"id\":1e999}],\"citations\":[]}",
        "{\"items\":[{\"id\":01}],\"citations\":[]}",
        "{\"items\":[{\"id\":+1}],\"citations\":[]}",
        "{\"items\":[{\"id\":1.}],\"citations\":[]}",
        "{\"items\":[{\"id\":1e}],\"citations\":[]}",
        "{\"items\":[{\"id\":1e+}],\"citations\":[]}",
        "{\"items\":[{\"id\":NaN}],\"citations\":[]}",
        "{\"items\":[{\"id\":\"a\",\"custom\":{\"a\":1,\"a\":2}}],\"citations\":[]}",
        "{\"items\":[],\"citations\":[{\"citationID\":\"c\",\"citationItems\":[]}]}",
        "{\"items\":[],\"citations\":[{\"citationID\":\"c\",\"citationItems\":[{\"id\":\"missing\"}]}]}",
        "{\"items\":[{\"id\":1}],\"citations\":[{\"citationID\":1,\"citationItems\":[{\"id\":1}]}]}",
        "{\"items\":[{\"id\":1}],\"citations\":[{\"citationID\":\"c\",\"citationItems\":[{\"id\":1,\"locator\":7}]}]}",
        "{\"items\":[{\"id\":1}],\"citations\":[{\"citationID\":\"c\",\"citationItems\":[{\"id\":1,\"author-only\":1}]}]}",
        "{\"items\":[{\"id\":1}],\"citations\":[{\"citationID\":\"c\",\"citationItems\":[{\"id\":1,\"author-only\":true,\"suppress-author\":true}]}]}",
        "{\"items\":[{\"id\":1}],\"citations\":[{\"citationID\":\"c\",\"citationItems\":[{\"id\":1}],\"properties\":{\"noteIndex\":1.00000000000000001}}]}",
        "{\"items\":[{\"id\":1}],\"citations\":[{\"citationID\":\"c\",\"citationItems\":[{\"id\":1}]},{\"citationID\":\"c\",\"citationItems\":[{\"id\":1}]}]}"
    };
    for(size_t i=0;i<sizeof bad/sizeof *bad;i++) rejected(&input,bad[i]);
    char invalid_utf8[]={ '{','"','x','"',':','"',(char)0xc0,(char)0x80,'"','}',0 };
    rejected(&input,invalid_utf8);
    char raw_nul[sizeof valid]; memcpy(raw_nul,valid,sizeof valid); raw_nul[20]=0;
    CeCslInput *old=input; assert(!ce_csl_input_load(&input,raw_nul,sizeof valid-1,error,sizeof error) && input==old);
    char deep[1024]; size_t at=0;
    for(size_t i=0;i<129;i++) deep[at++]='[';
    deep[at++]='0'; for(size_t i=0;i<129;i++) deep[at++]=']'; deep[at]=0;
    rejected(&input,deep);
    struct {const char *raw,*id;bool ok;} numbers[]={
        {"9007199254740991","9007199254740991",true},
        {"-9007199254740991","-9007199254740991",true},
        {"9007199254740992","9007199254740992",false},
        {"9007199254740991.1","9007199254740991",false},
        {"0.999999999999999999","1",false},
        {"1.000000000000000001","1",false},
        {"0.001e3","1",true}, {"12300e-2","123",true},
        {"1.2e1","12",true}, {"100.0000e-2","1",true},
        {"1e-9999","0",false}, {"-0","0",true}, {"0e9999","0",true},
        {"9e15","9000000000000000",true}, {"1e16","10000000000000000",false}
    };
    for(size_t i=0;i<sizeof numbers/sizeof *numbers;i++) numeric_case(&input,numbers[i].raw,numbers[i].id,numbers[i].ok);
    const char *surrogate="{\"items\":[{\"id\":\"\\ud83d\\ude42\"}],\"citations\":[]}";
    assert(ce_csl_input_load(&input,surrogate,strlen(surrogate),error,sizeof error));
    assert(!strcmp(ce_csl_input_item(input,0)->id,"🙂"));
    /* cJSON allocation failures never replace the previously loaded model.
     * These hooks cover cJSON allocations, not this module's malloc indexes. */
    cJSON_Hooks hooks={json_allocate,free}; cJSON_InitHooks(&hooks);
    attempts=fail_at=0; assert(ce_csl_input_load(&input,valid,strlen(valid),error,sizeof error));
    size_t allocations=attempts;
    for(size_t i=1;i<=allocations;i++) { attempts=0; fail_at=i; rejected(&input,valid); }
    fail_at=0; cJSON_InitHooks(NULL);
    const char *empty="{\"items\":[],\"citations\":[]}";
    assert(ce_csl_input_load(&input,empty,strlen(empty),error,sizeof error));
    assert(!ce_csl_input_item_count(input) && !ce_csl_input_cluster_count(input));
    assert(ce_csl_input_find(input,"missing")==SIZE_MAX);
    ce_csl_input_free(input); ce_csl_input_free(NULL);
    puts("CSL input: exact IDs, ordered clusters, retained metadata, strict JSON and transactional failures passed");
    return 0;
}
