#include "bibliography_archive.h"
#include "csl_json.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
/* The implementation is included to exercise the one default-malloc plain
 * export allocation as well as the public custom-allocator failures below.
 * Do not separately compile bibliography_archive.c into this executable. */
static bool fail_default_malloc;
static void *archive_malloc(size_t size) { return fail_default_malloc?NULL:malloc(size); }
#define malloc archive_malloc
#include "../src/bibliography_archive.c"
#undef malloc
static char error[512];
static size_t checks;
static cJSON *atom(const char *entries) {
    cJSON *node=cJSON_CreateObject(),*attrs=cJSON_CreateObject(); assert(node && attrs);
    assert(cJSON_AddStringToObject(node,"type","zoteroEndnoteSection"));
    assert(cJSON_AddItemToObject(node,"attrs",attrs));
    assert(cJSON_AddStringToObject(attrs,"entriesJson",entries));
    assert(cJSON_AddStringToObject(attrs,"cslRenderJson","FORGED PROCESSOR CACHE MUST NEVER DISPLAY"));
    assert(cJSON_AddStringToObject(attrs,"unknown","preserved"));
    return node;
}
static CeBibliographyArchive *decode(const char *entries) {
    cJSON *node=atom(entries); char *before=ce_csl_json_print_exact(node); assert(before);
    CeBibliographyArchive *archive=NULL;
    if(!ce_bibliography_archive_decode(node,NULL,&archive,error,sizeof error)) { fprintf(stderr,"decode: %s\n",error); abort(); }
    char *after=ce_csl_json_print_exact(node); assert(after && !strcmp(before,after));
    cJSON_free(before); cJSON_free(after); cJSON_Delete(node); ++checks; return archive;
}
static void plain_is(const CeBibliographyArchive *archive,const char *expected) {
    CeCslOutputString out={0}; assert(ce_bibliography_archive_plain(archive,&out,error,sizeof error));
    if(strcmp(out.data,expected)) { fprintf(stderr,"plain expected <%s> actual <%s>\n",expected,out.data); abort(); }
    ce_csl_output_string_free(&out); ++checks;
}
static void test_basic(void) {
    CeBibliographyArchive *a=decode("[{\"number\":7,\"citationId\":\"old-1\",\"key\":\"K1\",\"itemUri\":\"zotero://select/items/K1\",\"pdfUri\":\"zotero://open-pdf/library/items/P1\",\"entryHtml\":\"<b>Bold <b>still</b></b> <i>Italic</i> 中文🙂 &amp;lt; &#x1F642; &#38; <u>U</u><sup>2</sup><sub>i</sub><br>Next\",\"custom\":{\"n\":4503599627370501}}]");
    assert(ce_bibliography_archive_count(a)==1 && !ce_bibliography_archive_diagnostics(a));
    const CeBibliographyArchiveEntry *e=ce_bibliography_archive_entry(a,0); assert(e && e->source_index==0);
    assert(!strcmp(e->citation_id,"old-1") && !strcmp(e->key,"K1") && !strcmp(e->item_uri,"zotero://select/items/K1"));
    assert(e->row_count==1 && !e->rows[0].left);
    assert(ce_csl_output_run(e->rows[0].body,0)->format==CE_CSL_BOLD);
    assert(ce_csl_output_run(e->rows[0].body,1)->format==CE_CSL_BOLD); /* HTML inheritance, never toggle. */
    plain_is(a,"[7] Bold still Italic 中文🙂 &lt; 🙂 & U2i\nNext");
    CeCslOutputString owned={0}; assert(ce_bibliography_archive_plain(a,&owned,error,sizeof error));
    char *old_text=owned.data; size_t old_length=owned.length;
    fail_default_malloc=true;
    assert(!ce_bibliography_archive_plain(a,&owned,error,sizeof error));
    fail_default_malloc=false; assert(owned.data==old_text && owned.length==old_length);
    ce_bibliography_archive_free(a); assert(strstr(owned.data,"Bold still")); ce_csl_output_string_free(&owned);
    a=decode("[{\"entryText\":\"<literal> &amp; text\"},{\"entryHtml\":\"\"}]");
    assert(!ce_bibliography_archive_diagnostics(a)); plain_is(a,"<literal> &amp; text\n"); ce_bibliography_archive_free(a);
    a=decode("[]"); plain_is(a,""); assert(!ce_bibliography_archive_entry(a,0)); ce_bibliography_archive_free(a);
}
static const char *columns="[{\"number\":9,\"entryHtml\":\"<div class=\\\"csl-bib-body\\\"><div class=\\\"csl-entry\\\"><div class=\\\"csl-left-margin\\\">[A]</div> <div class=\\\"csl-right-inline\\\"><i>Body</i> <a href=\\\"https://example.invalid/a?x=1&amp;y=2\\\">Link</a></div><div class=\\\"csl-indent\\\">Indented</div><div class=\\\"csl-block\\\">Block</div></div></div>\"}]";
static void test_rows(void) {
    CeBibliographyArchive *a=decode(columns); const CeBibliographyArchiveEntry *e=ce_bibliography_archive_entry(a,0);
    assert(!e->diagnostics && e->row_count==3 && e->rows[0].kind==CE_CSL_BIBLIOGRAPHY_TWO_COLUMNS);
    assert(e->rows[1].kind==CE_CSL_BIBLIOGRAPHY_INDENT && e->rows[2].kind==CE_CSL_BIBLIOGRAPHY_BLOCK);
    const CeCslOutputRun *run=ce_csl_output_run(e->rows[0].body,2);
    assert(run && !strcmp(run->href,"https://example.invalid/a?x=1&y=2"));
    plain_is(a,"[9] [A]\tBody Link\nIndented\nBlock"); ce_bibliography_archive_free(a);
    a=decode("[{\"entryHtml\":\"<strong>A<span style='font-weight:normal; font-style:italic; text-decoration: underline; vertical-align:super'>B</span>C</strong>\"}]");
    e=ce_bibliography_archive_entry(a,0);
    assert(ce_csl_output_run(e->rows[0].body,1)->format==(CE_CSL_ITALIC|CE_CSL_UNDERLINE|CE_CSL_SUPERSCRIPT));
    assert(ce_csl_output_run(e->rows[0].body,2)->format==CE_CSL_BOLD); plain_is(a,"ABC"); ce_bibliography_archive_free(a);
}
static void fallback(const char *html,const char *text) {
    cJSON *list=cJSON_CreateArray(),*entry=cJSON_CreateObject(); assert(list && entry);
    assert(cJSON_AddItemToArray(list,entry) && cJSON_AddStringToObject(entry,"entryHtml",html));
    if(text) assert(cJSON_AddStringToObject(entry,"entryText",text));
    char *json=ce_csl_json_print_exact(list); assert(json); CeBibliographyArchive *a=decode(json);
    assert(ce_bibliography_archive_diagnostics(a)&CE_BIBLIOGRAPHY_ARCHIVE_FALLBACK_TEXT);
    plain_is(a,text?text:html);
    const CeBibliographyArchiveEntry *e=ce_bibliography_archive_entry(a,0);
    for(size_t i=0;i<ce_csl_output_run_count(e->rows[0].body);i++) {
        const CeCslOutputRun *run=ce_csl_output_run(e->rows[0].body,i); assert(!run->format && !run->href);
    }
    ce_bibliography_archive_free(a); cJSON_free(json); cJSON_Delete(list);
}
static void test_hostile(void) {
    const char *bad[]={"<script>alert('x')</script>","<img src='https://example.invalid/track' onerror='x'>",
        "<i onclick='x'>Text</i>","<b>unclosed","<b><i>x</b></i>","<svg><script>x</script></svg>",
        "<a href='javascript:alert(1)'>x</a>","<a href='https://user:secret@example.invalid/'>x</a>",
        "<a href='https://example.invalid/%0a'>x</a>","<a href='javascript&#58;alert(1)'>x</a>",
        "<a href='https://example.invalid/' href='https://evil.invalid/'>x</a>","<a href=''>x</a>",
        "<span style='background:url(https://example.invalid/)'>x</span>","<span style='font-variant:small-caps'>x</span>",
        "<div class='csl-right-inline'>orphan</div>","<div class='csl-left-margin'>orphan</div>",
        "<div class='csl-block'><div class='csl-indent'>nested</div></div>","&unknown;","&#0;","&#xD800;","&#1114112;",
        "<i/>self","<!-- comment -->Text","<a href='https://example.invalid/'><a href='https://example.invalid/'>x</a></a>"};
    for(size_t i=0;i<sizeof bad/sizeof *bad;i++) { fallback(bad[i],NULL); fallback(bad[i],"Readable archive &amp; literal <text>"); }
    char deep[520]; size_t used=0;
    for(size_t i=0;i<65;i++) { memcpy(deep+used,"<b>",3); used+=3; }
    deep[used++]='x';
    for(size_t i=0;i<65;i++) { memcpy(deep+used,"</b>",4); used+=4; }
    deep[used]=0; fallback(deep,NULL);
}
static void rejected(cJSON *node,CeBibliographyArchive *old) {
    CeBibliographyArchive *a=old; assert(!ce_bibliography_archive_decode(node,NULL,&a,error,sizeof error));
    assert(*error && a==old); ++checks;
}
static void test_invalid_limits(void) {
    CeBibliographyArchive *old=decode("[{\"entryText\":\"OLD\"}]");
    const char *invalid[]={"null","{}","[null]","[{\"entryText\":\"A\",\"entryText\":\"B\"}]","[{\"entryText\":\"\\ud800\"}]","[{\"entryText\":\"\\u0000\"}]"};
    for(size_t i=0;i<sizeof invalid/sizeof *invalid;i++) { cJSON *node=atom(invalid[i]); rejected(node,old); cJSON_Delete(node); }
    cJSON *node=atom("[]"),*attrs=cJSON_GetObjectItemCaseSensitive(node,"attrs");
    assert(ce_bibliography_archive_eligible(node));
    assert(cJSON_AddStringToObject(attrs,"bibliographyMode","citations")); assert(!ce_bibliography_archive_eligible(node)); rejected(node,old);
    cJSON_ReplaceItemInObjectCaseSensitive(attrs,"bibliographyMode",cJSON_CreateNull()); rejected(node,old);
    cJSON_ReplaceItemInObjectCaseSensitive(attrs,"bibliographyMode",cJSON_CreateString("future")); rejected(node,old); cJSON_Delete(node);
    size_t size=CE_CSL_OUTPUT_MAX_BYTES; char *text=malloc(size+2); assert(text); memset(text,'a',size+1); text[size]=0;
    cJSON *list=cJSON_CreateArray(),*entry=cJSON_CreateObject(); assert(cJSON_AddItemToArray(list,entry)); assert(cJSON_AddStringToObject(entry,"entryText",text));
    char *json=ce_csl_json_print_exact(list); assert(json); CeBibliographyArchive *a=decode(json);
    CeCslOutputString plain={0}; assert(ce_bibliography_archive_plain(a,&plain,error,sizeof error) && plain.length==size);
    ce_csl_output_string_free(&plain); ce_bibliography_archive_free(a); cJSON_free(json);
    text[size]='a'; text[size+1]=0; cJSON_ReplaceItemInObjectCaseSensitive(entry,"entryText",cJSON_CreateString(text));
    json=ce_csl_json_print_exact(list); assert(json); node=atom(json); rejected(node,old); cJSON_Delete(node); cJSON_free(json); cJSON_Delete(list); free(text);
    list=cJSON_CreateArray(); for(size_t i=0;i<=CE_BIBLIOGRAPHY_ARCHIVE_MAX_ENTRIES;i++) assert(cJSON_AddItemToArray(list,cJSON_CreateObject()));
    json=ce_csl_json_print_exact(list); assert(json); node=atom(json); rejected(node,old); cJSON_Delete(node); cJSON_free(json); cJSON_Delete(list);
    /* Both pathological limits use valid, sub-1-MiB HTML, so a syntax fallback
     * must not disguise the resource failure as a successful import. */
    const char *fragments[]={"<b>x</b>","<div>x</div>"};
    const size_t counts[]={CE_CSL_OUTPUT_MAX_RUNS+1,CE_BIBLIOGRAPHY_ARCHIVE_MAX_ROWS+1};
    for(size_t which=0;which<2;which++) {
        size_t width=strlen(fragments[which]),bytes=width*counts[which];
        char *html=malloc(bytes+1); assert(html && bytes<CE_CSL_OUTPUT_MAX_BYTES);
        for(size_t i=0;i<counts[which];i++) memcpy(html+i*width,fragments[which],width);
        html[bytes]=0; list=cJSON_CreateArray(); entry=cJSON_CreateObject();
        assert(cJSON_AddItemToArray(list,entry) && cJSON_AddStringToObject(entry,"entryHtml",html)); free(html);
        json=ce_csl_json_print_exact(list); assert(json); node=atom(json); rejected(node,old);
        cJSON_Delete(node); cJSON_free(json); cJSON_Delete(list);
    }
    a=decode("[{\"number\":1.0000000000000001,\"entryText\":\"Precise text\"}]");
    assert(ce_bibliography_archive_diagnostics(a)&CE_BIBLIOGRAPHY_ARCHIVE_INVALID_METADATA); plain_is(a,"Precise text"); ce_bibliography_archive_free(a);
    a=decode("[{\"number\":7,\"custom\":1.5,\"entryText\":\"No invented numeric label\"}]");
    assert(ce_bibliography_archive_diagnostics(a)&CE_BIBLIOGRAPHY_ARCHIVE_INVALID_METADATA); ce_bibliography_archive_free(a);
    plain_is(old,"OLD"); ce_bibliography_archive_free(old);
}
typedef struct Fault { size_t count,fail,live; bool sticky; } Fault;
static void *fault_alloc(void *context,size_t size) {
    Fault *f=context; ++f->count;
    if(f->fail && (f->count==f->fail || (f->sticky && f->count>=f->fail))) return NULL;
    void *p=malloc(size); if(p) ++f->live; return p;
}
static void fault_free(void *context,void *memory) { Fault *f=context; if(memory) { assert(f->live); --f->live; free(memory); } }
static size_t json_count,json_fail; static bool json_sticky;
static void *json_alloc(size_t size) {
    ++json_count; if(json_fail && (json_count==json_fail || (json_sticky && json_count>=json_fail))) return NULL;
    return malloc(size);
}
static void test_oom_case(const char *entries) {
    cJSON *node=atom(entries); CeBibliographyArchive *old=decode("[{\"entryText\":\"OLD\"}]"),*a=NULL;
    Fault f={0}; CeCslOutputAllocator allocator={&f,fault_alloc,fault_free};
    assert(ce_bibliography_archive_decode(node,&allocator,&a,error,sizeof error)); size_t points=f.count;
    ce_bibliography_archive_free(a); assert(!f.live);
    for(size_t sticky=0;sticky<2;sticky++) for(size_t point=1;point<=points;point++) {
        f=(Fault){.fail=point,.sticky=sticky!=0}; a=old;
        assert(!ce_bibliography_archive_decode(node,&allocator,&a,error,sizeof error));
        assert(a==old && !f.live); plain_is(old,"OLD");
    }
    cJSON_Hooks hooks={json_alloc,free}; cJSON_InitHooks(&hooks); json_count=0; json_fail=0; a=NULL;
    assert(ce_bibliography_archive_decode(node,NULL,&a,error,sizeof error)); size_t parser_points=json_count;
    ce_bibliography_archive_free(a);
    for(size_t sticky=0;sticky<2;sticky++) for(size_t point=1;point<=parser_points;point++) {
        json_count=0; json_fail=point; json_sticky=sticky!=0; a=old;
        assert(!ce_bibliography_archive_decode(node,NULL,&a,error,sizeof error)); assert(a==old);
    }
    cJSON_InitHooks(NULL); json_fail=0; cJSON_Delete(node); ce_bibliography_archive_free(old);
    printf("archive: %zu owned + %zu parser failpoints, sparse and sticky atomic\n",points,parser_points);
}
int main(void) {
    test_basic(); test_rows(); test_hostile(); test_invalid_limits(); test_oom_case(columns);
    test_oom_case("[{\"number\":1,\"entryHtml\":\"<b>Partial</b><a href='javascript:bad'>unsafe</a>\",\"entryText\":\"Safe fallback\"},{\"entryHtml\":\"<div class='csl-block'>Partial block</div><i>unclosed\"}]");
    printf("bibliography archive: %zu checks PASS\n",checks); return 0;
}
