#include "csl_disambiguation.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* This typed provider exercises the driver. It is deliberately NOT a CSL style
 * renderer or an APA conformance oracle; production needs the real finalizer. */
enum {MAX_ITEMS=64, MAX_PEOPLE=4, MAX_OCCURRENCES=2};
typedef struct Person {
    CeCslNameIdentity identity;
    const char *style, *initials;
    bool can_initials;
} Person;
typedef struct ModelItem {
    Person people[MAX_PEOPLE]; size_t people_count;
    size_t first, subsequent, bibliography, occurrences;
    const char *year, *branch;
    bool cited, ignore_suffix, constant_key;
} ModelItem;
typedef struct Model {
    ModelItem items[MAX_ITEMS]; size_t count, calls, invisible;
    bool provider_error, invalid_key, stale_minimum;
    size_t cancel_after;
} Model;
typedef struct OwnedProbe {
    CeCslDisambiguationProbe public_probe;
    CeCslOutputAllocator memory;
    CeCslDisambiguationNames names[MAX_OCCURRENCES];
    CeCslDisambiguationPerson people[MAX_OCCURRENCES][MAX_PEOPLE];
    const CeCslXmlNode *calls[MAX_OCCURRENCES];
    char key[1024];
} OwnedProbe;
static CeCslXmlNode names_node, macro_calls[MAX_OCCURRENCES];
static char error[256];
static CeCslDisambiguationBytes bytes(const char *s) {
    return (CeCslDisambiguationBytes){(const unsigned char *)s,strlen(s)};
}
static CeCslEvalOccurrence occurrence(size_t which,const CeCslXmlNode **call) {
    *call=&macro_calls[which];
    return (CeCslEvalOccurrence){&names_node,call,1};
}
static void append(char *out,size_t capacity,const char *text) {
    size_t length=strlen(out), size=strlen(text); assert(length+size<capacity);
    memcpy(out+length,text,size+1);
}
static bool overridden(const CeCslEvalNameAdjustment *a,size_t person) {
    for(size_t i=0;i<a->override_count;++i)
        if(a->overrides[i].binding==0 && a->overrides[i].person_index==person) return true;
    return false;
}
static CeCslDisambiguationStatus render(void *context,const CeCslDisambiguationRequest *request,
    const CeCslOutputAllocator *allocator,CeCslDisambiguationProbe **out,char *message,size_t capacity) {
    (void)message; (void)capacity;
    Model *model=context; ++model->calls;
    if(request->invisible) ++model->invisible;
    assert(request->item_index<model->count);
    if(model->provider_error) return CE_CSL_DISAMBIGUATION_PROVIDER_ERROR;
    OwnedProbe *p=allocator->allocate(allocator->context,sizeof(*p));
    if(!p) return CE_CSL_DISAMBIGUATION_OOM;
    memset(p,0,sizeof(*p)); p->memory=*allocator;
    ModelItem *item=&model->items[request->item_index];
    p->public_probe.names=p->names; p->public_probe.names_count=item->occurrences;
    for(size_t n=0;n<item->occurrences;++n) {
        p->names[n].occurrence=occurrence(n,&p->calls[n]);
        p->names[n].people=p->people[n]; p->names[n].person_count=item->people_count;
        CeCslEvalNameAdjustment a={0};
        assert(ce_csl_disambiguation_adjust_names(request->state,request->profile,&p->names[n].occurrence,&a));
        size_t shown=request->profile==CE_CSL_DISAMBIGUATION_SUBSEQUENT?item->subsequent:
            request->profile==CE_CSL_DISAMBIGUATION_BIBLIOGRAPHY?item->bibliography:item->first;
        if(shown<a.minimum_names) shown=a.minimum_names;
        if(shown>item->people_count) shown=item->people_count;
        p->names[n].next_minimum_names=shown<item->people_count?shown+1:0;
        if(model->stale_minimum && a.minimum_names) p->names[n].next_minimum_names=a.minimum_names;
        for(size_t i=0;i<item->people_count;++i) {
            Person *person=&item->people[i];
            CeCslDisambiguationPerson *r=&p->people[n][i];
            *r=(CeCslDisambiguationPerson){0}; r->person_index=i;
            r->source_variable=n?"editor":"author"; r->identity=&person->identity;
            r->rendered=i<shown; r->primary=n==0 && i==0 && shown;
            r->can_initials=person->can_initials;
            r->given_visible=r->given_initialized=overridden(&a,i);
            r->style_key=bytes(person->style); r->initials_key=bytes(person->initials);
            if(i<shown) {
                if(i) append(p->key,sizeof(p->key),"+");
                append(p->key,sizeof(p->key),overridden(&a,i)?person->initials:person->style);
            }
        }
        if(shown<item->people_count) append(p->key,sizeof(p->key),"+etal");
        append(p->key,sizeof(p->key),"|");
    }
    if(item->constant_key) strcpy(p->key,"constant");
    append(p->key,sizeof(p->key),item->year?item->year:"");
    if(request->state->disambiguate) append(p->key,sizeof(p->key),item->branch?item->branch:"");
    if(!item->ignore_suffix) append(p->key,sizeof(p->key),request->state->year_suffix);
    p->public_probe.key=bytes(p->key);
    if(model->invalid_key) p->public_probe.key=(CeCslDisambiguationBytes){NULL,1};
    *out=&p->public_probe; return CE_CSL_DISAMBIGUATION_OK;
}
static void release(void *context,CeCslDisambiguationProbe *probe) {
    (void)context;
    OwnedProbe *p=(OwnedProbe *)probe;
    CeCslOutputAllocator memory=p->memory;
    memory.deallocate(memory.context,p);
}
static Person person(const char *family,const char *given,const char *initials) {
    Person p={0}; p.identity.kind=CE_CSL_NAME_PERSONAL; p.identity.family=family;
    p.identity.given=given; p.style=family; p.initials=initials; p.can_initials=true;
    return p;
}
static Model model_of(size_t count) {
    Model model={0}; model.count=count;
    for(size_t i=0;i<count;++i) {
        ModelItem *item=&model.items[i];
        item->people[0]=person("Doe","Alice","A.Doe"); item->people_count=1;
        item->first=item->subsequent=item->bibliography=1; item->occurrences=1;
        item->year="2024"; item->cited=true;
    }
    return model;
}
typedef struct Fixture { CeCslInput *input; CeCslRegistry *registry; } Fixture;
static Fixture fixture(Model *model,const size_t *include,size_t include_count,
                       const size_t *order,size_t order_count) {
    cJSON *root=cJSON_CreateObject(); assert(root);
    cJSON *items=cJSON_AddArrayToObject(root,"items"), *citations=cJSON_AddArrayToObject(root,"citations");
    assert(items && citations);
    for(size_t i=0;i<model->count;++i) {
        char id[64]; snprintf(id,sizeof(id),"item-%zu",i);
        cJSON *item=cJSON_CreateObject(); assert(item && cJSON_AddItemToArray(items,item));
        assert(cJSON_AddStringToObject(item,"id",id));
        assert(cJSON_AddStringToObject(item,"year-suffix","stale"));
        if(model->items[i].cited) {
            cJSON *cluster=cJSON_CreateObject(); assert(cluster && cJSON_AddItemToArray(citations,cluster));
            assert(cJSON_AddStringToObject(cluster,"citationID",id));
            cJSON *cites=cJSON_AddArrayToObject(cluster,"citationItems"), *cite=cJSON_CreateObject();
            assert(cites && cite && cJSON_AddItemToArray(cites,cite));
            assert(cJSON_AddStringToObject(cite,"id",id));
        }
    }
    char *json=cJSON_PrintUnformatted(root); assert(json); Fixture f={0};
    assert(ce_csl_input_load(&f.input,json,strlen(json),error,sizeof(error)));
    cJSON_free(json); cJSON_Delete(root);
    CeCslRegistryOptions opts={include,include_count,order,order_count};
    assert(ce_csl_registry_build(f.input,&opts,NULL,&f.registry,error,sizeof(error)));
    return f;
}
static void fixture_free(Fixture f) { ce_csl_registry_free(f.registry); ce_csl_input_free(f.input); }
static CeCslDisambiguationStatus build(Fixture f,Model *model,const CeCslDisambiguationOptions *options,
    const CeCslOutputAllocator *allocator,CeCslDisambiguation **out) {
    CeCslDisambiguationProvider provider={model,render,release};
    return ce_csl_disambiguation_build(f.input,f.registry,options,&provider,allocator,out,error,sizeof(error));
}
static CeCslDisambiguation *success(Fixture f,Model *model,const CeCslDisambiguationOptions *options) {
    CeCslDisambiguation *r=NULL; CeCslDisambiguationStatus status=build(f,model,options,NULL,&r);
    if(status!=CE_CSL_DISAMBIGUATION_OK) fprintf(stderr,"Disambiguation: %d %s\n",status,error);
    assert(status==CE_CSL_DISAMBIGUATION_OK && !*error); return r;
}
static CeCslEvalNameAdjustment adjusted(const CeCslDisambiguation *r,size_t item,
    CeCslDisambiguationProfile profile,size_t which) {
    const CeCslXmlNode *call; CeCslEvalOccurrence o=occurrence(which,&call);
    CeCslEvalNameAdjustment a={0};
    assert(ce_csl_disambiguation_adjust_names(ce_csl_disambiguation_item(r,item),profile,&o,&a));
    return a;
}
static void global_primary(void) {
    Model m=model_of(3);
    m.items[0].people[0]=person("Lee","Alice","A.Lee"); m.items[0].year="2001";
    m.items[1].people[0]=person("Lee","Bob","B.Lee"); m.items[1].year="2002";
    m.items[2].people[0]=person("Other","Cara","C.Other");
    m.items[2].people[1]=m.items[0].people[0]; m.items[2].people_count=m.items[2].first=2;
    for(size_t i=0;i<3;++i) m.items[i].occurrences=2;
    Fixture f=fixture(&m,NULL,0,NULL,0);
    CeCslDisambiguationOptions o={0}; o.add_given_names=true;
    o.given_rule=CE_CSL_DISAMBIGUATE_PRIMARY_NAME_INITIALS;
    o.profiles=CE_CSL_DISAMBIGUATION_FIRST|CE_CSL_DISAMBIGUATION_BIBLIOGRAPHY;
    CeCslDisambiguation *r=success(f,&m,&o);
    assert(adjusted(r,0,CE_CSL_DISAMBIGUATION_FIRST,0).override_count==1);
    assert(adjusted(r,1,CE_CSL_DISAMBIGUATION_FIRST,0).override_count==1);
    assert(!adjusted(r,0,CE_CSL_DISAMBIGUATION_FIRST,1).override_count);
    assert(!adjusted(r,2,CE_CSL_DISAMBIGUATION_FIRST,0).override_count);
    assert(adjusted(r,0,CE_CSL_DISAMBIGUATION_BIBLIOGRAPHY,0).override_count==1);
    assert(!ce_csl_disambiguation_unresolved_count(r));
    assert(!ce_csl_disambiguation_item(r,0)->year_suffix[0]);
    assert(!ce_csl_disambiguation_item(r,SIZE_MAX));
    ce_csl_disambiguation_free(r); fixture_free(f);

    m=model_of(2); m.items[1].people[0]=person("Doe","Amy","A.Doe");
    f=fixture(&m,NULL,0,NULL,0); o.profiles=CE_CSL_DISAMBIGUATION_FIRST; o.add_year_suffix=true;
    r=success(f,&m,&o);
    assert(!adjusted(r,0,CE_CSL_DISAMBIGUATION_FIRST,0).override_count);
    assert(!strcmp(ce_csl_disambiguation_item(r,0)->year_suffix,"a"));
    assert(!strcmp(ce_csl_disambiguation_item(r,1)->year_suffix,"b"));
    ce_csl_disambiguation_free(r); fixture_free(f);
}
static Model expanding_model(void) {
    Model m=model_of(5);
    for(size_t i=0;i<5;++i) {
        m.items[i].people[1]=i<2?person("One","X","X.One"):person("Two","Y","Y.Two");
        m.items[i].people[2]=person("Tail","Z","Z.Tail"); m.items[i].people_count=3;
        m.items[i].branch="branch";
    }
    m.items[4].people[0]=person("Doe","Bob","B.Doe");
    return m;
}
static CeCslDisambiguationOptions all_methods(void) {
    CeCslDisambiguationOptions o={0}; o.add_given_names=o.add_names=o.use_disambiguate_branch=o.add_year_suffix=true;
    o.given_rule=CE_CSL_DISAMBIGUATE_PRIMARY_NAME_INITIALS; return o;
}
static void partial_progress_and_suffix(void) {
    Model m=expanding_model(); Fixture f=fixture(&m,NULL,0,NULL,0);
    CeCslDisambiguationOptions o=all_methods(); CeCslDisambiguation *r=success(f,&m,&o);
    for(size_t i=0;i<4;++i) {
        const CeCslDisambiguationItem *item=ce_csl_disambiguation_item(r,i);
        assert(item->disambiguate && !strcmp(item->year_suffix,i%2?"b":"a"));
        assert(adjusted(r,i,CE_CSL_DISAMBIGUATION_FIRST,0).minimum_names==2);
    }
    assert(!ce_csl_disambiguation_item(r,4)->disambiguate);
    assert(!ce_csl_disambiguation_item(r,4)->year_suffix[0]);
    assert(!ce_csl_disambiguation_unresolved_count(r));
    ce_csl_disambiguation_free(r); fixture_free(f);

    m=model_of(2); m.items[0].branch="unique-a"; m.items[1].branch="unique-b";
    f=fixture(&m,NULL,0,NULL,0); o=(CeCslDisambiguationOptions){0};
    o.use_disambiguate_branch=o.add_year_suffix=true;
    r=success(f,&m,&o);
    assert(ce_csl_disambiguation_item(r,0)->disambiguate && !ce_csl_disambiguation_item(r,0)->year_suffix[0]);
    ce_csl_disambiguation_free(r); fixture_free(f);
}
static void invisible_subsequent_and_rebuild(void) {
    Model m=model_of(2); m.items[1].cited=false;
    Fixture f=fixture(&m,NULL,0,NULL,0); CeCslDisambiguationOptions o={0}; o.add_year_suffix=true;
    CeCslDisambiguation *r=success(f,&m,&o);
    assert(!ce_csl_disambiguation_item(r,0)->year_suffix[0] && !ce_csl_disambiguation_item(r,1));
    fixture_free(f);
    const size_t include=1, order[]={1,0}; f=fixture(&m,&include,1,order,2);
    assert(build(f,&m,&o,NULL,&r)==CE_CSL_DISAMBIGUATION_OK);
    assert(m.invisible && !strcmp(ce_csl_disambiguation_item(r,1)->year_suffix,"a"));
    assert(!strcmp(ce_csl_disambiguation_item(r,0)->year_suffix,"b"));
    fixture_free(f); f=fixture(&m,NULL,0,NULL,0);
    assert(build(f,&m,&o,NULL,&r)==CE_CSL_DISAMBIGUATION_OK);
    assert(!ce_csl_disambiguation_item(r,0)->year_suffix[0]);
    ce_csl_disambiguation_free(r); fixture_free(f);

    m=model_of(2);
    for(size_t i=0;i<2;++i) { m.items[i].people_count=2; m.items[i].first=2; }
    m.items[0].people[1]=person("One","A","A.One"); m.items[1].people[1]=person("Two","B","B.Two");
    f=fixture(&m,NULL,0,NULL,0); r=success(f,&m,&o);
    assert(!ce_csl_disambiguation_item(r,0)->year_suffix[0]); ce_csl_disambiguation_free(r);
    o.profiles=CE_CSL_DISAMBIGUATION_FIRST|CE_CSL_DISAMBIGUATION_SUBSEQUENT;
    r=success(f,&m,&o); assert(!strcmp(ce_csl_disambiguation_item(r,0)->year_suffix,"a"));
    assert(!ce_csl_disambiguation_unresolved_count(r)); ce_csl_disambiguation_free(r); fixture_free(f);
}
static void suffixes_and_unresolved(void) {
    Model m=model_of(54); Fixture f=fixture(&m,NULL,0,NULL,0);
    CeCslDisambiguationOptions o={0}; o.add_year_suffix=true;
    CeCslDisambiguation *r=success(f,&m,&o);
    assert(!strcmp(ce_csl_disambiguation_item(r,25)->year_suffix,"z"));
    assert(!strcmp(ce_csl_disambiguation_item(r,26)->year_suffix,"aa"));
    assert(!strcmp(ce_csl_disambiguation_item(r,51)->year_suffix,"az"));
    assert(!strcmp(ce_csl_disambiguation_item(r,52)->year_suffix,"ba"));
    assert(!ce_csl_disambiguation_unresolved_count(r)); ce_csl_disambiguation_free(r); fixture_free(f);
    m=model_of(2); f=fixture(&m,NULL,0,NULL,0); r=success(f,&m,NULL);
    assert(ce_csl_disambiguation_unresolved_count(r)==1);
    assert(ce_csl_disambiguation_item(r,0)->unresolved_group==ce_csl_disambiguation_item(r,1)->unresolved_group);
    ce_csl_disambiguation_free(r);
    m.items[0].ignore_suffix=m.items[1].ignore_suffix=true; r=success(f,&m,&o);
    assert(ce_csl_disambiguation_unresolved_count(r)==1); /* Provider really must render the suffix. */
    ce_csl_disambiguation_free(r); fixture_free(f);
    m=model_of(0); f=fixture(&m,NULL,0,NULL,0); r=success(f,&m,&o);
    assert(!ce_csl_disambiguation_unresolved_count(r) && !ce_csl_disambiguation_item(r,0));
    ce_csl_disambiguation_free(r); fixture_free(f);
}

