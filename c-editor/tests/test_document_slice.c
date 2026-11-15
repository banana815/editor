#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cJSON.h"
#include "citation_session.h"
#include "citations.h"
#include "bibliography_selection.h"

static size_t allocations,fail_at;
static bool rejected;
static bool allocation_fails(void) {
    ++allocations;
    if (fail_at && allocations>=fail_at) { rejected=true;return true; }
    return false;
}
/* Keep the tested allocations ordinary malloc pointers: exported strings must
 * also work with consumers outside this translation unit. Tracking records use
 * the uninstrumented allocator and do not change the injected failure points.
 * The table catches leaks on macOS too, where ASan has no LeakSanitizer. */
typedef struct SliceAllocation {
    void *pointer;
    size_t size;
    struct SliceAllocation *next;
} SliceAllocation;
static SliceAllocation *allocation_buckets[65536];
static size_t live_allocations,live_bytes;
static size_t allocation_bucket(const void *p) { return ((uintptr_t)p>>4)%65536; }
static void track_allocation(SliceAllocation *record,void *p,size_t size) {
    if (!record) { record=malloc(sizeof *record);assert(record); }
    size_t bucket=allocation_bucket(p);
    record->pointer=p;record->size=size;record->next=allocation_buckets[bucket];
    allocation_buckets[bucket]=record;++live_allocations;live_bytes+=size;
}
static SliceAllocation *untrack_allocation(void *p) {
    SliceAllocation **slot=&allocation_buckets[allocation_bucket(p)];
    while (*slot && (*slot)->pointer!=p) slot=&(*slot)->next;
    SliceAllocation *record=*slot;
    if (record) { *slot=record->next;assert(live_allocations && live_bytes>=record->size);--live_allocations;live_bytes-=record->size; }
    return record;
}
static void slice_free(void *p) {
    if (!p) return;
    free(untrack_allocation(p));free(p);
}
static void *slice_malloc(size_t size) {
    if (allocation_fails()) return NULL;
    void *p=malloc(size);if (p) track_allocation(NULL,p,size);return p;
}
static void *slice_calloc(size_t count,size_t size) {
    if (allocation_fails() || (count && size>SIZE_MAX/count)) return NULL;
    void *p=calloc(count,size);if (p) track_allocation(NULL,p,count*size);return p;
}
static void *slice_realloc(void *p,size_t size) {
    if (allocation_fails()) return NULL;
    if (!size) { slice_free(p);return NULL; }
    SliceAllocation *record=untrack_allocation(p);
    void *result=realloc(p,size);
    if (result) track_allocation(record,result,size);
    else if (record) track_allocation(record,record->pointer,record->size);
    return result;
}
#define malloc slice_malloc
#define calloc slice_calloc
#define realloc slice_realloc
#define free slice_free
#include "../src/document_slice.c"
#undef malloc
#undef calloc
#undef realloc

