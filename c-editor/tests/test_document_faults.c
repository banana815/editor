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

typedef enum Operation { APPLY, APPLY_EMPTY, UNDO, REDO, IMPORT_METADATA } Operation;
static const char *metadata_html =
    "<div data-type=\"notelabDocument\" data-citation-profile=\"apa\" data-citation-locale=\"zh-CN\">"
    "<div data-type=\"zoteroCitationBlock\" citationid=\"c1\" data-library-id=\"9007199254740993\" "
    "data-csl-json=\"{&quot;id&quot;:&quot;a&quot;}\" data-csl-render-json=\"{&quot;html&quot;:&quot;&lt;em&gt;x&lt;/em&gt;&quot;}\"></div>"
    "<div data-type=\"zoteroEndnoteSection\" style=\"apa\" entriesjson=\"[]\" data-csl-render-json=\"{}\" data-bibliography-mode=\"citations\"></div></div>";
static const char *replacement =
    "{\"type\":\"doc\",\"content\":[{\"type\":\"heading\",\"attrs\":{\"level\":2},"
    "\"content\":[{\"type\":\"text\",\"text\":\"Replacement text\",\"marks\":[{\"type\":\"bold\"}]}]},"
    "{\"type\":\"zoteroCitationBlock\",\"attrs\":{\"citationId\":\"c1\",\"endnoteNumber\":7}}]}";
