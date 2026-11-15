/* Test-only allocation interposition, including document index malloc/realloc.
 * Compiling the same implementation here avoids linker-specific --wrap tricks
 * and does not add process-global allocator configuration to the product. */
#include "document.h"
#include "cJSON.h"
#include <assert.h>
#include <ctype.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef union FaultAllocation {
    long double alignment;
    void *pointer;
    size_t size;
    unsigned char padding[32];
} FaultAllocation;
static size_t fault_calls, fault_at, fault_rejected, fault_live, fault_bytes, fault_reallocs;
static bool reject_allocation(void) {
    ++fault_calls;
    if (fault_at && fault_calls >= fault_at) { ++fault_rejected; return true; }
    return false;
}
static void *fault_malloc(size_t size) {
    if (reject_allocation() || size > SIZE_MAX - sizeof(FaultAllocation)) return NULL;
    FaultAllocation *p = malloc(sizeof *p + size);
    if (!p) return NULL;
    p->size = size; ++fault_live; fault_bytes += size; return p + 1;
}
static void fault_free(void *memory) {
    if (!memory) return;
    FaultAllocation *p = (FaultAllocation *)memory - 1;
    assert(fault_live && fault_bytes >= p->size);
    --fault_live; fault_bytes -= p->size; free(p);
}
static void *fault_calloc(size_t count, size_t size) {
    if (count && size > SIZE_MAX / count) return NULL;
    void *p = fault_malloc(count * size); if (p) memset(p, 0, count * size); return p;
}
static void *fault_realloc(void *memory, size_t size) {
    ++fault_reallocs;
    if (!memory) return fault_malloc(size);
    if (!size) { fault_free(memory); return NULL; }
    if (reject_allocation() || size > SIZE_MAX - sizeof(FaultAllocation)) return NULL;
    FaultAllocation *old = (FaultAllocation *)memory - 1;
    size_t before = old->size;
    FaultAllocation *p = realloc(old, sizeof *old + size); if (!p) return NULL;
    p->size = size; fault_bytes = fault_bytes - before + size; return p + 1;
}
#define malloc fault_malloc
#define calloc fault_calloc
#define realloc fault_realloc
#define free fault_free
#include "../src/document.c"
#undef malloc
#undef calloc
#undef realloc
#undef free

static CeDocument *load_edit(const char *text,CeFormat format) {
    CeDocument *d=ce_document_new();char error[256];assert(d);
    if (!ce_document_import(d,text,format,error,sizeof error)) { fprintf(stderr,"import: %s\n",error);abort(); }
    return d;
}
static char *json_edit(CeDocument *d) { char *j=ce_document_export_json(d);assert(j);return j; }
static void json_is(CeDocument *d,const char *expected) {
    char *actual=json_edit(d);
    if (strcmp(actual,expected)) fprintf(stderr,"expected: %s\nactual: %s\n",expected,actual);
    assert(!strcmp(actual,expected));fault_free(actual);
}
static void html_is(CeDocument *d,const char *expected) {
    char *actual=ce_document_export_html(d);assert(actual);
    if (strcmp(actual,expected)) fprintf(stderr,"expected HTML: %s\nactual HTML: %s\n",expected,actual);
    assert(!strcmp(actual,expected));fault_free(actual);
}
static void change(CeDocument *d,size_t a,size_t b,int direction) {
    bool changed=false;char *before=json_edit(d);char *text=copy(ce_document_text(d));assert(text);
    if (!ce_document_adjust_list_depth(d,a,b,direction,&changed)) { fprintf(stderr,"list edit: %s\n",ce_document_error(d));abort(); }
    assert(changed && !strcmp(text,ce_document_text(d)) && ce_document_is_modified(d));
    char *after=json_edit(d);assert(ce_document_undo(d));json_is(d,before);assert(!ce_document_is_modified(d));
    assert(ce_document_redo(d));json_is(d,after);
    fault_free(before);fault_free(after);fault_free(text);
}
/* Expected structures were checked against the repository's actual
 * prosemirror-schema-list 1.5.1 sinkListItem/liftListItem commands. Ordered
 * suffixes inherit the source start, even when this restarts their numbering. */