static char test_error[512];
static CeDocument *load(const char *data,CeFormat format) {
    CeDocument *d=ce_document_new();assert(d);
    if (!ce_document_import(d,data,format,test_error,sizeof test_error)) { fprintf(stderr,"load: %s\n",test_error);abort(); }
    return d;
}
static CeDocumentSlice *capture_slice(const CeDocument *d,size_t a,size_t b) {
    CeDocumentSlice *slice=NULL;
    if (!ce_document_slice_capture(d,a,b,&slice,NULL,test_error,sizeof test_error)) { fprintf(stderr,"capture: %s\n",test_error);abort(); }
    return slice;
}
static char *snapshot(const CeDocument *d) { char *text=ce_document_export_json(d);assert(text);return text; }
static void document_is(const CeDocument *d,const char *expected) {
    char *text=snapshot(d);if (strcmp(text,expected)) fprintf(stderr,"expected JSON %s\nactual JSON %s\n",expected,text);
    assert(!strcmp(text,expected));free(text);
}
static CeDocument *paste(const CeDocument *d,size_t a,size_t b,const CeDocumentSlice *slice,CeDocumentSliceAnchor *anchor) {
    CeDocument *out=NULL;
    if (!ce_document_slice_prepare_replace(d,a,b,slice,&out,anchor,test_error,sizeof test_error)) { fprintf(stderr,"paste: %s\n",test_error);abort(); }
    return out;
}
static void text_is(const CeDocument *d,const char *expected) {
    if (strcmp(ce_document_text(d),expected)) fprintf(stderr,"expected text <%s> actual <%s>\n",expected,ce_document_text(d));
    assert(!strcmp(ce_document_text(d),expected));
}
static const cJSON *first_type(const cJSON *node,const char *type) {
    if (type_is(node,type)) return node;
    for (const cJSON *n=get(node,"content")?get(node,"content")->child:NULL;n;n=n->next) {
        const cJSON *found=first_type(n,type);if (found) return found;
    }
    return NULL;
}
static void test_text(void) {
    CeDocument *source=load("<p>甲<strong>AB🙂</strong>CD</p>",CE_FORMAT_HTML);
    CeDocumentSlice *slice=capture_slice(source,3,9);char *json=NULL,*plain=NULL,*html=NULL;
    assert(ce_document_slice_json(slice,&json,test_error,sizeof test_error));assert(strstr(json,"bold"));
    assert(ce_document_slice_plain(slice,NULL,NULL,&plain,test_error,sizeof test_error));assert(!strcmp(plain,"AB🙂"));
    assert(ce_document_slice_html(slice,NULL,NULL,&html,test_error,sizeof test_error));assert(strstr(html,"<strong>AB🙂</strong>"));
    assert(strstr(html,"data-notelab-slice=\""));
    CeDocumentSlice *round=NULL;assert(ce_document_slice_parse(json,strlen(json),&round,test_error,sizeof test_error));
    bool found=false;assert(ce_document_slice_from_html(html,strlen(html),&round,&found,test_error,sizeof test_error) && found);
    CeDocument *target=load("xy",CE_FORMAT_TEXT);char *before=snapshot(target);CeDocumentSliceAnchor anchor={0};
    CeDocument *result=paste(target,1,1,round,&anchor);text_is(result,"xAB🙂y");
    assert(anchor.offset==7 && anchor.block_index==0 && anchor.block_offset==7);
    assert(ce_document_style_at(result,1).marks&CE_MARK_BOLD);
    assert(!(ce_document_style_at(result,0).marks&CE_MARK_BOLD));
    document_is(target,before);assert(!ce_document_can_undo(target) && !ce_document_is_modified(target));
    char *replacement=snapshot(result);assert(ce_document_apply_json(target,replacement));assert(ce_document_can_undo(target));
    assert(ce_document_undo(target));document_is(target,before);assert(!ce_document_can_undo(target));
    assert(ce_document_redo(target));document_is(target,replacement);
    free(before);free(replacement);free(json);free(plain);free(html);
    ce_document_slice_free(round);ce_document_slice_free(slice);ce_document_free(source);ce_document_free(target);ce_document_free(result);
    source=load("AB\nCD",CE_FORMAT_TEXT);slice=capture_slice(source,1,4);target=load("xy",CE_FORMAT_TEXT);
    result=paste(target,1,1,slice,&anchor);text_is(result,"xB\nCy");assert(anchor.offset==4);
    ce_document_free(result);ce_document_slice_free(slice);
    slice=capture_slice(source,2,3);result=paste(target,1,1,slice,NULL);text_is(result,"x\ny");
    ce_document_free(result);ce_document_slice_free(slice);ce_document_free(target);ce_document_free(source);
}
static void test_lists(void) {
    CeDocument *source=load("<ol start=\"7\"><li><p>AB</p></li><li><p>CD</p></li></ol>",CE_FORMAT_HTML);
    CeDocument *target=load("<ol start=\"3\"><li><p>XY</p></li><li><p>ZZ</p></li></ol>",CE_FORMAT_HTML);
    CeDocumentSlice *slice=capture_slice(source,1,4);CeDocumentSliceAnchor anchor;
    CeDocument *result=paste(target,1,1,slice,&anchor);text_is(result,"XB\nCY\nZZ");assert(anchor.offset==4);
    char *json=snapshot(result);assert(strstr(json,"\"start\":3"));free(json);ce_document_free(result);ce_document_slice_free(slice);
    slice=capture_slice(source,1,2);result=paste(target,1,1,slice,NULL);text_is(result,"XBY\nZZ");
    ce_document_free(result);ce_document_slice_free(slice);
    slice=capture_slice(source,0,ce_document_length(source));CeDocument *empty=load("",CE_FORMAT_TEXT);
    result=paste(empty,0,0,slice,NULL);text_is(result,"AB\nCD");json=snapshot(result);
    assert(strstr(json,"orderedList") && strstr(json,"\"start\":7"));free(json);
    ce_document_free(result);ce_document_slice_free(slice);
    slice=capture_slice(source,3,5);result=paste(empty,0,0,slice,NULL);text_is(result,"CD");json=snapshot(result);
    assert(strstr(json,"orderedList") && strstr(json,"\"start\":8"));free(json);
    ce_document_free(empty);ce_document_free(result);ce_document_slice_free(slice);ce_document_free(source);ce_document_free(target);
}
static void test_atoms(void) {
    const char *source_json="{\"type\":\"doc\",\"content\":[{\"type\":\"zoteroCitationBlock\",\"attrs\":{\"citationId\":\"existing\",\"sourceId\":\"A\",\"title\":\"中文 <&>\",\"page\":\"12\",\"note\":\"摘录🙂\",\"custom\":{\"exact\":4503599627370501,\"flag\":true},\"cslJson\":\"{\\\"id\\\":\\\"A\\\",\\\"type\\\":\\\"book\\\",\\\"title\\\":\\\"Title\\\"}\"}}]}";
    CeDocument *source=load(source_json,CE_FORMAT_JSON);CeDocumentSlice *slice=capture_slice(source,0,3);
    char *plain=NULL,*html=NULL;assert(ce_document_slice_plain(slice,NULL,NULL,&plain,test_error,sizeof test_error));
    assert(strstr(plain,"中文 <&>") && strstr(plain,"摘录🙂") && !strstr(plain,"\xef\xbf\xbc"));
    assert(ce_document_slice_html(slice,NULL,NULL,&html,test_error,sizeof test_error));assert(strstr(html,"中文 &lt;&amp;&gt;"));
    free(plain);free(html);CeDocument *result=paste(source,0,0,slice,NULL);char *json=snapshot(result);
    cJSON *tree=cJSON_Parse(json);assert(tree && first_type(tree,"zoteroCitationBlock"));const cJSON *content=get(tree,"content");
    const cJSON *first=content->child,*last=first->next;assert(first && last && !last->next);
    assert(strcmp(string(get(first,"attrs"),"citationId"),"existing"));
    assert(!strcmp(string(get(last,"attrs"),"citationId"),"existing"));
    assert(strstr(json,"4503599627370501"));free(json);cJSON_Delete(tree);ce_document_free(result);
    CeDocument *conflict=load(source_json,CE_FORMAT_JSON);char *changed=snapshot(conflict);
    char *at=strstr(changed,"Title");assert(at);memcpy(at,"Other",5);
    assert(ce_document_import(conflict,changed,CE_FORMAT_JSON,test_error,sizeof test_error));free(changed);
    result=load("retained",CE_FORMAT_TEXT);CeDocumentSliceAnchor anchor={4,5,6};char *before=snapshot(result);
    assert(!ce_document_slice_prepare_replace(conflict,0,0,slice,&result,&anchor,test_error,sizeof test_error));
    assert(strstr(test_error,"conflicting"));document_is(result,before);assert(anchor.offset==4 && anchor.block_index==5 && anchor.block_offset==6);
    free(before);ce_document_free(result);ce_document_free(conflict);ce_document_free(source);ce_document_slice_free(slice);
    source=load("{\"type\":\"doc\",\"content\":[{\"type\":\"futureAtom\",\"attrs\":{\"nested\":[true,42,\"保留\"]}}]}",CE_FORMAT_JSON);
    slice=capture_slice(source,0,3);CeDocument *empty=load("",CE_FORMAT_TEXT);result=paste(empty,0,0,slice,NULL);
    char *original=snapshot(source);document_is(result,original);free(original);
    ce_document_slice_free(slice);ce_document_free(source);ce_document_free(empty);ce_document_free(result);
}
static void test_bibliography(void) {
    CeDocument *source=load("{\"type\":\"doc\",\"content\":[{\"type\":\"zoteroEndnoteSection\",\"attrs\":{\"bibliographyMode\":\"citations\",\"entriesJson\":\"[{\\\"entryHtml\\\":\\\"<i>Readable book</i>.\\\"}]\"}}]}",CE_FORMAT_JSON);
    CeDocumentSlice *slice=capture_slice(source,0,3);CeDocument *empty=load("",CE_FORMAT_TEXT),*result=NULL;
    assert(!ce_document_slice_prepare_replace(empty,0,0,slice,&result,NULL,test_error,sizeof test_error));assert(!result);
    char *plain=NULL,*html=NULL;assert(ce_document_slice_plain(slice,NULL,NULL,&plain,test_error,sizeof test_error));assert(strstr(plain,"Readable book"));
    assert(ce_document_slice_html(slice,NULL,NULL,&html,test_error,sizeof test_error));assert(strstr(html,"&lt;i&gt;Readable book&lt;/i&gt;"));
    free(plain);free(html);ce_document_slice_free(slice);ce_document_free(source);ce_document_free(empty);
}
static void test_guard_and_validation(void) {
    CeDocument *d=load("same same",CE_FORMAT_TEXT);CeDocumentSlice *slice=NULL;CeDocumentCutGuard *guard=NULL;bool matches=false;
    assert(ce_document_slice_capture(d,0,4,&slice,&guard,test_error,sizeof test_error));
    assert(ce_document_cut_guard_matches(guard,d,0,4,&matches,test_error,sizeof test_error) && matches);
    assert(ce_document_cut_guard_matches(guard,d,5,9,&matches,test_error,sizeof test_error) && !matches);
    assert(ce_document_style(d,0,4,CE_MARK_BOLD,NULL,true));
    assert(ce_document_cut_guard_matches(guard,d,0,4,&matches,test_error,sizeof test_error) && !matches);
    char *before=NULL;assert(ce_document_slice_json(slice,&before,test_error,sizeof test_error));
    const char *bad[]={"{\"type\":\"notelab-slice\",\"type\":\"notelab-slice\"}",
        "{\"type\":\"notelab-slice\",\"version\":1,\"content\":[],\"openStart\":1,\"openEnd\":0}",
        "{\"type\":\"notelab-slice\",\"version\":1,\"content\":[],\"openStart\":0,\"openEnd\":0,\"unknown\":true}","\xff"};
    for (size_t i=0;i<sizeof bad/sizeof bad[0];i++) {
        assert(!ce_document_slice_parse(bad[i],strlen(bad[i]),&slice,test_error,sizeof test_error));
        char *after=NULL;assert(ce_document_slice_json(slice,&after,test_error,sizeof test_error));assert(!strcmp(after,before));free(after);
    }
    free(before);ce_document_slice_free(slice);ce_document_cut_guard_free(guard);ce_document_free(d);
}
static void test_html_and_unknown(void) {
    CeDocument *d=load("<p><a href=\"javascript:alert(1)\">&lt;script&gt; &amp; text</a></p>",CE_FORMAT_HTML);
    CeDocumentSlice *slice=capture_slice(d,0,ce_document_length(d));char *html=NULL;
    assert(ce_document_slice_html(slice,NULL,NULL,&html,test_error,sizeof test_error));
    assert(!strstr(html,"<script>") && !strstr(html,"<a href=\"javascript:"));
    bool found=true;CeDocumentSlice *old=slice;
    assert(ce_document_slice_from_html("<p>plain</p>",12,&slice,&found,test_error,sizeof test_error));assert(!found && slice==old);
    char *both=malloc(strlen(html)*2+1);assert(both);strcpy(both,html);strcat(both,html);
    found=false;assert(!ce_document_slice_from_html(both,strlen(both),&slice,&found,test_error,sizeof test_error));assert(!found && slice==old);
    const char *close=strstr(html,"\" style=");assert(close);
    size_t truncated=(size_t)(close-html)+1;
    assert(!ce_document_slice_from_html(html,truncated,&slice,&found,test_error,sizeof test_error));assert(!found && slice==old);
    assert(!ce_document_slice_from_html(html,strlen(html)-6,&slice,&found,test_error,sizeof test_error));assert(!found && slice==old);
    assert(ce_document_slice_from_html("data-notelab-slice example",25,&slice,&found,test_error,sizeof test_error) && !found && slice==old);
    free(both);free(html);ce_document_slice_free(slice);ce_document_free(d);
    d=load("{\"type\":\"doc\",\"content\":[{\"type\":\"futureWrapper\",\"attrs\":{\"flag\":true},\"content\":[{\"type\":\"paragraph\",\"content\":[{\"type\":\"text\",\"text\":\"ABCD\"}]}]}]}",CE_FORMAT_JSON);
    slice=capture_slice(d,0,4);char *before=NULL;assert(ce_document_slice_json(slice,&before,test_error,sizeof test_error));
    assert(!ce_document_slice_capture(d,1,2,&slice,NULL,test_error,sizeof test_error));
    char *after=NULL;assert(ce_document_slice_json(slice,&after,test_error,sizeof test_error));assert(!strcmp(before,after));
    free(before);free(after);ce_document_slice_free(slice);ce_document_free(d);
}
static void test_boundaries(void) {
    CeDocument *d=load("XY",CE_FORMAT_TEXT),*result=NULL;CeDocumentSlice *empty=capture_slice(d,1,1);
    char *before=snapshot(d);result=paste(d,1,1,empty,NULL);document_is(result,before);ce_document_free(result);result=NULL;
    CeDocument *alias=d;assert(!ce_document_slice_prepare_replace(d,0,0,empty,&alias,NULL,test_error,sizeof test_error));assert(alias==d);
    free(before);ce_document_slice_free(empty);ce_document_free(d);
    d=load("<p>A</p><ul><li><p>B</p></li></ul>",CE_FORMAT_HTML);before=snapshot(d);
    assert(!ce_document_slice_prepare_replace(d,1,2,NULL,&result,NULL,test_error,sizeof test_error));assert(!result && strstr(test_error,"incompatible"));document_is(d,before);
    result=paste(d,0,2,NULL,NULL);text_is(result,"B");free(before);ce_document_free(result);ce_document_free(d);
    d=load("keep",CE_FORMAT_TEXT);CeDocumentSlice *slice=capture_slice(d,0,4),*old=slice;
    const char *bad[]={
        "<div data-notelab-slice='&bogus;'></div>",
        "<div data-notelab-slice='&#0;'></div>",
        "<div data-notelab-slice='&#xD800;'></div>",
        "<div data-notelab-slice='{}' data-notelab-slice='{}'></div>",
        "<div data-notelab-slice={}></div>",
        "<div data-notelab-slice='{}'></span>"};
    for (size_t i=0;i<sizeof bad/sizeof bad[0];i++) {
        bool found=true;assert(!ce_document_slice_from_html(bad[i],strlen(bad[i]),&slice,&found,test_error,sizeof test_error));assert(found && slice==old);
    }
    size_t size=CE_DOCUMENT_SLICE_MAX_BYTES+1;char *large=malloc(size);assert(large);memset(large,' ',size);
    assert(!ce_document_slice_parse(large,size,&slice,test_error,sizeof test_error));assert(slice==old);
    bool found=false;assert(!ce_document_slice_from_html(large,size,&slice,&found,test_error,sizeof test_error));assert(!found && slice==old);free(large);
    char nested[2048];strcpy(nested,"{\"type\":\"notelab-slice\",\"version\":1,\"openStart\":0,\"openEnd\":0,\"content\":[{\"type\":\"futureAtom\",\"attrs\":{\"x\":");
    for (size_t i=0;i<129;i++) strcat(nested,"[");strcat(nested,"0");for (size_t i=0;i<129;i++) strcat(nested,"]");strcat(nested,"}}]}");
    assert(!ce_document_slice_parse(nested,strlen(nested),&slice,test_error,sizeof test_error));assert(slice==old);
    Buffer nodes={0};Error e={test_error,sizeof test_error,false};
    adds(&nodes,"{\"type\":\"notelab-slice\",\"version\":1,\"openStart\":0,\"openEnd\":0,\"content\":[{\"type\":\"futureAtom\",\"attrs\":{\"x\":[0",&e);
    for (size_t i=0;i<CE_DOCUMENT_SLICE_MAX_NODES;i++) adds(&nodes,",0",&e);adds(&nodes,"]}}]}",&e);assert(!e.failed);
    assert(!ce_document_slice_parse(nodes.data,nodes.size,&slice,test_error,sizeof test_error));assert(slice==old);free(nodes.data);
    ce_document_slice_free(slice);ce_document_free(d);
}
static size_t test_oom_one(CeDocument *source,size_t from,size_t to,CeDocument *target,size_t at) {
    CeDocument *holder=load("old marker",CE_FORMAT_TEXT);
    CeDocumentSlice *incoming=capture_slice(source,from,to);char *serialized=NULL,*html=NULL;
    assert(ce_document_slice_json(incoming,&serialized,test_error,sizeof test_error));
    assert(ce_document_slice_html(incoming,NULL,NULL,&html,test_error,sizeof test_error));
    char *source_before=snapshot(source),*target_before=snapshot(target);size_t checked=0;
    for (unsigned operation=0;operation<8;operation++) {
        size_t total=0;
        for (size_t trial=0;trial<=total;trial++) {
            size_t baseline_live=live_allocations,baseline_bytes=live_bytes;
            CeDocumentSlice *output=capture_slice(holder,0,1),*old_slice=output;
            CeDocumentCutGuard *guard=NULL;CeDocumentSlice *unused=NULL;
            assert(ce_document_slice_capture(holder,0,1,&unused,&guard,test_error,sizeof test_error));ce_document_slice_free(unused);
            CeDocumentCutGuard *old_guard=guard;CeDocument *candidate=load("old output",CE_FORMAT_TEXT),*old_candidate=candidate;
            CeDocumentSliceAnchor anchor={9,8,7};char *string_out=malloc(4);assert(string_out);strcpy(string_out,"old");char *old_string=string_out;
            bool found=false,matches=true,ok=false;allocations=0;fail_at=trial;rejected=false;
            switch (operation) {
                case 0:ok=ce_document_slice_capture(source,from,to,&output,&guard,test_error,sizeof test_error);break;
                case 1:ok=ce_document_slice_parse(serialized,strlen(serialized),&output,test_error,sizeof test_error);break;
                case 2:ok=ce_document_slice_plain(incoming,NULL,NULL,&string_out,test_error,sizeof test_error);break;
                case 3:ok=ce_document_slice_html(incoming,NULL,NULL,&string_out,test_error,sizeof test_error);break;
                case 4:ok=ce_document_slice_prepare_replace(target,at,at,incoming,&candidate,&anchor,test_error,sizeof test_error);break;
                case 5:ok=ce_document_slice_from_html(html,strlen(html),&output,&found,test_error,sizeof test_error);break;
                case 6:ok=ce_document_cut_guard_matches(guard,holder,0,1,&matches,test_error,sizeof test_error);break;
                case 7:ok=ce_document_slice_json(incoming,&string_out,test_error,sizeof test_error);break;
            }
            size_t reached=allocations;fail_at=0;
            if (!trial) { assert(ok);total=reached; }
            else {
                ++checked;
                if (ok || !rejected) fprintf(stderr,"OOM op=%u trial=%zu total=%zu reached=%zu ok=%d rejected=%d\n",operation,trial,total,reached,ok,rejected);
                assert(!ok && rejected);assert(output==old_slice && guard==old_guard && candidate==old_candidate && string_out==old_string);
                assert(anchor.offset==9 && anchor.block_index==8 && anchor.block_offset==7 && !found && matches);
            }
            document_is(source,source_before);document_is(target,target_before);
            ce_document_slice_free(output);ce_document_cut_guard_free(guard);ce_document_free(candidate);free(string_out);
            if (live_allocations!=baseline_live || live_bytes!=baseline_bytes)
                fprintf(stderr,"leak op=%u trial=%zu: %zu/%zu allocations, %zu/%zu bytes\n",operation,trial,live_allocations,baseline_live,live_bytes,baseline_bytes);
            assert(live_allocations==baseline_live && live_bytes==baseline_bytes);
        }
    }
    free(source_before);free(target_before);free(serialized);free(html);ce_document_slice_free(incoming);ce_document_free(holder);return checked;
}
static void test_formal_whole_document(void) {
    CeDocument *source=load("Prose",CE_FORMAT_TEXT);CeZoteroItem item={0};
    item.id="A";item.title="Book";item.csl_json="{\"id\":\"A\",\"type\":\"book\",\"title\":\"Book\",\"author\":[{\"family\":\"Doe\",\"given\":\"Jane\"}],\"issued\":{\"date-parts\":[[2024]]}}";
    char *old_id=NULL;assert(ce_citation_insert(source,5,&item,"","excerpt",&old_id,test_error,sizeof test_error));
    CeCitationSession *session=NULL;assert(ce_citation_session_build(source,NULL,NULL,&session,test_error,sizeof test_error));
    assert(ce_document_import(source,ce_citation_prepared_json(ce_citation_session_prepared(session)),CE_FORMAT_JSON,test_error,sizeof test_error));
    char *before=snapshot(source);CeDocumentSlice *slice=capture_slice(source,0,ce_document_length(source));
    CeDocument *candidate=paste(source,0,0,slice,NULL);size_t sections=0,citations=0;
    for (size_t i=0;i<ce_document_block_count(candidate);i++) { CeBlockInfo b;assert(ce_document_block_info(candidate,i,&b));sections+=b.kind==CE_BLOCK_ENDNOTES;citations+=b.kind==CE_BLOCK_CITATION; }
    assert(sections==1 && citations==2);
    assert(ce_citation_session_apply(&session,source,candidate,NULL,NULL,true,test_error,sizeof test_error));
    assert(ce_citation_prepared_stats(ce_citation_session_prepared(session))->citation_count==2);
    assert(ce_citation_prepared_stats(ce_citation_session_prepared(session))->reference_count==1);
    assert(ce_citation_prepared_citation(ce_citation_session_prepared(session),old_id));
    assert(ce_document_undo(source));document_is(source,before);assert(!ce_document_can_undo(source));
    /* Unknown metadata on a generated section/entry must not disappear when
     * its old rendered bibliography is omitted from a pasted whole document. */
    cJSON *tree=cJSON_Parse(before);assert(tree);cJSON *section=(cJSON *)first_type(tree,"zoteroEndnoteSection");assert(section);
    assert(cJSON_AddTrueToObject(cJSON_GetObjectItemCaseSensitive(section,"attrs"),"futureSection"));
    char *modified=cJSON_PrintUnformatted(tree);assert(modified);cJSON_Delete(tree);
    CeDocument *custom=load(modified,CE_FORMAT_JSON);cJSON_free(modified);CeDocumentSlice *custom_slice=capture_slice(custom,0,ce_document_length(custom));
    char *candidate_before=snapshot(candidate);
    assert(!ce_document_slice_prepare_replace(source,0,0,custom_slice,&candidate,NULL,test_error,sizeof test_error));
    assert(strstr(test_error,"custom metadata"));document_is(candidate,candidate_before);free(candidate_before);
    ce_document_slice_free(custom_slice);ce_document_free(custom);
    size_t checked=test_oom_one(source,0,ce_document_length(source),source,0);
    printf("%zu citation/bibliography sticky allocation failure points preserve all outputs\n",checked);
    free(before);free(old_id);ce_document_slice_free(slice);ce_document_free(candidate);ce_citation_session_free(session);ce_document_free(source);
}
static CeDocument *selected_document(const char *id,const char *title) {
    cJSON *item=cJSON_CreateObject();assert(item);
    assert(cJSON_AddStringToObject(item,"id",id) && cJSON_AddStringToObject(item,"type","book") &&
           cJSON_AddStringToObject(item,"title",title));
    char *snapshot=ce_csl_json_print_exact(item);assert(snapshot);cJSON_Delete(item);
    CeZoteroItem source={0};source.id=(char *)id;source.title=(char *)title;source.csl_json=snapshot;
    const CeZoteroItem *items[]={&source};CeDocument *doc=load("",CE_FORMAT_TEXT);
    assert(ce_bibliography_select(doc,items,1,test_error,sizeof test_error));cJSON_free(snapshot);
    CeCitationSession *session=NULL;assert(ce_citation_session_build(doc,NULL,NULL,&session,test_error,sizeof test_error));
    assert(ce_document_import(doc,ce_citation_prepared_json(ce_citation_session_prepared(session)),CE_FORMAT_JSON,test_error,sizeof test_error));
    ce_citation_session_free(session);return doc;
}
static CeDocumentSlice *selected_slice(CeDocument *doc) {
    CeBlockInfo block;assert(ce_document_block_info(doc,ce_document_block_count(doc)-1,&block) && block.kind==CE_BLOCK_ENDNOTES);
    return capture_slice(doc,block.start,block.end);
}
static void test_selected_bibliography(void) {
    CeDocument *source=selected_document("A","Selected book A"),*target=selected_document("B","Selected book B");
    CeDocumentSlice *slice=selected_slice(source),*round=NULL;char *html=NULL;bool found=false;
    assert(ce_document_slice_html(slice,NULL,NULL,&html,test_error,sizeof test_error));
    assert(ce_document_slice_from_html(html,strlen(html),&round,&found,test_error,sizeof test_error) && found);free(html);
    CeDocument *candidate=paste(target,ce_document_length(target),ce_document_length(target),round,NULL);
    size_t sections=0,citations=0;
    for(size_t i=0;i<ce_document_block_count(candidate);i++) { CeBlockInfo b;assert(ce_document_block_info(candidate,i,&b));sections+=b.kind==CE_BLOCK_ENDNOTES;citations+=b.kind==CE_BLOCK_CITATION; }
    assert(sections==2 && !citations);char *before=snapshot(target);CeCitationSession *session=NULL;
    assert(ce_citation_session_build(target,NULL,NULL,&session,test_error,sizeof test_error));
    assert(ce_citation_session_apply(&session,target,candidate,NULL,NULL,true,test_error,sizeof test_error));
    const CeCitationProcessStats *stats=ce_citation_prepared_stats(ce_citation_session_prepared(session));
    assert(!stats->citation_count && stats->reference_count==2 && stats->selected_count==2);
    assert(ce_document_undo(target));document_is(target,before);assert(!ce_document_can_undo(target));free(before);
    assert(ce_citation_session_build(target,NULL,NULL,&session,test_error,sizeof test_error));
    /* Copying the same explicit bibliography is a union, not another fake cite. */
    CeDocumentSlice *duplicate=selected_slice(target);ce_document_free(candidate);candidate=paste(target,0,0,duplicate,NULL);
    assert(ce_citation_session_apply(&session,target,candidate,NULL,NULL,true,test_error,sizeof test_error));
    stats=ce_citation_prepared_stats(ce_citation_session_prepared(session));assert(!stats->citation_count && stats->reference_count==1 && stats->selected_count==1);
    /* Same source with different CSL is preserved in the private candidate,
     * then rejected by the authoritative transaction without touching live. */
    CeDocument *conflict=selected_document("B","Changed source B");CeDocumentSlice *conflicting=selected_slice(conflict);
    ce_document_free(candidate);candidate=paste(target,0,0,conflicting,NULL);before=snapshot(target);CeCitationSession *old=session;
    assert(!ce_citation_session_apply(&session,target,candidate,NULL,NULL,true,test_error,sizeof test_error));
    assert(session==old && strstr(test_error,"Conflicting"));document_is(target,before);free(before);
    ce_document_slice_free(conflicting);ce_document_free(conflict);
    /* Unknown section/source metadata survives selected-only transport. */
    cJSON *tree=cJSON_Parse(ce_citation_prepared_json(ce_citation_session_prepared(session)));assert(tree);
    cJSON *node=(cJSON *)first_type(tree,"zoteroEndnoteSection"),*attrs=cJSON_GetObjectItemCaseSensitive(node,"attrs"),*items=NULL;
    assert(cJSON_AddStringToObject(attrs,"customSection","keep"));
    const char *encoded=string(attrs,"selectedItemsJson");assert(ce_csl_json_parse_strict(encoded,strlen(encoded),&items,test_error,sizeof test_error));
    assert(cJSON_AddNumberToObject(items->child,"customSource",4503599627370501.0));
    char *item_json=ce_csl_json_print_exact(items);assert(item_json);cJSON_Delete(items);
    assert(cJSON_ReplaceItemInObjectCaseSensitive(attrs,"selectedItemsJson",cJSON_CreateString(item_json)));cJSON_free(item_json);
    char *custom_json=ce_csl_json_print_exact(tree);assert(custom_json);cJSON_Delete(tree);
    CeDocument *custom=load(custom_json,CE_FORMAT_JSON);cJSON_free(custom_json);CeDocumentSlice *custom_slice=selected_slice(custom);
    CeDocument *empty=load("",CE_FORMAT_TEXT);ce_document_free(candidate);candidate=paste(empty,0,0,custom_slice,NULL);
    char *copied=snapshot(candidate);assert(strstr(copied,"customSection") && strstr(copied,"4503599627370501"));free(copied);
    CeCitationSession *empty_session=NULL;assert(ce_citation_session_apply(&empty_session,empty,candidate,NULL,NULL,true,test_error,sizeof test_error));
    copied=snapshot(empty);assert(strstr(copied,"customSection") && strstr(copied,"4503599627370501"));free(copied);
    /* Conflicting custom metadata may not be silently merged away. */
    ce_document_free(candidate);candidate=paste(target,0,0,custom_slice,NULL);before=snapshot(target);old=session;
    assert(!ce_citation_session_apply(&session,target,candidate,NULL,NULL,true,test_error,sizeof test_error));
    assert(old==session);document_is(target,before);free(before);
    ce_document_slice_free(custom_slice);ce_document_free(custom);ce_document_free(empty);ce_citation_session_free(empty_session);
    size_t checked=test_oom_one(source,ce_document_length(source)-3,ce_document_length(source),target,0);
    printf("%zu selected bibliography sticky allocation failure points preserve all outputs\n",checked);
    ce_document_slice_free(duplicate);ce_document_slice_free(round);ce_document_slice_free(slice);
    ce_document_free(candidate);ce_document_free(source);ce_document_free(target);ce_citation_session_free(session);
    /* Structural validation is early, but complete identity/CSL semantics
     * remain the processor's responsibility; no trust in imported caches. */
    const char *bad[]={"null","[] trailing","[{\"cslJson\":\"null\"}]","[{\"cslJson\":\"{}\",\"x\":1.0000000000000001}]"};
    for(size_t i=0;i<sizeof bad/sizeof *bad;i++) {
        source=selected_document("A","Selected book A");char *json=snapshot(source);tree=cJSON_Parse(json);free(json);assert(tree);
        attrs=cJSON_GetObjectItemCaseSensitive((cJSON *)first_type(tree,"zoteroEndnoteSection"),"attrs");
        assert(cJSON_ReplaceItemInObjectCaseSensitive(attrs,"selectedItemsJson",cJSON_CreateString(bad[i])));
        json=ce_csl_json_print_exact(tree);assert(json);cJSON_Delete(tree);assert(ce_document_import(source,json,CE_FORMAT_JSON,test_error,sizeof test_error));cJSON_free(json);
        slice=selected_slice(source);empty=load("",CE_FORMAT_TEXT);candidate=NULL;
        assert(!ce_document_slice_prepare_replace(empty,0,0,slice,&candidate,NULL,test_error,sizeof test_error) && !candidate);
        ce_document_slice_free(slice);ce_document_free(source);ce_document_free(empty);
    }
}
static void test_oom(void) {
    CeDocument *source=load("<ol start=\"4\"><li><p>A<strong>BC</strong></p></li><li><p>D<em>EF</em></p></li></ol>",CE_FORMAT_HTML);
    CeDocument *target=load("<ol start=\"1\"><li><p>XY</p></li></ol>",CE_FORMAT_HTML);
    size_t checked=test_oom_one(source,1,5,target,1);
    printf("%zu text/list sticky allocation failure points preserve all outputs\n",checked);
    ce_document_free(source);ce_document_free(target);
}
typedef union { long double alignment; double number; void *pointer; unsigned char padding[32]; size_t marker; } Prefix;
static size_t prefixed_live;
static void *prefixed_allocate(size_t size) {
    Prefix *p=malloc(sizeof *p+size);if (!p) return NULL;p->marker=0x51ce1234;++prefixed_live;return p+1;
}
static void prefixed_free(void *value) {
    if (!value) return;Prefix *p=(Prefix *)value-1;assert(p->marker==0x51ce1234);p->marker=0;--prefixed_live;free(p);
}
static void test_allocator_contract(void) {
    cJSON_Hooks hooks={prefixed_allocate,prefixed_free};cJSON_InitHooks(&hooks);
    CeDocument *d=load("<p><strong>Owner 中文</strong></p>",CE_FORMAT_HTML);
    CeDocumentSlice *slice=capture_slice(d,0,ce_document_length(d)),*round=NULL;
    char *json=NULL,*plain=NULL,*html=NULL;
    assert(ce_document_slice_json(slice,&json,test_error,sizeof test_error));
    assert(ce_document_slice_plain(slice,NULL,NULL,&plain,test_error,sizeof test_error));
    assert(ce_document_slice_html(slice,NULL,NULL,&html,test_error,sizeof test_error));
    bool found=false;assert(ce_document_slice_from_html(html,strlen(html),&round,&found,test_error,sizeof test_error) && found);
    CeDocument *result=paste(d,0,0,round,NULL);text_is(result,"Owner 中文Owner 中文");
    ce_document_free(result);ce_document_free(d);ce_document_slice_free(slice);ce_document_slice_free(round);
    /* All public strings use ordinary free, independently of cJSON. */
    free(json);free(plain);free(html);assert(!prefixed_live);cJSON_InitHooks(NULL);
}
int main(void) {
    cJSON_Hooks hooks={slice_malloc,slice_free};cJSON_InitHooks(&hooks);
    test_text();test_lists();test_atoms();test_bibliography();test_guard_and_validation();test_html_and_unknown();test_boundaries();test_oom();test_formal_whole_document();test_selected_bibliography();
    assert(!live_allocations && !live_bytes);
    cJSON_InitHooks(NULL);test_allocator_contract();
    assert(!live_allocations && !live_bytes);
    puts("owned typed slices, paragraph/list joins, citation identity preservation and safe exports passed");return 0;
}
