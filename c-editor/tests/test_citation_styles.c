#include "citation_styles.h"
#include "csl_document.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char error[512];
static const CeCitationProfile profiles[]={CE_CITATION_PROFILE_APA,CE_CITATION_PROFILE_IEEE,CE_CITATION_PROFILE_GBT};
static const char *ids[]={"apa","ieee","gbt-7714-2015-numeric"};
static const char *hashes[]={
    "3917def5fc951b104d64216c3373096042368b9704ba269ca83c14ebaba889fc",
    "b4c7619fc16c45a31e4cc3271eab94ffe83192d3b4c7fc729470a3b459448de3",
    "d9cd04f94bc21a99dc320d0a2230d5a1fb47e3634c17c5820cf975c40db3c0c7"
};
static void test_loading(void) {
    CeCitationStyle *loaded=NULL;
    assert(!ce_citation_style_info((CeCitationProfile)-1));
    assert(!ce_citation_style_info((CeCitationProfile)3));
    assert(!ce_citation_style_loaded_info(NULL));
    assert(!ce_citation_style_style(NULL) && !ce_citation_style_locale(NULL));
    ce_citation_style_free(NULL);
    for(size_t i=0;i<3;++i) {
        const CeCitationStyleInfo *info=ce_citation_style_info(profiles[i]);assert(info);
        assert(info->profile==profiles[i] && !strcmp(info->profile_id,ids[i]));
        assert(!strcmp(info->style_sha256,hashes[i]));
        assert(!strcmp(info->style_commit,"3c3995baaa0c4fe1e5c6328027ff6d985c07bfde"));
        assert(!strcmp(info->locale_commit,"14940e56f7c783e4173cafbd92e0f5856f2caa14"));
        assert(!strcmp(info->csl_version,"1.0") && *info->updated);
        assert(ce_citation_style_load(profiles[i],NULL,NULL,&loaded,error,sizeof error));
        assert(ce_citation_style_loaded_info(loaded)==info);
        assert(!strcmp(ce_csl_style_id(ce_citation_style_style(loaded)),info->style_id));
        assert(!strcmp(ce_csl_style_title(ce_citation_style_style(loaded)),info->title));
        assert(!strcmp(ce_csl_locale_language(ce_citation_style_locale(loaded)),i==2?"zh-cn":"en-us"));
        const CeCslXmlNode *root=ce_csl_style_root(ce_citation_style_style(loaded));
        assert(!strcmp(ce_csl_xml_attribute(root,"version"),info->csl_version));
        const CeCslXmlNode *updated=ce_csl_xml_child(ce_csl_xml_child(root,"info"),"updated");
        assert(updated && !strcmp(updated->text,info->updated));
        CeCslLocaleTerm month=ce_csl_locale_term(ce_citation_style_locale(loaded),"month-01",
            CE_CSL_LOCALE_LONG,false,CE_CSL_LOCALE_NEUTER);
        assert(month.found && month.length);
        assert(ce_citation_style_load(profiles[i],"ZH-cn",NULL,&loaded,error,sizeof error));
        assert(!strcmp(ce_csl_locale_language(ce_citation_style_locale(loaded)),"zh-cn"));
        assert(ce_citation_style_load(profiles[i],"EN-us",NULL,&loaded,error,sizeof error));
        assert(!strcmp(ce_csl_locale_language(ce_citation_style_locale(loaded)),"en-us"));
    }
    CeCitationStyle *before=loaded;
    const CeCslStyle *style=ce_citation_style_style(loaded);
    const CeCslLocale *locale=ce_citation_style_locale(loaded);
    const char *bad[]={"","en","zh","en-GB","zh-TW","en_US","fr-FR"," en-US","en-US ","en-US-x-test"};
    for(size_t i=0;i<sizeof bad/sizeof *bad;++i) {
        assert(!ce_citation_style_load(CE_CITATION_PROFILE_APA,bad[i],NULL,&loaded,error,sizeof error));
        assert(*error && loaded==before && ce_citation_style_style(loaded)==style && ce_citation_style_locale(loaded)==locale);
    }
    assert(!ce_citation_style_load((CeCitationProfile)99,NULL,NULL,&loaded,error,sizeof error));
    assert(loaded==before && *error);
    assert(!ce_citation_style_load(CE_CITATION_PROFILE_APA,NULL,NULL,NULL,NULL,0));
    CeCslOutputAllocator invalid={0};
    assert(!ce_citation_style_load(CE_CITATION_PROFILE_APA,NULL,&invalid,&loaded,error,sizeof error));
    assert(loaded==before && *error);ce_citation_style_free(loaded);
}
typedef struct Fault { size_t calls,at,live; } Fault;
static void *fault_allocate(void *context,size_t size) {
    Fault *f=context;++f->calls;
    if(f->at && f->calls>=f->at) return NULL;
    void *p=malloc(size);if(p) ++f->live;return p;
}
static void fault_deallocate(void *context,void *pointer) {
    if(!pointer) return;
    Fault *f=context;assert(f->live);--f->live;free(pointer);
}
static void test_allocation_failures(void) {
    size_t total=0;
    CeCitationStyle *old=NULL;
    assert(ce_citation_style_load(CE_CITATION_PROFILE_APA,NULL,NULL,&old,error,sizeof error));
    const CeCslStyle *style=ce_citation_style_style(old);
    const CeCslLocale *locale=ce_citation_style_locale(old);
    for(size_t i=0;i<3;++i) {
        Fault fault={0};CeCslOutputAllocator allocator={&fault,fault_allocate,fault_deallocate};
        CeCitationStyle *candidate=NULL;
        assert(ce_citation_style_load(profiles[i],NULL,&allocator,&candidate,error,sizeof error));
        size_t count=fault.calls;assert(count);ce_citation_style_free(candidate);assert(!fault.live);
        for(size_t failure=1;failure<=count;++failure) {
            fault=(Fault){0,failure,0};candidate=old;
            assert(!ce_citation_style_load(profiles[i],NULL,&allocator,&candidate,error,sizeof error));
            assert(*error && candidate==old && !fault.live);
            assert(ce_citation_style_style(candidate)==style && ce_citation_style_locale(candidate)==locale);
        }
        total+=count;
    }
    ce_citation_style_free(old);
    printf("citation resource loader: %zu wrapper/style/locale persistent allocation failures preserved old owner\n",total);
}
static void test_document_integration(void) {
    /* Own synthetic fixture; these plain results were independently compared
     * with pinned citeproc 2.4.63 as a research-only black-box oracle. Tests and
     * the product execute only the C processor, with no oracle dependency. */
    const char *bibliographies[]={
        "Doe, J. (2020). Example work. Press.",
        "[1] J. Doe, Example work. London: Press, 2020.",
        "[1] DOE J. Example work[M]. London: Press, 2020."
    };
    const char *json="{\"items\":[{\"id\":\"a\",\"type\":\"book\",\"title\":\"Example work\","
        "\"author\":[{\"family\":\"Doe\",\"given\":\"Jane\"}],\"publisher\":\"Press\","
        "\"publisher-place\":\"London\",\"issued\":{\"date-parts\":[[2020]]}}],"
        "\"citations\":[{\"citationID\":\"c\",\"citationItems\":[{\"id\":\"a\"}]}]}";
    CeCslInput *input=NULL;assert(ce_csl_input_load(&input,json,strlen(json),error,sizeof error));
    for(size_t i=0;i<3;++i) {
        CeCitationStyle *profile=NULL;CeCslDocument *document=NULL;
        assert(ce_citation_style_load(profiles[i],NULL,NULL,&profile,error,sizeof error));
        bool ok=ce_csl_document_build(ce_citation_style_style(profile),ce_citation_style_locale(profile),
            input,NULL,NULL,&document,error,sizeof error);
        if(!ok) fprintf(stderr,"%s document failed: %s\n",ids[i],error);
        assert(ok && ce_csl_document_citation_count(document)==1);
        const CeCslDocumentCitation *citation=ce_csl_document_citation(document,0);assert(citation);
        CeCslOutputString text={0},bib={0};
        assert(ce_csl_output_plain(citation->output,&text,error,sizeof error));
        assert(!strcmp(text.data,i==0?"(Doe, 2020)":"[1]"));
        assert(!citation->missing_term && !citation->literal_markup && !citation->malformed_markup);
        bool superscript=false;
        for(size_t r=0;r<ce_csl_output_run_count(citation->output);++r)
            superscript|=(ce_csl_output_run(citation->output,r)->format&CE_CSL_SUPERSCRIPT)!=0;
        assert(superscript==(i==2));
        assert(ce_csl_document_bibliography_count(document)==1 && ce_csl_document_visible_bibliography_count(document)==1);
        const CeCslDocumentBibliography *entry=ce_csl_document_bibliography(document,0);assert(entry && entry->visible);
        assert(!entry->missing_term && !entry->literal_markup && !entry->malformed_markup);
        const CeCslBibliographyEntry *entries[]={entry->entry};
        assert(ce_csl_bibliography_plain(entries,1,CE_CSL_BIBLIOGRAPHY_PLAIN_COMPACT,NULL,&bib,error,sizeof error));
        assert(bib.length==strlen(bibliographies[i]) && !strcmp(bib.data,bibliographies[i]));
        const CeCslBibliographyOptions *options=ce_csl_bibliography_entry_options(entry->entry);assert(options);
        assert(options->alignment==(i==0?CE_CSL_BIBLIOGRAPHY_ALIGN_NONE:CE_CSL_BIBLIOGRAPHY_ALIGN_FLUSH));
        assert(options->hanging_indent==(i==0));
        printf("embedded %s C processor: citation <%s>, bibliography <%s>\n",ids[i],text.data,bib.data);
        ce_csl_output_string_free(&text);ce_csl_output_string_free(&bib);
        ce_csl_document_free(document);ce_citation_style_free(profile);
    }
    ce_csl_input_free(input);
}
int main(int argc,char **argv) {
    assert(argc==1 || (argc==2 && !strcmp(argv[1],"--load-only")));
    test_loading();test_allocation_failures();
    if(argc==1) test_document_integration();
    puts("embedded citation styles tests passed");return 0;
}