static void test_commands(void) {
    CeDocument *d=load_edit("<ol start=\"7\"><li><p>A</p></li><li><p>B</p></li><li><p>C</p></li></ol>",CE_FORMAT_HTML);
    change(d,2,3,1);
    html_is(d,"<ol start=\"7\"><li><p>A</p><ol start=\"1\"><li><p>B</p></li></ol></li><li><p>C</p></li></ol>");
    assert(ce_document_undo(d));change(d,2,3,-1);
    html_is(d,"<ol start=\"7\"><li><p>A</p></li></ol><p>B</p><ol start=\"7\"><li><p>C</p></li></ol>");ce_document_free(d);
    d=load_edit("<ol start=\"7\"><li><p>A</p><ol start=\"4\"><li><p>B</p></li><li><p>C</p></li><li><p>D</p></li></ol></li><li><p>E</p></li></ol>",CE_FORMAT_HTML);
    change(d,2,5,-1);
    html_is(d,"<ol start=\"7\"><li><p>A</p></li><li><p>B</p></li><li><p>C</p><ol start=\"4\"><li><p>D</p></li></ol></li><li><p>E</p></li></ol>");ce_document_free(d);
    d=load_edit("<ul><li><p>A</p><ul><li><p>B</p></li><li><p>C</p></li></ul></li><li><p>D</p></li></ul>",CE_FORMAT_HTML);
    change(d,2,7,-1);html_is(d,"<p>A</p><ul><li><p>B</p></li><li><p>C</p></li></ul><p>D</p>");ce_document_free(d);
    d=load_edit("<ul><li><p>A</p><ul><li><p>X</p></li></ul></li><li><p>B</p></li><li><p>C</p></li></ul>",CE_FORMAT_HTML);
    change(d,4,7,1);html_is(d,"<ul><li><p>A</p><ul><li><p>X</p></li><li><p>B</p></li><li><p>C</p></li></ul></li></ul>");ce_document_free(d);
    d=load_edit("<ul><li><p>A</p><ul><li><p>B</p></li><li><p>C</p></li></ul><p>tail</p></li><li><p>D</p></li></ul>",CE_FORMAT_HTML);
    change(d,2,3,-1);html_is(d,"<ul><li><p>A</p></li><li><p>B</p><ul><li><p>C</p></li></ul><p>tail</p></li><li><p>D</p></li></ul>");ce_document_free(d);
    d=load_edit("<ol start=\"7\"><li><p>A</p><ol start=\"4\"><li><p>B</p><ol start=\"9\"><li><p>X</p></li></ol></li><li><p>C</p></li></ol></li><li><p>D</p></li></ol>",CE_FORMAT_HTML);
    change(d,2,3,-1);html_is(d,"<ol start=\"7\"><li><p>A</p></li><li><p>B</p><ol start=\"9\"><li><p>X</p></li><li><p>C</p></li></ol></li><li><p>D</p></li></ol>");ce_document_free(d);
    d=load_edit("<ul><li><p>A</p></li><li><p>B</p></li></ul><p>middle</p><ol start=\"9\"><li><p>C</p></li><li><p>D</p></li></ol>",CE_FORMAT_HTML);
    change(d,2,12,-1);html_is(d,"<ul><li><p>A</p></li></ul><p>B</p><p>middle</p><p>C</p><ol start=\"9\"><li><p>D</p></li></ol>");ce_document_free(d);
}
static void test_noop_and_bounds(void) {
    CeDocument *d=load_edit("<ul><li><p>A</p></li><li><p></p></li><li><p>中文🙂</p></li></ul>",CE_FORMAT_HTML);
    bool changed=true;assert(ce_document_adjust_list_depth(d,0,0,1,&changed));assert(!changed && !ce_document_can_undo(d) && !ce_document_is_modified(d));
    change(d,2,2,1);assert(ce_document_undo(d));
    char *before=json_edit(d);changed=true;
    assert(!ce_document_adjust_list_depth(d,4,5,1,&changed));assert(changed);json_is(d,before);
    assert(!ce_document_adjust_list_depth(d,0,1,0,&changed));assert(changed);json_is(d,before);
    fault_free(before);ce_document_free(d);
    d=load_edit("plain",CE_FORMAT_TEXT);changed=true;assert(ce_document_adjust_list_depth(d,0,5,-1,&changed));assert(!changed && !ce_document_can_undo(d));ce_document_free(d);
}
static void test_limits(void) {
    /* Unknown metadata participates in value/depth bounds even though editing
     * it is neither needed nor permitted by these structural operations. */
    CeDocument *d=load_edit("A",CE_FORMAT_TEXT);
    cJSON *attrs=cJSON_CreateObject(),*values=cJSON_CreateArray();assert(attrs && values);
    assert(cJSON_AddItemToObject(d->root,"attrs",attrs) && cJSON_AddItemToObject(attrs,"future",values));
    for(size_t i=0;i<LE_MAX_VALUES;++i) assert(cJSON_AddItemToArray(values,cJSON_CreateNull()));
    char *before=json_edit(d);bool changed=true;const char *text=ce_document_text(d);
    assert(!ce_document_adjust_list_depth(d,0,1,-1,&changed));assert(changed && ce_document_text(d)==text && !ce_document_can_undo(d));json_is(d,before);
    fault_free(before);ce_document_free(d);
    d=load_edit("A",CE_FORMAT_TEXT);attrs=cJSON_CreateObject();assert(attrs && cJSON_AddItemToObject(d->root,"attrs",attrs));
    cJSON *nested=cJSON_CreateArray();assert(nested && cJSON_AddItemToObject(attrs,"future",nested));
    for(size_t i=0;i<129;++i) { cJSON *child=cJSON_CreateArray();assert(child && cJSON_AddItemToArray(nested,child));nested=child; }
    before=json_edit(d);assert(!ce_document_clear_block_formatting(d,0,1));json_is(d,before);fault_free(before);ce_document_free(d);
    char *large=malloc(LE_MAX_BYTES+1);assert(large);memset(large,'x',LE_MAX_BYTES);large[LE_MAX_BYTES]=0;
    d=load_edit(large,CE_FORMAT_TEXT);free(large);text=ce_document_text(d);
    assert(!ce_document_clear_block_formatting(d,0,1));assert(ce_document_text(d)==text && !ce_document_can_undo(d) && !ce_document_is_modified(d));ce_document_free(d);
}
static const char *metadata_doc="{\"type\":\"doc\",\"attrs\":{\"future\":true},\"content\":[{\"type\":\"bulletList\",\"content\":[{\"type\":\"listItem\",\"content\":[{\"type\":\"paragraph\",\"content\":[{\"type\":\"text\",\"text\":\"A\"}]}]},{\"type\":\"listItem\",\"attrs\":{\"custom\":42},\"content\":[{\"type\":\"heading\",\"attrs\":{\"level\":2,\"textAlign\":\"center\",\"keep\":true},\"content\":[{\"type\":\"text\",\"text\":\"中文ABC\",\"marks\":[{\"type\":\"bold\"}]}]},{\"type\":\"zoteroCitationBlock\",\"attrs\":{\"citationId\":\"c1\",\"unknown\":[true,42]}},{\"type\":\"futureAtom\",\"attrs\":{\"exact\":4503599627370501}}]}]}]}";
static void test_preservation(void) {
    CeDocument *d=load_edit(metadata_doc,CE_FORMAT_JSON);char *before=json_edit(d);
    change(d,2,2,1);char *j=json_edit(d);assert(strstr(j,"\"custom\":42") && strstr(j,"4503599627370501") && strstr(j,"\"citationId\":\"c1\""));fault_free(j);
    assert(ce_document_undo(d));assert(!ce_document_adjust_list_depth(d,2,2,-1,NULL));json_is(d,before);
    assert(!ce_document_clear_block_formatting(d,2,ce_document_length(d)));json_is(d,before);fault_free(before);ce_document_free(d);
    d=load_edit("{\"type\":\"doc\",\"content\":[{\"type\":\"bulletList\",\"attrs\":{\"future\":true},\"content\":[{\"type\":\"listItem\",\"content\":[{\"type\":\"paragraph\",\"content\":[{\"type\":\"text\",\"text\":\"A\"}]}]}]}]}",CE_FORMAT_JSON);
    before=json_edit(d);assert(!ce_document_adjust_list_depth(d,0,1,-1,NULL));json_is(d,before);fault_free(before);ce_document_free(d);
}
static void test_clear(void) {
    CeDocument *d=load_edit("{\"type\":\"doc\",\"content\":[{\"type\":\"heading\",\"attrs\":{\"level\":3,\"textAlign\":\"right\",\"unknown\":17},\"content\":[{\"type\":\"text\",\"text\":\"ABCDE\",\"marks\":[{\"type\":\"bold\"},{\"type\":\"futureMark\",\"attrs\":{\"x\":true}}]}]}]}",CE_FORMAT_JSON);
    char *before=json_edit(d);assert(ce_document_clear_block_formatting(d,1,4));
    assert(ce_document_style_at(d,0).marks&CE_MARK_BOLD);assert(!ce_document_style_at(d,2).marks);assert(ce_document_style_at(d,4).marks&CE_MARK_BOLD);
    CeBlockInfo b;assert(ce_document_block_info(d,0,&b) && b.kind==CE_BLOCK_PARAGRAPH);
    char *j=json_edit(d);assert(strstr(j,"\"unknown\":17") && !strstr(j,"textAlign") && !strstr(j,"level"));fault_free(j);
    assert(ce_document_undo(d));json_is(d,before);assert(!ce_document_can_undo(d) && !ce_document_is_modified(d));
    assert(ce_document_clear_block_formatting(d,2,2));assert(ce_document_style_at(d,2).marks&CE_MARK_BOLD);fault_free(before);ce_document_free(d);
    d=load_edit("<ul><li><p>A</p><ol start=\"4\"><li><p><strong>B</strong></p></li><li><p>C</p></li></ol></li><li><p>D</p></li></ul>",CE_FORMAT_HTML);
    before=json_edit(d);assert(ce_document_clear_block_formatting(d,2,3));html_is(d,"<ul><li><p>A</p></li></ul><p>B</p><ol start=\"4\"><li><p>C</p></li></ol><ul><li><p>D</p></li></ul>");
    assert(ce_document_undo(d));json_is(d,before);assert(!ce_document_can_undo(d));fault_free(before);ce_document_free(d);
    d=load_edit("{\"type\":\"doc\",\"content\":[{\"type\":\"bulletList\",\"content\":[{\"type\":\"listItem\",\"content\":[{\"type\":\"heading\",\"attrs\":{\"level\":2},\"content\":[{\"type\":\"text\",\"text\":\"A\",\"marks\":[{\"type\":\"italic\"}]}]},{\"type\":\"zoteroCitationBlock\",\"attrs\":{\"citationId\":\"c1\",\"unknown\":true}},{\"type\":\"futureAtom\",\"attrs\":{\"x\":[1,2]}}]}]}]}",CE_FORMAT_JSON);
    assert(ce_document_clear_block_formatting(d,0,ce_document_length(d)));j=json_edit(d);assert(!strstr(j,"listItem") && !strstr(j,"marks") && strstr(j,"\"citationId\":\"c1\"") && strstr(j,"futureAtom"));fault_free(j);ce_document_free(d);
}
static void test_selected_html(void) {
    const char *sources="[{\"sourceId\":\"中文🙂<&\\\"\",\"cslJson\":\"{\\\"title\\\":\\\"A & B\\\"}\"}]";
    cJSON *root=cJSON_CreateObject(),*content=cJSON_CreateArray(),*n=cJSON_CreateObject(),*attrs=cJSON_CreateObject();assert(root && content && n && attrs);
    assert(cJSON_AddStringToObject(root,"type","doc") && cJSON_AddItemToObject(root,"content",content));
    assert(cJSON_AddStringToObject(n,"type","zoteroEndnoteSection") && cJSON_AddItemToObject(n,"attrs",attrs));assert(cJSON_AddItemToArray(content,n));
    assert(cJSON_AddStringToObject(attrs,"style","apa") && cJSON_AddStringToObject(attrs,"entriesJson","[]") && cJSON_AddStringToObject(attrs,"bibliographyMode","selected") && cJSON_AddStringToObject(attrs,"selectedItemsJson",sources));
    char *json=cJSON_PrintUnformatted(root);assert(json);CeDocument *d=load_edit(json,CE_FORMAT_JSON);char *html=ce_document_export_html(d);assert(html && strstr(html,"data-bibliography-mode=\"selected\"") && strstr(html,"data-selected-items-json=") && strstr(html,"&lt;&amp;"));
    CeDocument *round=load_edit(html,CE_FORMAT_HTML);json_is(round,json);fault_free(html);ce_document_free(round);
    assert(le_set(attrs,"bibliographyMode",cJSON_CreateString("future-mode")));char *bad=cJSON_PrintUnformatted(root);assert(bad);assert(ce_document_import(d,bad,CE_FORMAT_JSON,NULL,0));assert(!ce_document_export_html(d));json_is(d,bad);
    assert(!ce_document_import(d,"<p data-selected-items-json=\"[]\">A</p>",CE_FORMAT_HTML,NULL,0));json_is(d,bad);
    cJSON_free(bad);cJSON_free(json);cJSON_Delete(root);ce_document_free(d);
}
static CeDocument *fault_document(int operation) {
    const char *html=operation==3?
        "<ol start=\"7\"><li><p>A</p><ol start=\"4\"><li><p>B</p><ol start=\"9\"><li><p>X</p></li></ol></li><li><p>C</p></li></ol></li><li><p>D</p></li></ol>":
        "<ol start=\"7\"><li><p>A</p></li><li><h2 style=\"text-align: center\"><strong>中文BC</strong></h2><ul><li><p>X</p></li></ul></li><li><p>D</p></li></ol>";
    CeDocument *d=load_edit(html,CE_FORMAT_HTML);
    assert(ce_document_insert(d,ce_document_length(d)," saved"));ce_document_mark_saved(d);
    assert(ce_document_insert(d,ce_document_length(d)," redo"));assert(ce_document_undo(d));return d;
}
static bool invoke_edit(CeDocument *d,int operation,bool *changed) {
    if(operation==3) return ce_document_adjust_list_depth(d,2,3,-1,changed);
    return operation==2?ce_document_clear_block_formatting(d,2,10):ce_document_adjust_list_depth(d,2,10,operation? -1:1,changed);
}
static void test_faults(void) {
    size_t checked=0;
    for (int operation=0;operation<4;++operation) {
        CeDocument *d=fault_document(operation);fault_calls=0;bool changed=false;
        assert(invoke_edit(d,operation,&changed));size_t calls=fault_calls;ce_document_free(d);assert(!fault_live && !fault_bytes);
        for (size_t failure=1;failure<=calls;++failure) {
            d=fault_document(operation);char *before=json_edit(d);const char *text=ce_document_text(d);size_t live=fault_live,bytes=fault_bytes;
            changed=false;fault_calls=fault_rejected=0;fault_at=failure;bool ok=invoke_edit(d,operation,&changed);fault_at=0;
            if(ok || fault_live!=live || fault_bytes!=bytes) fprintf(stderr,"OOM op=%d point=%zu/%zu ok=%d live=%zu/%zu bytes=%zu/%zu\n",operation,failure,calls,ok,fault_live,live,fault_bytes,bytes);
            assert(!ok && fault_rejected && !changed && fault_live==live && fault_bytes==bytes);
            assert(ce_document_text(d)==text && ce_document_can_undo(d) && ce_document_can_redo(d) && !ce_document_is_modified(d));json_is(d,before);
            assert(ce_document_redo(d) && ce_document_undo(d));json_is(d,before);assert(ce_document_undo(d));
            fault_free(before);ce_document_free(d);assert(!fault_live && !fault_bytes);++checked;
        }
    }
    printf("list edits: %zu sticky allocation failures preserve content/history/savepoint and leak no memory\n",checked);
}
int main(void) {
    cJSON_Hooks hooks={fault_malloc,fault_free};cJSON_InitHooks(&hooks);
    test_commands();test_noop_and_bounds();test_limits();test_preservation();test_clear();test_selected_html();
    assert(!fault_live && !fault_bytes);test_faults();cJSON_InitHooks(NULL);
    puts("list indent/outdent, complete format clear and selected bibliography HTML passed");return 0;
}
