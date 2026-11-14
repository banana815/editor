#include "csl_finalize.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Original synthetic metadata; black-box expected text recorded with official
 * npm citeproc@2.4.63 (processor 1.4.61), using the pinned styles/locales in
 * resources/SOURCES.json. No processor code or upstream test-suite fixture is
 * included. The oracle bibliography adapter's terminal newline is removed.
 * Our test-only plain adapter represents second-field-align as one field space;
 * the production entry finalizer deliberately retains raw text and source spans.
 * One explicit normative literal-name expectation differs from citeproc's
 * extension applying family name-part case to a non-personal name.
 * These representative outputs and broad typed-tree checks are not a claim of
 * complete CSL/profile conformance. The product remains entirely C. */
typedef struct Golden {
    const char *id, *item, *cite;
    const char *citation[3], *bibliography[3];
} Golden;
static const Golden goldens[] = {
    {"book", "{\"id\":\"book\",\"type\":\"book\",\"title\":\"an example of reproducible research\",\"author\":[{\"family\":\"Doe\",\"given\":\"Jane\"},{\"family\":\"Roe\",\"given\":\"John\"}],\"issued\":{\"date-parts\":[[2024]]},\"publisher\":\"Example Press\",\"publisher-place\":\"Oxford\",\"edition\":\"2\"}", "{\"locator\":\"5\",\"label\":\"page\"}",
        {"(Doe & Roe, 2024, p. 5)", "[1, p. 5]", "[1]"},
        {"Doe, J., & Roe, J. (2024). an example of reproducible research (2nd ed.). Example Press.", "[1] J. Doe and J. Roe, an example of reproducible research, 2nd ed. Oxford: Example Press, 2024.", "[1] DOE J, ROE J. an example of reproducible research[M]. 2 版. Oxford: Example Press, 2024."}},
    {"journal", "{\"id\":\"journal\",\"type\":\"article-journal\",\"title\":\"Context-aware testing\",\"author\":[{\"family\":\"Doe\",\"given\":\"Jane\"}],\"issued\":{\"date-parts\":[[2025,3,14]]},\"container-title\":\"Journal of Testing\",\"volume\":\"12\",\"issue\":\"2\",\"page\":\"101\",\"DOI\":\"10.1234/example\"}", "{}",
        {"(Doe, 2025)", "[1]", "[1]"},
        {"Doe, J. (2025). Context-aware testing. Journal of Testing, 12(2), 101. https://doi.org/10.1234/example", "[1] J. Doe, “Context-aware testing,” Journal of Testing, vol. 12, no. 2, p. 101, Mar. 2025, doi: 10.1234/example.", "[1] DOE J. Context-aware testing[J/OL]. Journal of Testing, 2025, 12(2): 101. DOI:10.1234/example."}},
    {"chapter", "{\"id\":\"chapter\",\"type\":\"chapter\",\"title\":\"Shared testing\",\"author\":[{\"family\":\"Moran\",\"given\":\"Alice\"}],\"editor\":[{\"family\":\"Lee\",\"given\":\"Sam\"}],\"issued\":{\"date-parts\":[[2022]]},\"container-title\":\"Methods in Systems\",\"publisher\":\"Example Press\",\"publisher-place\":\"New York\",\"page\":\"55\"}", "{}",
        {"(Moran, 2022)", "[1]", "[1]"},
        {"Moran, A. (2022). Shared testing. In S. Lee (Ed.), Methods in Systems (p. 55). Example Press.", "[1] A. Moran, “Shared testing,” in Methods in Systems, S. Lee, Ed., New York: Example Press, 2022, p. 55.", "[1] MORAN A. Shared testing[M]//LEE S. Methods in Systems. New York: Example Press, 2022: 55."}},
    {"webpage", "{\"id\":\"webpage\",\"type\":\"webpage\",\"title\":\"An online dataset\",\"author\":[{\"literal\":\"Example Research Group\"}],\"issued\":{\"date-parts\":[[2023,5,6]]},\"accessed\":{\"date-parts\":[[2026,1,2]]},\"URL\":\"https://example.invalid/data\"}", "{}",
        {"(Example Research Group, 2023)", "[1]", "[1]"},
        {"Example Research Group. (2023, May 6). An online dataset. https://example.invalid/data", "[1] Example Research Group, “An online dataset.” Accessed: Jan. 02, 2026. [Online]. Available: https://example.invalid/data", "[1] EXAMPLE RESEARCH GROUP. An online dataset[EB/OL]. (2023-05-06)[2026-01-02]. https://example.invalid/data."}},
    {"chinese", "{\"id\":\"chinese\",\"type\":\"book\",\"title\":\"科研文献整理方法\",\"author\":[{\"family\":\"张\",\"given\":\"伟\"}],\"issued\":{\"date-parts\":[[2024]]},\"publisher\":\"示例出版社\",\"publisher-place\":\"北京\",\"language\":\"zh-CN\"}", "{}",
        {"(张, 2024)", "[1]", "[1]"},
        {"张伟. (2024). 科研文献整理方法. 示例出版社.", "[1] 张伟, 科研文献整理方法. 北京: 示例出版社, 2024.", "[1] 张伟. 科研文献整理方法[M]. 北京: 示例出版社, 2024."}},
    {"anonymous", "{\"id\":\"anonymous\",\"type\":\"report\",\"title\":\"An anonymous report\",\"issued\":{\"date-parts\":[[2021]]},\"publisher\":\"Example Institute\",\"number\":\"17\"}", "{}",
        {"(An Anonymous Report, 2021)", "[1]", "[1]"},
        {"An anonymous report (No. 17). (2021). Example Institute.", "[1] “An anonymous report,” Example Institute, 17, 2021.", "[1] An anonymous report: 17[R]. Example Institute, 2021."}}
};
static char error[512];
static const char *gb_literal_normative = "[1] Example Research Group. An online dataset[EB/OL]. (2023-05-06)[2026-01-02]. https://example.invalid/data.";
static char *read_file(const char *path) {
    FILE *file = fopen(path, "rb"); assert(file);
    assert(!fseek(file, 0, SEEK_END)); long size = ftell(file); assert(size >= 0);
    rewind(file); char *data = malloc((size_t)size + 1); assert(data);
    assert(fread(data, 1, (size_t)size, file) == (size_t)size);
    fclose(file); data[size] = 0; return data;
}
static bool append_entry(const CeCslEvalRequest *request, const CeCslEvaluation *evaluation,
    const CeCslFinalized *finalized, bool bibliography, CeCslOutput *output) {
    const char *alignment = bibliography ? ce_csl_xml_attribute(request->section,"second-field-align") : NULL;
    if (!alignment) return ce_csl_output_concat(output,ce_csl_finalized_output(finalized),error,sizeof(error));
    assert(!strcmp(alignment,"flush") || !strcmp(alignment,"margin"));
    const CeCslEvalNode *first=ce_csl_eval_node_info(ce_csl_evaluation_root(evaluation))->first_child;
    while (first && !ce_csl_eval_node_info(first)->nonempty) first=ce_csl_eval_node_info(first)->next;
    size_t boundary=0, bytes=0;
    ce_csl_inline_text(ce_csl_finalized_inline(finalized),&bytes);
    for (size_t i=0;i<ce_csl_finalized_span_count(finalized);++i) {
        const CeCslFinalSpan *s=ce_csl_finalized_span(finalized,i);
        if (s->kind==CE_CSL_FINAL_NODE && s->node==first) { boundary=s->offset+s->length; break; }
    }
    if (!boundary || boundary>=bytes)
        return ce_csl_output_concat(output,ce_csl_finalized_output(finalized),error,sizeof(error));
    CeCslInline *left=NULL,*right=NULL;
    bool ok=ce_csl_inline_slice(ce_csl_finalized_inline(finalized),0,boundary,NULL,&left,error,sizeof(error)) &&
        ce_csl_inline_slice(ce_csl_finalized_inline(finalized),boundary,bytes-boundary,NULL,&right,error,sizeof(error)) &&
        ce_csl_inline_to_output(left,output,error,sizeof(error)) &&
        ce_csl_output_append(output," ",1,0,NULL,error,sizeof(error)) &&
        ce_csl_inline_to_output(right,output,error,sizeof(error));
    ce_csl_inline_free(left); ce_csl_inline_free(right); return ok;
}
static bool render(const CeCslStyle *style, const CeCslLocale *locale,
    const char *id, const cJSON *item_json, const cJSON *cite_json, bool bibliography,
    CeCslOutputString *text, size_t *nodes) {
    CeCslInputItem item = {.id = id, .data = item_json};
    CeCslInputCite cite = {.data = cite_json};
    CeCslGeneratedVariables generated = {.citation_number={true,true,1},
        .first_reference_note_number={true,false,0}, .year_suffix={true,"",0}, .citation_label={true,"",0}};
    CeCslEvalRequest request = {.style=style,.locale=locale,.position=CE_CSL_POSITION_FIRST,
        .section=bibliography ? ce_csl_style_bibliography(style) : ce_csl_style_citation(style),
        .variables={.item=&item,.cite=bibliography ? NULL : &cite,.generated=&generated}};
    CeCslEvaluation *evaluation = NULL; CeCslFinalized *finalized = NULL;
    CeCslOutput *output = NULL;
    bool ok = ce_csl_evaluate(&request, NULL, &evaluation, error, sizeof(error)) &&
        ce_csl_finalize(evaluation, NULL, &finalized, error, sizeof(error));
    if (!ok) goto done;
    *nodes += ce_csl_evaluation_summary(evaluation)->node_count;
    output = ce_csl_output_new(NULL, error, sizeof(error));
    if (!output) { ok=false; goto done; }
    const CeCslEvalDecoration *d = &ce_csl_eval_node_info(ce_csl_evaluation_root(evaluation))->decoration;
    /* Single-cite adapter: the citation layout wraps once. Document cluster
     * joining/sorting/collapse is outside this test's materializer boundary. */
    if (!bibliography && d->prefix)
        ok = ce_csl_output_append(output,d->prefix,strlen(d->prefix),d->effective_format,NULL,error,sizeof(error));
    if (ok) ok = append_entry(&request,evaluation,finalized,bibliography,output);
    if (ok && !bibliography && d->suffix)
        ok = ce_csl_output_append(output,d->suffix,strlen(d->suffix),d->effective_format,NULL,error,sizeof(error));
    if (ok) ok = ce_csl_output_plain(output,text,error,sizeof(error));
    if (ok) {
        size_t bytes=0; ce_csl_inline_text(ce_csl_finalized_inline(finalized),&bytes);
        for (size_t i=0;i<ce_csl_finalized_span_count(finalized);++i) {
            const CeCslFinalSpan *s=ce_csl_finalized_span(finalized,i);
            assert(s && s->offset<=bytes && s->length<=bytes-s->offset);
            if (s->kind==CE_CSL_FINAL_PERSON)
                assert(ce_csl_names_result_person(ce_csl_eval_node_names(s->node),s->person_index));
        }
    }
done:
    ce_csl_output_free(output); ce_csl_finalized_free(finalized); ce_csl_evaluation_free(evaluation);
    return ok;
}
static const char *types[] = {"article", "article-journal", "article-magazine", "article-newspaper", "bill", "book", "broadcast",
    "chapter", "classic", "collection", "dataset", "document", "entry", "entry-dictionary", "entry-encyclopedia", "event",
    "figure", "graphic", "hearing", "interview", "legal_case", "legislation", "manuscript", "map", "motion_picture",
    "musical_score", "pamphlet", "paper-conference", "patent", "performance", "periodical", "personal_communication",
    "post", "post-weblog", "regulation", "report", "review", "review-book", "software", "song", "speech", "standard",
    "thesis", "treaty", "webpage"};