static CeDocument *prepared(void) {
    CeDocument *d = ce_document_new(); char error[256]; assert(d);
    assert(ce_document_import(d,"<p><em>Original</em></p>",CE_FORMAT_HTML,error,sizeof error));
    assert(ce_document_insert(d,ce_document_length(d)," saved")); ce_document_mark_saved(d);
    assert(ce_document_insert(d,ce_document_length(d)," redo")); assert(ce_document_undo(d));
    assert(!ce_document_is_modified(d) && ce_document_can_undo(d) && ce_document_can_redo(d));
    return d;
}
static CeDocument *prepared_large_undo(void) {
    char html[8192], error[256]; size_t used=0;
    for(size_t i=0;i<24;++i) {
        int n=snprintf(html+used,sizeof html-used,
            "<p>Long historical paragraph %zu: abcdefghijklmnopqrstuvwxyz "
            "ABCDEFGHIJKLMNOPQRSTUVWXYZ 0123456789 abcdefghijklmnopqrstuvwxyz.</p>",i);
        assert(n>0 && (size_t)n<sizeof html-used);used+=(size_t)n;
    }
    CeDocument *d=ce_document_new(),*small=prepared();assert(d);
    assert(ce_document_import(d,html,CE_FORMAT_HTML,error,sizeof error));
    char *json=ce_document_export_json(small);assert(json);ce_document_free(small);
    assert(ce_document_apply_json(d,json));fault_free(json);ce_document_mark_saved(d);
    assert(ce_document_insert(d,ce_document_length(d)," redo"));assert(ce_document_undo(d));
    /* Applying a small document replaces its caches as well. Restoring the
     * larger undo target must grow text, block and span storage even in the
     * former implementation that reused the current document's indexes. */
    assert(d->cblocks<24 && d->cspans<24 && d->text.cap<2048);
    return d;
}
static bool invoke(CeDocument *d, Operation operation) {
    switch(operation) {
        case APPLY: return ce_document_apply_json(d,replacement);
        case APPLY_EMPTY: return ce_document_apply_json(d,"{\"doc\":{\"type\":\"doc\"}}");
        case UNDO: return ce_document_undo(d);
        case REDO: return ce_document_redo(d);
        case IMPORT_METADATA: return ce_document_import(d,metadata_html,CE_FORMAT_HTML,NULL,0);
    }
    return false;
}
static void sweep(Operation operation, bool large_undo) {
    CeDocument *d=large_undo?prepared_large_undo():prepared(); fault_calls=fault_reallocs=0;
    assert(invoke(d,operation)); size_t calls=fault_calls,reallocs=fault_reallocs;
    ce_document_free(d); assert(!fault_live && !fault_bytes);
    assert(reallocs); /* The candidate's text/spans/block indexes were exercised. */
    size_t repeated=0,failed_realloc=0;
    for(size_t failure=1;failure<=calls;++failure) {
        d=large_undo?prepared_large_undo():prepared(); char *json=ce_document_export_json(d),*html=ce_document_export_html(d);
        assert(json && html);
        const char *text=ce_document_text(d); CeStyle style=ce_document_style_at(d,1);
        size_t blocks=ce_document_block_count(d),spans=ce_document_span_count(d);
        size_t live=fault_live,bytes=fault_bytes;
        fault_calls=fault_rejected=fault_reallocs=0;fault_at=failure;
        bool ok=invoke(d,operation);fault_at=0;
        if(ok) fprintf(stderr,"operation %d unexpectedly succeeded at allocation %zu/%zu\n",operation,failure,calls);
        assert(!ok && *ce_document_error(d));
        repeated+=fault_rejected>1;failed_realloc+=fault_reallocs!=0;
        assert(fault_live==live && fault_bytes==bytes);
        assert(ce_document_text(d)==text && ce_document_block_count(d)==blocks && ce_document_span_count(d)==spans);
        assert(ce_document_style_at(d,1).marks==style.marks);
        char *after=ce_document_export_json(d),*after_html=ce_document_export_html(d);
        assert(after && after_html && !strcmp(json,after) && !strcmp(html,after_html));
        assert(!ce_document_is_modified(d) && ce_document_can_undo(d) && ce_document_can_redo(d));
        assert(ce_document_redo(d) && !strcmp(ce_document_text(d),"Original saved redo"));
        assert(ce_document_undo(d) && !ce_document_is_modified(d));
        assert(ce_document_undo(d));
        if(large_undo) assert(ce_document_block_count(d)==24 && ce_document_length(d)>2048);
        else assert(!strcmp(ce_document_text(d),"Original"));
        fault_free(json);fault_free(html);fault_free(after);fault_free(after_html);ce_document_free(d);
        assert(!fault_live && !fault_bytes);
    }
    printf("document operation %d%s: %zu sticky allocation failures, %zu repeated failures, %zu paths reaching index allocation passed\n",
        operation,large_undo?" (large history)":"",calls,repeated,failed_realloc);
}
static void sweep_metadata_export(void) {
    char error[256]; CeDocument *d=ce_document_new(); assert(d);
    assert(ce_document_import(d,metadata_html,CE_FORMAT_HTML,error,sizeof(error)));
    char *before=ce_document_export_json(d); assert(before); const char *text=ce_document_text(d);
    fault_calls=0; char *expected=ce_document_export_html(d); assert(expected); size_t calls=fault_calls;
    for(size_t failure=1;failure<=calls;++failure) {
        size_t live=fault_live,bytes=fault_bytes; fault_calls=fault_rejected=0; fault_at=failure;
        char *result=ce_document_export_html(d); fault_at=0;
        assert(!result && fault_rejected && fault_live==live && fault_bytes==bytes);
        assert(ce_document_text(d)==text && !ce_document_is_modified(d));
        char *after=ce_document_export_json(d),*html=ce_document_export_html(d);
        assert(after && html && !strcmp(before,after) && !strcmp(expected,html)); fault_free(after); fault_free(html);
    }
    fault_free(before); fault_free(expected); ce_document_free(d); assert(!fault_live && !fault_bytes);
    printf("document metadata HTML export: %zu sticky allocation failures passed\n",calls);
}
static void sweep_exact_json_export(void) {
    const char *source="{\"type\":\"doc\",\"custom\":4503599627370501,"
        "\"content\":[{\"type\":\"paragraph\",\"attrs\":{\"custom\":-4503599627370501},"
        "\"content\":[{\"type\":\"text\",\"text\":\"Original\"}]}]}";
    CeDocument *d=ce_document_new(); assert(d);
    assert(ce_document_import(d,source,CE_FORMAT_JSON,NULL,0));
    fault_calls=0; char *before=ce_document_export_json(d); assert(before);
    size_t calls=fault_calls; const char *text=ce_document_text(d);
    assert(strstr(before,"\"custom\":4503599627370501") && strstr(before,"\"custom\":-4503599627370501"));
    for(size_t failure=1;failure<=calls;++failure) {
        size_t live=fault_live,bytes=fault_bytes;
        fault_calls=fault_rejected=0; fault_at=failure;
        char *result=ce_document_export_json(d); fault_at=0;
        assert(!result && fault_rejected && fault_live==live && fault_bytes==bytes);
        assert(ce_document_text(d)==text && !ce_document_is_modified(d));
        assert(!ce_document_can_undo(d) && !ce_document_can_redo(d));
        char *after=ce_document_export_json(d); assert(after && !strcmp(before,after)); fault_free(after);
    }
    fault_free(before); ce_document_free(d); assert(!fault_live && !fault_bytes);
    printf("document exact-integer JSON export: %zu sticky allocation failures passed\n",calls);
}
static void sweep_constructor(void) {
    fault_calls=0;
    CeDocument *d=ce_document_new(); assert(d);
    size_t calls=fault_calls;
    assert(ce_document_block_count(d)==1 && !ce_document_length(d));
    assert(!ce_document_is_modified(d) && !ce_document_can_undo(d));
    ce_document_free(d); assert(!fault_live && !fault_bytes);
    for(size_t failure=1;failure<=calls;++failure) {
        fault_calls=fault_rejected=0; fault_at=failure;
        d=ce_document_new(); fault_at=0;
        if(d || !fault_rejected || fault_live || fault_bytes)
            fprintf(stderr,"constructor failure %zu/%zu: returned=%d rejected=%zu live=%zu bytes=%zu\n",failure,calls,d!=NULL,fault_rejected,fault_live,fault_bytes);
        assert(!d && fault_rejected && !fault_live && !fault_bytes);
    }
    printf("document constructor: %zu sticky allocation failures leave no live allocations\n",calls);
}
int main(void) {
    cJSON_Hooks hooks={fault_malloc,fault_free};cJSON_InitHooks(&hooks);
    sweep_constructor();
    sweep(APPLY,false);sweep(APPLY_EMPTY,false);sweep(UNDO,false);sweep(REDO,false);sweep(UNDO,true);
    sweep(IMPORT_METADATA,false);sweep_metadata_export();sweep_exact_json_export();
    cJSON_InitHooks(NULL);
    puts("document apply/undo/redo retain content, borrowed indexes, histories and savepoint under persistent OOM");return 0;
}
