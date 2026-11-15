#include "csl_variable.h"
#include "csl_date.h"
#include "csl_number_field.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define COUNT(a) (sizeof(a)/sizeof((a)[0]))
typedef struct Fixture {
    cJSON *item_json, *cite_json;
    CeCslInputItem item; CeCslInputCite cite;
    CeCslGeneratedVariables generated;
    CeCslVariableContext context;
} Fixture;
static void fixture(Fixture *f, const char *item, const char *cite) {
    *f = (Fixture){0}; f->item_json = cJSON_Parse(item ? item : "{}"); assert(f->item_json);
    f->item.data = f->item_json; f->item.id = "fixture"; f->context.item = &f->item;
    if (cite) { f->cite_json = cJSON_Parse(cite); assert(f->cite_json); f->cite.data = f->cite_json; f->context.cite = &f->cite; }
    f->context.generated = &f->generated;
}
static void fixture_free(Fixture *f) { cJSON_Delete(f->item_json); cJSON_Delete(f->cite_json); }
static CeCslVariableValue get(Fixture *f, const char *name, CeCslVariableForm form) {
    CeCslVariableValue value = {0}; char error[256];
    CeCslVariableStatus status = ce_csl_variable_resolve(&f->context,name,form,&value,error,sizeof(error));
    if (status != CE_CSL_VARIABLE_OK) fprintf(stderr,"Variable %s failed (%s): %s\n",name,ce_csl_variable_status_string(status),error);
    assert(status == CE_CSL_VARIABLE_OK && !*error); return value;
}
static void text_is(const CeCslVariableValue *value, const char *text) {
    size_t size = 123; const char *actual = ce_csl_variable_text(value,&size);
    if (!actual || size != strlen(text) || memcmp(actual,text,size)) fprintf(stderr,"Variable text expected <%s>, actual <%.*s>\n",text,(int)size,actual ? actual : "NULL");
    assert(actual && size == strlen(text) && !memcmp(actual,text,size));
}
static void facts_is(Fixture *f, const char *name, bool present, bool numeric, bool uncertain) {
    char error[256]; CeCslVariableFacts facts = {0};
    if (!ce_csl_variable_condition(&f->context,name,&facts,error,sizeof(error))) { fprintf(stderr,"Facts %s failed: %s\n",name,error); abort(); }
    assert(facts.present == present && facts.numeric == numeric && facts.uncertain_date == uncertain && !*error);
}
static void expect_failure(Fixture *f, const char *name, CeCslVariableForm form, CeCslVariableStatus expected) {
    char error[256]; CeCslVariableValue value = {0};
    value.canonical_name = "sentinel"; value.state = CE_CSL_VARIABLE_VALUE; value.kind = CE_CSL_VARIABLE_TEXT;
    value.borrowed_text = "unchanged"; value.text_size = 9;
    CeCslVariableValue before = value;
    CeCslVariableStatus status = ce_csl_variable_resolve(&f->context,name,form,&value,error,sizeof(error));
    if (status != expected) fprintf(stderr,"Failure %s expected %s got %s: %s\n",name,ce_csl_variable_status_string(expected),ce_csl_variable_status_string(status),error);
    assert(status == expected && *error && !memcmp(&value,&before,sizeof(value)));
}
static void test_metadata(void) {
    Fixture f;
    fixture(&f,"{\"title\":\"Full Title\",\"title-short\":\"\",\"container-title\":\"Full Journal\",\"event\":\"Legacy\",\"event-title\":\"\",\"page\":\"S001–S009\",\"volume\":0,\"issue\":\"001\",\"genre\":null,\"medium\":\" \\t \",\"archive_location\":\"Box 1\"}",NULL);
    CeCslVariableValue v = get(&f,"title",CE_CSL_VARIABLE_LONG); text_is(&v,"Full Title");
    v = get(&f,"title",CE_CSL_VARIABLE_SHORT); assert(v.state == CE_CSL_VARIABLE_EMPTY); text_is(&v,"");
    v = get(&f,"title-short",CE_CSL_VARIABLE_LONG); assert(v.state == CE_CSL_VARIABLE_EMPTY && !strcmp(v.canonical_name,"title"));
    v = get(&f,"container-title",CE_CSL_VARIABLE_SHORT); text_is(&v,"Full Journal");
    v = get(&f,"event",CE_CSL_VARIABLE_LONG); assert(v.state == CE_CSL_VARIABLE_EMPTY && !strcmp(v.canonical_name,"event-title"));
    v = get(&f,"genre",CE_CSL_VARIABLE_LONG); assert(v.state == CE_CSL_VARIABLE_EMPTY && cJSON_IsNull(v.node));
    v = get(&f,"publisher",CE_CSL_VARIABLE_LONG); assert(v.state == CE_CSL_VARIABLE_MISSING && !v.node);
    v = get(&f,"medium",CE_CSL_VARIABLE_LONG); text_is(&v," \t "); assert(v.state == CE_CSL_VARIABLE_VALUE);
    v = get(&f,"volume",CE_CSL_VARIABLE_LONG); text_is(&v,"0"); assert(v.state == CE_CSL_VARIABLE_VALUE);
    v = get(&f,"issue",CE_CSL_VARIABLE_LONG); text_is(&v,"001"); assert(!v.text_is_inline);
    facts_is(&f,"title",true,false,false); facts_is(&f,"volume",true,true,false);
    facts_is(&f,"genre",false,false,false); facts_is(&f,"medium",true,false,false);
    expect_failure(&f,"archive-location",CE_CSL_VARIABLE_LONG,CE_CSL_VARIABLE_UNKNOWN);
    expect_failure(&f,"doi",CE_CSL_VARIABLE_LONG,CE_CSL_VARIABLE_UNKNOWN);
    fixture_free(&f);
    fixture(&f,"{\"event\":\"旧版活动\",\"title-short\":\"Only Short\",\"container-title-short\":null}",NULL);
    v = get(&f,"event-title",CE_CSL_VARIABLE_LONG); text_is(&v,"旧版活动");
    v = get(&f,"title",CE_CSL_VARIABLE_LONG); assert(v.state == CE_CSL_VARIABLE_MISSING);
    v = get(&f,"title",CE_CSL_VARIABLE_SHORT); text_is(&v,"Only Short");
    v = get(&f,"container-title",CE_CSL_VARIABLE_SHORT); assert(v.state == CE_CSL_VARIABLE_EMPTY);
    facts_is(&f,"title",false,false,false); fixture_free(&f);
}
static void test_numeric(void) {
    const char *values[] = {"0","-0","9007199254740991","-9007199254740991","\"00001\"","\"184467440737095516160000001\"","\"1.25\"","\"D2, 03 & L4d\""};
    const char *text[] = {"0","0","9007199254740991","-9007199254740991","00001","184467440737095516160000001","1.25","D2, 03 & L4d"};
    for (size_t i = 0; i < COUNT(values); ++i) {
        char input[256]; Fixture f; snprintf(input,sizeof(input),"{\"edition\":%s}",values[i]); fixture(&f,input,NULL);
        CeCslVariableValue v = get(&f,"edition",CE_CSL_VARIABLE_LONG), copy = v;
        memset(&v,0,sizeof(v)); text_is(&copy,text[i]);
        CeCslNumberFieldInfo info; char error[256];
        assert(ce_csl_number_field_scan(text[i],strlen(text[i]),&info,error,sizeof(error)));
        facts_is(&f,"edition",true,info.numeric,false); fixture_free(&f);
    }
    const char *unsupported[] = {"1.25","9007199254740992","-9007199254740992","1e99"};
    for (size_t i = 0; i < COUNT(unsupported); ++i) {
        char input[256]; Fixture f; snprintf(input,sizeof(input),"{\"volume\":%s}",unsupported[i]); fixture(&f,input,NULL);
        expect_failure(&f,"volume",CE_CSL_VARIABLE_LONG,CE_CSL_VARIABLE_UNSUPPORTED); fixture_free(&f);
    }
    Fixture f; fixture(&f,"{\"title\":\"2nd\",\"medium\":\"2nd edition\"}",NULL);
    facts_is(&f,"title",true,true,false); facts_is(&f,"medium",true,false,false); fixture_free(&f);
}
static void test_generated_and_cites(void) {
    Fixture f; fixture(&f,"{\"citation-number\":999,\"first-reference-note-number\":7,\"year-suffix\":\"source-a\",\"citation-label\":\"SourceLabel\",\"locator\":\"private-item-locator\"}","{\"locator\":\"23-25\"}");
    expect_failure(&f,"citation-number",CE_CSL_VARIABLE_LONG,CE_CSL_VARIABLE_UNRESOLVED);
    expect_failure(&f,"first-reference-note-number",CE_CSL_VARIABLE_LONG,CE_CSL_VARIABLE_UNRESOLVED);
    CeCslVariableValue v = get(&f,"year-suffix",CE_CSL_VARIABLE_LONG); text_is(&v,"source-a"); assert(!v.derived);
    v = get(&f,"citation-label",CE_CSL_VARIABLE_LONG); text_is(&v,"SourceLabel");
    f.generated.citation_number = (CeCslGeneratedNumber){true,true,UINT64_MAX};
    v = get(&f,"citation-number",CE_CSL_VARIABLE_LONG); text_is(&v,"18446744073709551615"); assert(v.derived && !v.node);
    f.generated.first_reference_note_number = (CeCslGeneratedNumber){true,false,0};
    v = get(&f,"first-reference-note-number",CE_CSL_VARIABLE_LONG); assert(v.state == CE_CSL_VARIABLE_MISSING);
    f.generated.first_reference_note_number = (CeCslGeneratedNumber){true,true,23};
    v = get(&f,"first-reference-note-number",CE_CSL_VARIABLE_LONG); text_is(&v,"23");
    f.generated.year_suffix = (CeCslGeneratedText){true,"",0};
    v = get(&f,"year-suffix",CE_CSL_VARIABLE_LONG); assert(v.state == CE_CSL_VARIABLE_EMPTY && v.derived);
    f.generated.citation_label = (CeCslGeneratedText){true,"Assigned",8};
    v = get(&f,"citation-label",CE_CSL_VARIABLE_LONG); text_is(&v,"Assigned");
    f.generated.citation_label = (CeCslGeneratedText){true,NULL,0};
    v = get(&f,"citation-label",CE_CSL_VARIABLE_LONG); assert(v.state == CE_CSL_VARIABLE_MISSING);
    v = get(&f,"locator",CE_CSL_VARIABLE_LONG); text_is(&v,"23-25");
    cJSON *other_json = cJSON_Parse("{\"locator\":\"99\"}"), *blank_json = cJSON_Parse("{\"label\":\"page\"}"); assert(other_json && blank_json);
    CeCslInputCite other = {0,other_json}, blank = {0,blank_json};
    f.context.cite = &other; v = get(&f,"locator",CE_CSL_VARIABLE_LONG); text_is(&v,"99");
    f.context.cite = &blank; v = get(&f,"locator",CE_CSL_VARIABLE_LONG); assert(v.state == CE_CSL_VARIABLE_MISSING);
    f.context.cite = NULL; v = get(&f,"locator",CE_CSL_VARIABLE_LONG); assert(v.state == CE_CSL_VARIABLE_MISSING);
    f.generated.citation_number.value = 0;
    expect_failure(&f,"citation-number",CE_CSL_VARIABLE_LONG,CE_CSL_VARIABLE_INVALID);
    f.generated.year_suffix = (CeCslGeneratedText){true,"a\0b",3};
    expect_failure(&f,"year-suffix",CE_CSL_VARIABLE_LONG,CE_CSL_VARIABLE_INVALID);
    f.generated.year_suffix = (CeCslGeneratedText){true,NULL,2};
    expect_failure(&f,"year-suffix",CE_CSL_VARIABLE_LONG,CE_CSL_VARIABLE_INVALID);
    cJSON_Delete(other_json); cJSON_Delete(blank_json); fixture_free(&f);
    fixture(&f,NULL,NULL);
    expect_failure(&f,"year-suffix",CE_CSL_VARIABLE_LONG,CE_CSL_VARIABLE_UNRESOLVED);
    expect_failure(&f,"citation-label",CE_CSL_VARIABLE_LONG,CE_CSL_VARIABLE_UNRESOLVED); fixture_free(&f);
}
static void test_page_first(void) {
    const char *source[] = {"\"  S001–S009, S012  \"","\"第２\"","\"中001文 - 中009文\"","123","\"184467440737095516160001-184467440737095516160005\""};
    const char *first[] = {"S001",NULL,"中001文","123","184467440737095516160001"};
    for (size_t i = 0; i < COUNT(source); ++i) {
        Fixture f; char input[256]; snprintf(input,sizeof(input),"{\"page\":%s}",source[i]); fixture(&f,input,NULL);
        if (!first[i]) expect_failure(&f,"page-first",CE_CSL_VARIABLE_LONG,CE_CSL_VARIABLE_UNSUPPORTED);
        else {
            CeCslVariableValue v = get(&f,"page-first",CE_CSL_VARIABLE_LONG), copy = v;
            assert(v.derived && v.dependency_count == 1 && !strcmp(v.dependencies[0],"page"));
            memset(&v,0,sizeof(v)); text_is(&copy,first[i]);
        }
        fixture_free(&f);
    }
    Fixture f; fixture(&f,"{\"page\":\"ix-xii\"}",NULL);
    expect_failure(&f,"page-first",CE_CSL_VARIABLE_LONG,CE_CSL_VARIABLE_UNSUPPORTED); fixture_free(&f);
    fixture(&f,"{\"page\":\"ix-xii\",\"page-first\":\"ix\"}",NULL);
    CeCslVariableValue v = get(&f,"page-first",CE_CSL_VARIABLE_LONG); text_is(&v,"ix"); assert(!v.derived); fixture_free(&f);
    fixture(&f,"{\"page\":\"bad range\",\"page-first\":\"\"}",NULL);
    v = get(&f,"page-first",CE_CSL_VARIABLE_LONG); assert(v.state == CE_CSL_VARIABLE_EMPTY); fixture_free(&f);
    fixture(&f,NULL,NULL); v = get(&f,"page-first",CE_CSL_VARIABLE_LONG); assert(v.state == CE_CSL_VARIABLE_MISSING); fixture_free(&f);
}
static void test_names(void) {
    Fixture f; fixture(&f,"{\"editor\":[{\"family\":\"Doe\",\"given\":\"John\"}],\"translator\":[{\"given\":\"John\",\"family\":\"Doe\",\"comma-suffix\":\"false\"}]}",NULL);
    CeCslVariableValue v = get(&f,"editor-translator",CE_CSL_VARIABLE_LONG);
    assert(v.kind == CE_CSL_VARIABLE_NAMES && v.state == CE_CSL_VARIABLE_VALUE && v.derived && v.dependency_count == 2);
    assert(v.node == cJSON_GetObjectItemCaseSensitive(f.item_json,"editor"));
    facts_is(&f,"editor-translator",true,false,false); fixture_free(&f);
    const char *different[] = {
        "{\"editor\":[{\"family\":\"Doe\",\"given\":\"John\"}],\"translator\":[{\"family\":\"Doe\",\"given\":\"James\"}]}",
        "{\"editor\":[{\"family\":\"Doe\"}],\"translator\":[{\"family\":\"Doe\",\"parse-names\":false}]}",
        "{\"editor\":[{\"family\":\"A\"},{\"family\":\"B\"}],\"translator\":[{\"family\":\"B\"},{\"family\":\"A\"}]}",
        "{\"editor\":[{\"family\":\"Doe\"},{}],\"translator\":[{\"family\":\"Doe\"}]}",
        "{\"editor\":[{\"family\":\"Gogh\",\"non-dropping-particle\":\"van\"}],\"translator\":[{\"family\":\"Gogh\"}]}",
        "{\"editor\":[{}],\"translator\":[{\"literal\":\"\"}]}",
        "{\"editor\":null,\"translator\":[]}"
    };
    for (size_t i = 0; i < COUNT(different); ++i) {
        fixture(&f,different[i],NULL); v = get(&f,"editor-translator",CE_CSL_VARIABLE_LONG); assert(v.state == CE_CSL_VARIABLE_MISSING); fixture_free(&f);
    }
    fixture(&f,"{\"editor-translator\":[],\"editor\":\"bad names\",\"translator\":\"bad names\"}",NULL);
    v = get(&f,"editor-translator",CE_CSL_VARIABLE_LONG); assert(v.state == CE_CSL_VARIABLE_EMPTY && !v.derived); fixture_free(&f);
    fixture(&f,"{\"editor-translator\":null,\"editor\":[{\"family\":\"Doe\"}],\"translator\":[{\"family\":\"Doe\"}]}",NULL);
    v = get(&f,"editor-translator",CE_CSL_VARIABLE_LONG); assert(v.state == CE_CSL_VARIABLE_EMPTY); fixture_free(&f);
    fixture(&f,"{\"author\":[{}, {\"literal\":\"\"}],\"editor-translator\":[{\"literal\":\"Organisation\"}]}",NULL);
    facts_is(&f,"author",false,false,false); facts_is(&f,"editor-translator",true,false,false);
    expect_failure(&f,"author",CE_CSL_VARIABLE_SHORT,CE_CSL_VARIABLE_INVALID); fixture_free(&f);
}
static void test_dates(void) {
    Fixture f; fixture(&f,"{\"issued\":{\"date-parts\":[[2000]],\"circa\":\"true\"},\"original-date\":{},\"accessed\":null,\"submitted\":{\"circa\":true},\"event-date\":{\"literal\":\"Spring someday\"}}",NULL);
    facts_is(&f,"issued",true,false,true); facts_is(&f,"original-date",false,false,false);
    facts_is(&f,"accessed",false,false,false); facts_is(&f,"submitted",false,false,false);
    facts_is(&f,"event-date",true,false,false);
    CeCslVariableValue v = get(&f,"issued",CE_CSL_VARIABLE_LONG); assert(v.kind == CE_CSL_VARIABLE_DATE && v.node == cJSON_GetObjectItemCaseSensitive(f.item_json,"issued"));
    size_t size = 42; assert(!ce_csl_variable_text(&v,&size) && !size);
    expect_failure(&f,"issued",CE_CSL_VARIABLE_SHORT,CE_CSL_VARIABLE_INVALID); fixture_free(&f);
    fixture(&f,"{\"issued\":{\"raw\":\"circa the summer of 2000\"}}",NULL);
    expect_failure(&f,"issued",CE_CSL_VARIABLE_LONG,CE_CSL_VARIABLE_INVALID); fixture_free(&f);
    const char *inputs[] = {"{}","{\"date-parts\":[]}","{\"date-parts\":[[0]]}","{\"date-parts\":[[0],[2000]]}","{\"literal\":\" \",\"circa\":true}"};
    const bool present[] = {false,false,false,true,true};
    for (size_t i = 0; i < COUNT(inputs); ++i) {
        cJSON *date = cJSON_Parse(inputs[i]); CeCslDateInfo info = {0}; char error[128]; assert(date);
        assert(ce_csl_date_inspect(date,&info,error,sizeof(error))); assert(info.present == present[i]); cJSON_Delete(date);
    }
    cJSON *bad = cJSON_Parse("{\"literal\":\"bad\\u0001text\"}"); CeCslDateInfo info = {true,true}; char error[128]; assert(bad);
    assert(!ce_csl_date_inspect(bad,&info,error,sizeof(error)) && info.present && info.circa && *error); cJSON_Delete(bad);
}
typedef struct Callbacks {
    const char *blocked;
    const char *last;
    size_t suppression_calls, short_calls;
    bool suppress_error;
    CeCslVariableStatus short_status;
    CeCslVariableShortValue short_result;
} Callbacks;
static bool suppressed(void *context, const char *name, bool *blocked, char *error, size_t capacity) {
    Callbacks *c = context; ++c->suppression_calls; c->last = name;
    if (c->suppress_error) { if (error && capacity) *error = 0; return false; }
    *blocked = c->blocked && !strcmp(c->blocked,name); return true;
}
static CeCslVariableStatus abbreviation(void *context, const char *name, const cJSON *long_value,
    CeCslVariableShortValue *out, char *error, size_t capacity) {
    Callbacks *c = context; (void)long_value; (void)error; (void)capacity;
    ++c->short_calls; c->last = name; *out = c->short_result; return c->short_status;
}
static void test_callbacks(void) {
    Fixture f; fixture(&f,"{\"title\":\"Long\",\"title-short\":\"\",\"publisher\":\"Long Publisher\",\"event\":\"Old Event\",\"page\":\"1-3\",\"editor\":[{\"family\":\"Doe\"}],\"translator\":[{\"family\":\"Doe\"}]}",NULL);
    Callbacks c = {0}; f.context.context = &c; f.context.suppressed = suppressed; f.context.short_value = abbreviation;
    c.short_result = (CeCslVariableShortValue){true,"Pub.",4};
    CeCslVariableValue v = get(&f,"publisher",CE_CSL_VARIABLE_SHORT); text_is(&v,"Pub."); assert(c.short_calls == 1);
    v = get(&f,"title",CE_CSL_VARIABLE_SHORT); assert(v.state == CE_CSL_VARIABLE_EMPTY && c.short_calls == 1);
    f.generated.citation_number = (CeCslGeneratedNumber){true,true,10};
    v = get(&f,"citation-number",CE_CSL_VARIABLE_SHORT); text_is(&v,"Pub."); assert(v.derived && !v.text_is_inline);
    f.generated.citation_number.present = false;
    size_t calls = c.short_calls;
    v = get(&f,"citation-number",CE_CSL_VARIABLE_SHORT); assert(v.state == CE_CSL_VARIABLE_MISSING && c.short_calls == calls);
    c.short_result = (CeCslVariableShortValue){true,"",0};
    v = get(&f,"publisher",CE_CSL_VARIABLE_SHORT); assert(v.state == CE_CSL_VARIABLE_EMPTY);
    c.short_result = (CeCslVariableShortValue){0};
    v = get(&f,"publisher",CE_CSL_VARIABLE_SHORT); text_is(&v,"Long Publisher");
    c.blocked = "event-title"; v = get(&f,"event",CE_CSL_VARIABLE_LONG); assert(v.state == CE_CSL_VARIABLE_SUPPRESSED && !strcmp(c.last,"event-title"));
    c.blocked = "title"; v = get(&f,"title-short",CE_CSL_VARIABLE_LONG); assert(v.state == CE_CSL_VARIABLE_SUPPRESSED && !strcmp(c.last,"title"));
    c.blocked = "editor"; v = get(&f,"editor-translator",CE_CSL_VARIABLE_LONG); assert(v.state == CE_CSL_VARIABLE_SUPPRESSED); facts_is(&f,"editor-translator",false,false,false);
    c.blocked = "page"; v = get(&f,"page-first",CE_CSL_VARIABLE_LONG); assert(v.state == CE_CSL_VARIABLE_SUPPRESSED);
    c.blocked = "citation-number"; v = get(&f,"citation-number",CE_CSL_VARIABLE_LONG); assert(v.state == CE_CSL_VARIABLE_SUPPRESSED);
    c.blocked = NULL; c.suppress_error = true;
    expect_failure(&f,"title",CE_CSL_VARIABLE_LONG,CE_CSL_VARIABLE_INVALID);
    CeCslVariableFacts facts = {true,true,true}; char error[256];
    assert(!ce_csl_variable_condition(&f.context,"title",&facts,error,sizeof(error)) && facts.present && facts.numeric && facts.uncertain_date && *error);
    c.suppress_error = false; c.short_status = CE_CSL_VARIABLE_UNSUPPORTED;
    expect_failure(&f,"publisher",CE_CSL_VARIABLE_SHORT,CE_CSL_VARIABLE_UNSUPPORTED);
    c.short_status = (CeCslVariableStatus)999;
    expect_failure(&f,"publisher",CE_CSL_VARIABLE_SHORT,CE_CSL_VARIABLE_INVALID);
    c.short_status = CE_CSL_VARIABLE_OK; c.short_result = (CeCslVariableShortValue){true,"x\0y",3};
    expect_failure(&f,"publisher",CE_CSL_VARIABLE_SHORT,CE_CSL_VARIABLE_INVALID);
    fixture_free(&f);
    fixture(&f,"{\"editor\":\"bad\",\"translator\":\"bad\"}",NULL);
    c = (Callbacks){.blocked="editor"}; f.context.suppressed = suppressed; f.context.context = &c;
    v = get(&f,"editor-translator",CE_CSL_VARIABLE_LONG); assert(v.state == CE_CSL_VARIABLE_SUPPRESSED);
    c.blocked = "translator";
    v = get(&f,"editor-translator",CE_CSL_VARIABLE_LONG); assert(v.state == CE_CSL_VARIABLE_SUPPRESSED);
    c.blocked = NULL; expect_failure(&f,"editor-translator",CE_CSL_VARIABLE_LONG,CE_CSL_VARIABLE_INVALID);
    fixture_free(&f);
}
static void test_invalid_immutable(void) {
    const char *inputs[] = {
        "{\"title\":123}", "{\"title\":true}", "{\"title\":{}}", "{\"title\":[1]}",
        "{\"title\":\"bad\\u0001text\"}", "{\"author\":[{\"given\":\"No family\"}]}",
        "{\"author\":[{\"family\":\"Doe\",\"unknown\":1}]}", "{\"issued\":[]}",
        "{\"issued\":{\"date-parts\":[[2000,13]]}}"
    };
    const char *names[] = {"title","title","title","title","title","author","author","issued","issued"};
    for (size_t i = 0; i < COUNT(inputs); ++i) {
        Fixture f; fixture(&f,inputs[i],NULL); char *before = cJSON_PrintUnformatted(f.item_json); assert(before);
        expect_failure(&f,names[i],CE_CSL_VARIABLE_LONG,CE_CSL_VARIABLE_INVALID);
        char *after = cJSON_PrintUnformatted(f.item_json); assert(after && !strcmp(before,after));
        cJSON_free(before); cJSON_free(after); fixture_free(&f);
    }
    Fixture f; fixture(&f,NULL,NULL);
    char *large = malloc(CE_CSL_VARIABLE_MAX_TEXT_BYTES+2); assert(large);
    memset(large,'x',CE_CSL_VARIABLE_MAX_TEXT_BYTES+1); large[CE_CSL_VARIABLE_MAX_TEXT_BYTES+1] = 0;
    assert(cJSON_AddStringToObject(f.item_json,"title",large));
    expect_failure(&f,"title",CE_CSL_VARIABLE_LONG,CE_CSL_VARIABLE_LIMIT); free(large);
    char name[65]; memset(name,'a',64); name[64] = 0;
    expect_failure(&f,name,CE_CSL_VARIABLE_LONG,CE_CSL_VARIABLE_LIMIT);
    expect_failure(&f,"",CE_CSL_VARIABLE_LONG,CE_CSL_VARIABLE_UNKNOWN); fixture_free(&f);
}
static void test_condition_integration(void) {
    Fixture f; fixture(&f,"{\"edition\":\"2nd\",\"title\":\"Title\",\"issued\":{\"date-parts\":[[2000]],\"circa\":true}}",NULL);
    const char *source = "<choose xmlns='" CE_CSL_NAMESPACE "'><if is-numeric='edition' variable='title' is-uncertain-date='issued'><text value='match'/></if><else><text value='other'/></else></choose>";
    char error[256]; CeCslXml *x = ce_csl_xml_parse(source,strlen(source),error,sizeof(error)); assert(x);
    CeCslConditionContext ctx = {.context=&f.context,.variable=ce_csl_variable_condition}; const CeCslXmlNode *branch = NULL;
    assert(ce_csl_choose_branch(x->root,&ctx,&branch,error,sizeof(error)) && ce_csl_xml_is(branch,"if"));
    Callbacks c = {.blocked="title"}; f.context.suppressed = suppressed; f.context.context = &c;
    assert(ce_csl_choose_branch(x->root,&ctx,&branch,error,sizeof(error)) && ce_csl_xml_is(branch,"else"));
    ce_csl_xml_free(x); fixture_free(&f);
}
static void inventory_tokens(Fixture *f, const char *words, CeCslVariableForm form, size_t *count) {
    for (const char *s = words; s && *s;) {
        while (*s == ' ') ++s;
        if (!*s) break;
        const char *end = strchr(s,' '); size_t n = end ? (size_t)(end-s) : strlen(s); char token[64];
        assert(n < sizeof(token)); memcpy(token,s,n); token[n] = 0;
        CeCslVariableValue value = get(f,token,form); assert(value.state == CE_CSL_VARIABLE_MISSING); ++*count;
        s += n;
    }
}
static void generated_empty(Fixture *f) {
    f->generated.citation_number.resolved = f->generated.first_reference_note_number.resolved = true;
    f->generated.year_suffix.resolved = f->generated.citation_label.resolved = true;
}
static void inventory_walk(Fixture *f, const CeCslXmlNode *node, size_t *count) {
    for (; node; node = node->next) {
        const char *attributes[] = {"variable","is-numeric","is-uncertain-date"};
        for (size_t i = 0; i < COUNT(attributes); ++i) {
            const char *value = ce_csl_xml_attribute(node,attributes[i]);
            if (value) inventory_tokens(f,value,CE_CSL_VARIABLE_LONG,count);
        }
        const char *form = ce_csl_xml_attribute(node,"form");
        if (ce_csl_xml_is(node,"text") && form && !strcmp(form,"short"))
            inventory_tokens(f,ce_csl_xml_attribute(node,"variable"),CE_CSL_VARIABLE_SHORT,count);
        inventory_walk(f,node->children,count);
    }
}
static void test_inventory(int argc, char **argv) {
    const char *all73 = "DOI URL accessed archive archive-place archive_collection archive_location author authority available-date chair chapter-number citation-number collection-editor collection-number collection-title compiler composer container-author container-title contributor curator director edition editor editor-translator editorial-director event event-date event-place event-title executive-producer genre guest host illustrator interviewer issue issued locator medium narrator number number-of-volumes organizer original-date original-publisher original-title page page-first part-number part-title performer producer publisher publisher-place recipient references reviewed-author reviewed-genre reviewed-title script-writer section series-creator status submitted supplement-number title translator version volume volume-title year-suffix";
    Fixture f; fixture(&f,NULL,NULL); generated_empty(&f); size_t count = 0;
    inventory_tokens(&f,all73,CE_CSL_VARIABLE_LONG,&count); assert(count == 73);
    for (int i = 1; i < argc; ++i) {
        FILE *file = fopen(argv[i],"rb"); assert(file && !fseek(file,0,SEEK_END)); long size = ftell(file); assert(size > 0 && size <= (long)CE_CSL_XML_MAX_BYTES); rewind(file);
        char *text = malloc((size_t)size+1), error[256]; assert(text && fread(text,1,(size_t)size,file) == (size_t)size && !fclose(file)); text[size] = 0;
        CeCslXml *x = ce_csl_xml_parse(text,(size_t)size,error,sizeof(error)); assert(x);
        size_t uses = 0; inventory_walk(&f,x->root,&uses); assert(uses);
        printf("Resolved %zu real-style variable requests: %s\n",uses,argv[i]); ce_csl_xml_free(x); free(text);
    }
    fixture_free(&f);
}
int main(int argc, char **argv) {
    test_metadata(); test_numeric(); test_generated_and_cites(); test_page_first(); test_names(); test_dates();
    test_callbacks(); test_invalid_immutable(); test_condition_integration(); test_inventory(argc,argv);
    puts("CSL variable typing, absence/empty/suppression, exact counters, aliases, structured derivation and atomic facts passed");
    return 0;
}