typedef union FaultHeader { long double alignment; void *pointer; unsigned char padding[32]; } FaultHeader;
typedef struct Fault { size_t calls,fail_at,live; } Fault;
static void *fault_allocate(void *context,size_t size) {
    Fault *f=context; if(++f->calls==f->fail_at) return NULL;
    FaultHeader *p=malloc(sizeof(*p)+size); assert(p); ++f->live; return p+1;
}
static void fault_free(void *context,void *pointer) {
    Fault *f=context; assert(pointer && f->live); --f->live; free((FaultHeader *)pointer-1);
}
static bool cancelled(void *context) { Model *m=context; return m->calls>=m->cancel_after; }
static void failures(void) {
    Model m=expanding_model(); Fixture f=fixture(&m,NULL,0,NULL,0); CeCslDisambiguationOptions o=all_methods();
    o.profiles=CE_CSL_DISAMBIGUATION_FIRST|CE_CSL_DISAMBIGUATION_SUBSEQUENT|CE_CSL_DISAMBIGUATION_BIBLIOGRAPHY;
    Fault fault={0}; CeCslOutputAllocator allocator={&fault,fault_allocate,fault_free};
    CeCslDisambiguation *r=NULL;
    assert(build(f,&m,&o,&allocator,&r)==CE_CSL_DISAMBIGUATION_OK);
    size_t allocation_points=fault.calls; ce_csl_disambiguation_free(r); assert(!fault.live);
    r=success(f,&m,&o); CeCslDisambiguation *old=r;
    const CeCslDisambiguationItem *old_item=ce_csl_disambiguation_item(r,0);
    for(size_t i=1;i<=allocation_points;++i) {
        fault.calls=0; fault.fail_at=i; m.calls=0;
        CeCslDisambiguationStatus status=build(f,&m,&o,&allocator,&r);
        if(status!=CE_CSL_DISAMBIGUATION_OOM || fault.live)
            fprintf(stderr,"Failure point %zu/%zu status %d live %zu: %s\n",i,allocation_points,status,fault.live,error);
        assert(status==CE_CSL_DISAMBIGUATION_OOM && !fault.live && r==old);
        assert(ce_csl_disambiguation_item(r,0)==old_item && old_item->suffix_ordinal==1);
    }
    printf("Disambiguation allocation failure points: %zu\n",allocation_points);
    fault.fail_at=0; fault.calls=0;
    o.add_given_names=true; o.given_rule=CE_CSL_DISAMBIGUATE_BY_CITE;
    assert(build(f,&m,&o,&allocator,&r)==CE_CSL_DISAMBIGUATION_UNSUPPORTED && !fault.calls);
    for(int rule=CE_CSL_DISAMBIGUATE_ALL_NAMES;rule<CE_CSL_DISAMBIGUATE_PRIMARY_NAME_INITIALS;++rule) {
        o.given_rule=(CeCslGivenNameRule)rule;
        assert(build(f,&m,&o,&allocator,&r)==CE_CSL_DISAMBIGUATION_UNSUPPORTED && !fault.calls);
    }
    o=all_methods(); o.max_probes=1;
    assert(build(f,&m,&o,&allocator,&r)==CE_CSL_DISAMBIGUATION_LIMIT && !fault.live && r==old);
    o=all_methods(); o.max_names=1;
    assert(build(f,&m,&o,&allocator,&r)==CE_CSL_DISAMBIGUATION_LIMIT && !fault.live && r==old);
    o=all_methods(); o.max_changes=1;
    assert(build(f,&m,&o,&allocator,&r)==CE_CSL_DISAMBIGUATION_LIMIT && !fault.live && r==old);
    o=all_methods(); o.max_emitted_bytes=1;
    assert(build(f,&m,&o,&allocator,&r)==CE_CSL_DISAMBIGUATION_LIMIT && !fault.live && r==old);
    o=all_methods(); o.max_compared_bytes=1;
    assert(build(f,&m,&o,&allocator,&r)==CE_CSL_DISAMBIGUATION_LIMIT && !fault.live && r==old);
    o=all_methods(); o.max_workspace_bytes=1024;
    assert(build(f,&m,&o,&allocator,&r)==CE_CSL_DISAMBIGUATION_LIMIT && !fault.live && r==old);
    o=all_methods(); o.max_work_units=1;
    assert(build(f,&m,&o,&allocator,&r)==CE_CSL_DISAMBIGUATION_LIMIT && !fault.live && r==old);
    o=all_methods(); m.calls=0; m.cancel_after=2; o.cancelled=cancelled; o.cancel_context=&m;
    assert(build(f,&m,&o,&allocator,&r)==CE_CSL_DISAMBIGUATION_CANCELLED && !fault.live && r==old);
    o=all_methods(); m.provider_error=true;
    assert(build(f,&m,&o,&allocator,&r)==CE_CSL_DISAMBIGUATION_PROVIDER_ERROR && !fault.live && r==old);
    m.provider_error=false; m.invalid_key=true;
    assert(build(f,&m,&o,&allocator,&r)==CE_CSL_DISAMBIGUATION_PROVIDER_ERROR && !fault.live && r==old);
    m.invalid_key=false; m.stale_minimum=true;
    assert(build(f,&m,&o,&allocator,&r)==CE_CSL_DISAMBIGUATION_PROVIDER_ERROR && !fault.live && r==old);
    ce_csl_disambiguation_free(r); fixture_free(f);
}
static void invalid_options_and_nonmonotone(void) {
    Model m=model_of(3);
    for(size_t i=0;i<2;++i) {
        m.items[i].people[0]=person("A","A","A.A");
        m.items[i].people[1]=i?person("C","C","C.C"):person("B","B","B.B");
        m.items[i].people_count=2;
    }
    m.items[2].people[0]=person("A+B","Other","O.A+B");
    Fixture f=fixture(&m,NULL,0,NULL,0); CeCslDisambiguation *r=success(f,&m,NULL), *old=r;
    CeCslDisambiguationOptions o={0}; o.add_names=true;
    assert(build(f,&m,&o,NULL,&r)==CE_CSL_DISAMBIGUATION_UNSUPPORTED && r==old);
    assert(strstr(error,"merge"));
    o=(CeCslDisambiguationOptions){0}; o.given_rule=(CeCslGivenNameRule)99;
    assert(build(f,&m,&o,NULL,&r)==CE_CSL_DISAMBIGUATION_INVALID && r==old);
    o=(CeCslDisambiguationOptions){0}; o.profiles=CE_CSL_DISAMBIGUATION_BIBLIOGRAPHY;
    assert(build(f,&m,&o,NULL,&r)==CE_CSL_DISAMBIGUATION_INVALID && r==old);
    o.profiles=8;
    assert(build(f,&m,&o,NULL,&r)==CE_CSL_DISAMBIGUATION_INVALID && r==old);
    o=(CeCslDisambiguationOptions){0}; o.max_work_units=CE_CSL_DISAMBIGUATION_MAX_WORK+1;
    assert(build(f,&m,&o,NULL,&r)==CE_CSL_DISAMBIGUATION_INVALID && r==old);
    CeCslDisambiguationProvider provider={&m,render,release};
    assert(ce_csl_disambiguation_build(NULL,f.registry,NULL,&provider,NULL,&r,error,sizeof(error))==CE_CSL_DISAMBIGUATION_INVALID);
    assert(ce_csl_disambiguation_build(f.input,NULL,NULL,&provider,NULL,&r,error,sizeof(error))==CE_CSL_DISAMBIGUATION_INVALID);
    provider.release=NULL;
    assert(ce_csl_disambiguation_build(f.input,f.registry,NULL,&provider,NULL,&r,error,sizeof(error))==CE_CSL_DISAMBIGUATION_INVALID);
    assert(!ce_csl_disambiguation_item(NULL,0) && !ce_csl_disambiguation_unresolved_count(NULL));
    assert(!ce_csl_disambiguation_adjust_names(NULL,CE_CSL_DISAMBIGUATION_FIRST,NULL,NULL));
    ce_csl_disambiguation_free(r); ce_csl_disambiguation_free(NULL); fixture_free(f);
}
int main(void) {
    global_primary(); partial_progress_and_suffix(); invisible_subsequent_and_rebuild();
    suffixes_and_unresolved(); failures(); invalid_options_and_nonmonotone();
    puts("Typed-provider disambiguation driver passed (not an APA/finalizer conformance test)");
    return 0;
}