int main(int argc, char **argv) {
    if (argc!=6) { fprintf(stderr,"Usage: %s APA IEEE GB2015 en-US zh-CN\n",argv[0]); return 2; }
    CeCslXml *xml[2]; const CeCslXmlNode *roots[2];
    for (size_t i=0;i<2;++i) {
        char *raw=read_file(argv[i+4]); xml[i]=ce_csl_xml_parse(raw,strlen(raw),error,sizeof(error));
        assert(xml[i]); roots[i]=xml[i]->root; free(raw);
    }
    CeCslLocaleCatalog catalog={.roots=roots,.root_count=2};
    size_t failures=0, checks=0, matrix=0, nodes=0, normative_divergences=0;
    for (size_t profile=0;profile<3;++profile) {
        CeCslStyle *style=NULL; char *raw=read_file(argv[profile+1]);
        assert(ce_csl_style_load(&style,raw,strlen(raw),error,sizeof(error))); free(raw);
        CeCslLocale *locale=ce_csl_locale_new(ce_csl_style_root(style),&catalog,
            profile==2 ? "zh-CN" : "en-US",NULL,error,sizeof(error)); assert(locale);
        for (size_t fixture=0;fixture<sizeof(goldens)/sizeof(*goldens);++fixture) {
            const Golden *g=&goldens[fixture]; cJSON *item=cJSON_Parse(g->item), *cite=cJSON_Parse(g->cite);
            assert(item && cite);
            for (size_t section=0;section<2;++section) {
                CeCslOutputString actual={0}; const char *expected=section ? g->bibliography[profile] : g->citation[profile];
                if (profile==2 && section && !strcmp(g->id,"webpage")) {
                    /* CSL 1.0.2: non-personal names lack name parts; family-part
                     * uppercase does not apply to an institution's literal. */
                    assert(strcmp(expected,gb_literal_normative));
                    expected=gb_literal_normative; ++normative_divergences;
                }
                bool ok=render(style,locale,g->id,item,cite,section!=0,&actual,&nodes);
                ++checks;
                if (!ok || strcmp(actual.data,expected)) {
                    ++failures;
                    fprintf(stderr,"DIFF profile=%zu fixture=%s section=%s\n expected: %s\n actual:   %s\n",
                        profile,g->id,section ? "bibliography" : "citation",expected,ok ? actual.data : error);
                }
                ce_csl_output_string_free(&actual);
            }
            cJSON_Delete(item); cJSON_Delete(cite);
        }
        cJSON *item=cJSON_Parse(goldens[1].item), *cite=cJSON_Parse("{\"locator\":\"20-22\",\"label\":\"page\"}");
        assert(item && cite);
        cJSON_AddStringToObject(item,"publisher","Example Press");
        cJSON_AddStringToObject(item,"publisher-place","Example City");
        cJSON_AddItemToObject(item,"editor",cJSON_Parse("[{\"family\":\"Smith\",\"given\":\"Alex\"}]"));
        for (size_t variant=0;variant<2;++variant) {
            if (variant) {
                cJSON_DeleteItemFromObjectCaseSensitive(item,"author");
                cJSON_DeleteItemFromObjectCaseSensitive(item,"editor");
                cJSON_DeleteItemFromObjectCaseSensitive(item,"issued");
            }
            for (size_t type=0;type<sizeof(types)/sizeof(*types);++type) {
                cJSON_ReplaceItemInObject(item,"type",cJSON_CreateString(types[type]));
                for (size_t section=0;section<2;++section) {
                    CeCslOutputString actual={0}; ++matrix;
                    bool ok=render(style,locale,"matrix",item,cite,section!=0,&actual,&nodes);
                    if (!ok || !actual.length) {
                        ++failures;
                        fprintf(stderr,"MATRIX profile=%zu variant=%zu type=%s section=%zu: %s\n",
                            profile,variant,types[type],section,ok ? "empty output" : error);
                    }
                    ce_csl_output_string_free(&actual);
                }
            }
        }
        cJSON_Delete(item); cJSON_Delete(cite); ce_csl_locale_free(locale); ce_csl_style_free(style);
    }
    for (size_t i=0;i<2;++i) ce_csl_xml_free(xml[i]);
    printf("CSL profiles: %zu goldens, %zu typed-tree/finalizer variants, %zu nodes, %zu differences, %zu documented oracle/spec divergences\n",
        checks,matrix,nodes,failures,normative_divergences);
    return failures ? 1 : 0;
}
