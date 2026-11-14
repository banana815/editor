#include "csl_finalize.h"
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static char error[512];
typedef struct Fixture {
    CeCslStyle *style;
    CeCslLocale *locale;
    cJSON *item_json, *cite_json;
    CeCslInputItem item;
    CeCslInputCite cite;
    CeCslGeneratedVariables generated;
    CeCslEvalRequest request;
    CeCslEvaluation *result;
} Fixture;
static void fixture(Fixture *f, const char *macros, const char *section, const char *item, const char *cite) {
    memset(f, 0, sizeof(*f));
    const char *head = "<style xmlns='http://purl.org/net/xbiblio/csl' version='1.0' class='in-text' default-locale='en-US'>"
        "<info><title>Owned evaluator regression</title><id>https://example.invalid/evaluator</id>"
        "<updated>2026-01-01T00:00:00+00:00</updated></info>"
        "<locale><terms><term name='open-quote'>“</term><term name='close-quote'>”</term>"
        "<term name='open-inner-quote'>‘</term><term name='close-inner-quote'>’</term>"
        "<term name='page'><single>page</single><multiple>pages</multiple></term>"
        "<term name='volume'><single>volume</single><multiple>volumes</multiple></term>"
        "<term name='edition' gender='feminine'>edition</term>"
        "<term name='ordinal'>th</term><term name='ordinal-01' match='last-digit'>st</term>"
        "<term name='et-al'>et al.</term><term name='and'>and</term><term name='month-01'>January</term>"
        "</terms></locale>";
    bool bibliography = !strncmp(section, "<bibliography", 13);
    const char *citation = bibliography ? "<citation><layout><text value='unused'/></layout></citation>" : "";
    size_t size = strlen(head) + strlen(macros ? macros : "") + strlen(section) + strlen(citation) + 16;
    char *xml = malloc(size); assert(xml);
    snprintf(xml, size, "%s%s%s%s</style>", head, macros ? macros : "", citation, section);
    bool ok = ce_csl_style_load(&f->style, xml, strlen(xml), error, sizeof(error));
    if (!ok) fprintf(stderr, "style: %s\n", error);
    assert(ok); free(xml);
    f->locale = ce_csl_locale_new(ce_csl_style_root(f->style), NULL, "en-US", NULL, error, sizeof(error));
    if (!f->locale) fprintf(stderr, "locale: %s\n", error);
    assert(f->locale);
    f->item_json = cJSON_Parse(item ? item : "{}"); assert(f->item_json);
    f->item = (CeCslInputItem){.id = "owned-item", .data = f->item_json};
    if (cite) { f->cite_json = cJSON_Parse(cite); assert(f->cite_json); f->cite.data = f->cite_json; }
    f->generated.citation_number = (CeCslGeneratedNumber){true, true, 7};
    f->generated.first_reference_note_number = (CeCslGeneratedNumber){true, false, 0};
    f->generated.year_suffix = (CeCslGeneratedText){true, "", 0};
    f->generated.citation_label = (CeCslGeneratedText){true, "", 0};
    f->request = (CeCslEvalRequest){.style = f->style, .locale = f->locale,
        .section = ce_csl_style_citation(f->style), .position = CE_CSL_POSITION_FIRST,
        .variables = {.item = &f->item, .cite = cite ? &f->cite : NULL, .generated = &f->generated}};
    if (bibliography) f->request.section = ce_csl_style_bibliography(f->style);
}
static void fixture_free(Fixture *f) {
    ce_csl_evaluation_free(f->result); ce_csl_locale_free(f->locale); ce_csl_style_free(f->style);
    cJSON_Delete(f->item_json); cJSON_Delete(f->cite_json);
}
static void evaluate(Fixture *f) {
    bool ok = ce_csl_evaluate(&f->request, NULL, &f->result, error, sizeof(error));
    if (!ok) fprintf(stderr, "evaluate: %s\n", error);
    assert(ok);
}
static void text_is(const CeCslInline *value, const char *expected) {
    size_t size; const char *text = ce_csl_inline_text(value, &size);
    if (!text || size != strlen(expected) || memcmp(text, expected, size))
        fprintf(stderr, "expected <%s>, actual <%.*s>\n", expected, (int)size, text ? text : "NULL");
    assert(text && size == strlen(expected) && !memcmp(text, expected, size));
}
static void finalize(Fixture *f, CeCslFinalized **out, const char *expected) {
    evaluate(f);
    bool ok = ce_csl_finalize(f->result, NULL, out, error, sizeof(error));
    if (!ok) fprintf(stderr, "finalize: %s\n", error);
    assert(ok); text_is(ce_csl_finalized_inline(*out), expected);
    size_t size; const char *text = ce_csl_inline_text(ce_csl_finalized_inline(*out), &size);
    for (size_t i = 0; i < ce_csl_finalized_span_count(*out); ++i) {
        const CeCslFinalSpan *s = ce_csl_finalized_span(*out, i);
        assert(s->node && s->kind <= CE_CSL_FINAL_CLOSE_QUOTE && s->offset <= size && s->length <= size - s->offset);
        assert(s->offset == size || ((unsigned char)text[s->offset] & 0xc0) != 0x80);
        assert(s->offset + s->length == size || ((unsigned char)text[s->offset + s->length] & 0xc0) != 0x80);
    }
}
static void test_structure(void) {
    Fixture f; CeCslFinalized *out = NULL;
    fixture(&f, "<macro name='pair'><text value='X'/><text value='Y'/></macro>",
        "<citation><layout prefix='(' suffix=')' delimiter='; '><group delimiter='|'>"
        "<text value='A'/><choose><if variable='title'><text value='B'/><text value='C'/></if></choose>"
        "<group><text value='D'/><text value='E'/></group><text macro='pair'/></group><text variable='citation-number'/></layout></citation>",
        "{\"title\":\"yes\"}", NULL);
    finalize(&f, &out, "A|B|C|DE|XY7"); ce_csl_finalized_free(out); out = NULL; fixture_free(&f);
    fixture(&f, NULL, "<bibliography><layout prefix='[' suffix=']' font-style='italic'><group display='block' delimiter='|'>"
        "<names variable='author'><name/><substitute><text variable='title'/></substitute></names>"
        "<text variable='title'/><date variable='issued' prefix='(' suffix=')'><date-part name='year'/></date>"
        "</group></layout></bibliography>", "{\"title\":\"Book\",\"issued\":{\"date-parts\":[[2025]]}}", NULL);
    finalize(&f, &out, "[Book|(2025)]");
    bool display = false;
    for (size_t i = 0; i < ce_csl_finalized_span_count(out); ++i) {
        const CeCslFinalSpan *s = ce_csl_finalized_span(out, i);
        if (s->display == CE_CSL_EVAL_DISPLAY_BLOCK) { display = true; assert(s->offset == 1 && s->length == 11); }
    }
    assert(display);
    for (size_t i = 0; i < ce_csl_inline_run_count(ce_csl_finalized_inline(out)); ++i)
        assert(ce_csl_inline_run(ce_csl_finalized_inline(out), i)->format == CE_CSL_ITALIC);
    ce_csl_finalized_free(out); fixture_free(&f);
}
static void test_case_and_persons(void) {
    Fixture f; CeCslFinalized *out = NULL;
    fixture(&f, "<macro name='parts'><group delimiter='|'>"
        "<names variable='author'><name initialize-with='. '/></names><text variable='title' prefix='x.' suffix='.y'/>"
        "</group></macro>", "<bibliography><layout><text macro='parts' text-case='uppercase' strip-periods='true' font-style='italic' prefix='pre ' suffix=' post'/></layout></bibliography>",
        "{\"author\":[{\"family\":\"Straße\",\"given\":\"John\"}],\"title\":\"hello <span class='nocase'>i.Phone</span> <i>world</i>\"}", NULL);
    finalize(&f, &out, "pre J STRASSE|XHELLO iPhone WORLDY post");
    bool person = false, protected_seen = false, flip = false;
    size_t size; const char *text = ce_csl_inline_text(ce_csl_finalized_inline(out), &size);
    for (size_t i = 0; i < ce_csl_finalized_span_count(out); ++i) {
        const CeCslFinalSpan *s = ce_csl_finalized_span(out, i);
        if (s->kind == CE_CSL_FINAL_PERSON) {
            assert(s->person_index == 0 && s->length == 9 && !memcmp(text + s->offset, "J STRASSE", 9)); person = true;
        }
    }
    for (size_t i = 0; i < ce_csl_inline_run_count(ce_csl_finalized_inline(out)); ++i) {
        const CeCslInlineRun *run = ce_csl_inline_run(ce_csl_finalized_inline(out), i);
        if (run->nocase) { protected_seen = true; assert(run->length == 6 && !memcmp(text + run->offset, "iPhone", 6)); }
        if (run->offset > 4 && !(run->format & CE_CSL_ITALIC)) flip = true;
    }
    assert(person && protected_seen && flip);
    ce_csl_finalized_free(out); fixture_free(&f);
}
static void test_quotes(void) {
    Fixture f; CeCslFinalized *out = NULL;
    const char *options = "<locale xml:lang='en-US'><style-options punctuation-in-quote='true'/></locale>";
    fixture(&f, options, "<citation><layout><group delimiter='; '><text variable='title' quotes='true' suffix='.'/><text value='tail'/></group></layout></citation>", "{\"title\":\"Hello\"}", NULL);
    finalize(&f, &out, "“Hello.”; tail");
    size_t closes = 0;
    for (size_t i = 0; i < ce_csl_finalized_span_count(out); ++i)
        closes += ce_csl_finalized_span(out, i)->kind == CE_CSL_FINAL_CLOSE_QUOTE;
    assert(closes == 1 && !ce_csl_finalized_missing_term(out));
    ce_csl_finalized_free(out); out = NULL; fixture_free(&f);
    fixture(&f, "<locale xml:lang='en-US'><style-options punctuation-in-quote='true'/></locale><macro name='nested'><text variable='title' quotes='true'/></macro>",
        "<bibliography><layout><text macro='nested' quotes='true' suffix='.'/></layout></bibliography>", "{\"title\":\"Nested\"}", NULL);
    finalize(&f, &out, "“‘Nested.’”"); ce_csl_finalized_free(out); out = NULL; fixture_free(&f);
    fixture(&f, options, "<citation><layout><text variable='title' quotes='true' suffix='.'/></layout></citation>", "{\"title\":\"Really?\"}", NULL);
    finalize(&f, &out, "“Really?”"); ce_csl_finalized_free(out); out = NULL; fixture_free(&f);
    fixture(&f, NULL, "<citation><layout><text variable='title' quotes='true' suffix='.'/></layout></citation>", "{\"title\":\"Really?\"}", NULL);
    finalize(&f, &out, "“Really?”."); ce_csl_finalized_free(out); out = NULL; fixture_free(&f);
    fixture(&f, options, "<citation><layout><text variable='title' quotes='true' suffix=','/></layout></citation>", "{\"title\":\"Hello.\"}", NULL);
    finalize(&f, &out, "“Hello.,”"); ce_csl_finalized_free(out); fixture_free(&f);

    out = NULL;
    fixture(&f, options, "<citation><layout><text variable='title' quotes='true'/><text variable='publisher'/></layout></citation>",
        "{\"title\":\"Hello\",\"publisher\":\", literal\"}", NULL);
    finalize(&f, &out, "“Hello”, literal"); ce_csl_finalized_free(out); fixture_free(&f);
    out = NULL;
    fixture(&f, options, "<citation><layout><text variable='title' quotes='true'/><text variable='publisher' prefix=', '/></layout></citation>",
        "{\"title\":\"Hello\",\"publisher\":\"generated\"}", NULL);
    finalize(&f, &out, "“Hello,” generated"); ce_csl_finalized_free(out); fixture_free(&f);
    out = NULL;
    fixture(&f, options, "<citation><layout><text variable='title' quotes='true' suffix=' .'/></layout></citation>",
        "{\"title\":\"Hello\"}", NULL);
    finalize(&f, &out, "“Hello” ."); ce_csl_finalized_free(out); fixture_free(&f);
}
static void test_boundaries(void) {
    Fixture f; CeCslFinalized *out = NULL;
    fixture(&f, NULL, "<citation><layout><group delimiter='; '><text value='A.' suffix='.'/><text value='B?' suffix='.'/><text value='Literal... B,, C;;'/><text value='C;' suffix=';'/></group></layout></citation>", "{}", NULL);
    finalize(&f, &out, "A.; B?; Literal... B,, C;; C;");
    ce_csl_finalized_free(out); out = NULL; fixture_free(&f);
    fixture(&f, NULL, "<citation><layout><group delimiter='.'><text value='A.'/><text value='.B'/></group></layout></citation>", "{}", NULL);
    finalize(&f, &out, "A.B"); ce_csl_finalized_free(out); out = NULL; fixture_free(&f);
    fixture(&f, NULL, "<citation><layout><text variable='title'/></layout></citation>", "{\"title\":\"<script>x</script> &amp; <i>bad\"}", NULL);
    finalize(&f, &out, "<script>x</script> &amp; <i>bad"); ce_csl_finalized_free(out); fixture_free(&f);
    out = NULL;
    fixture(&f, NULL, "<citation><layout><text variable='title'/><text variable='publisher'/></layout></citation>",
        "{\"title\":\"A,\",\"publisher\":\",B\"}", NULL);
    finalize(&f, &out, "A,,B"); ce_csl_finalized_free(out); fixture_free(&f);
}
static void test_limits_and_empty(void) {
    Fixture f; CeCslFinalized *out = NULL;
    fixture(&f, NULL, "<citation><layout><group delimiter='; '><text variable='title'/><text value='unused'/></group></layout></citation>", "{}", NULL);
    finalize(&f, &out, ""); assert(!ce_csl_finalized_span_count(out));
    ce_csl_finalized_free(out); fixture_free(&f); out = NULL;
    fixture(&f, NULL, "<citation><layout><text variable='title'/></layout></citation>", "{}", NULL);
    size_t large_size = CE_CSL_EVALUATE_MAX_BYTES / 2; /* Bound copy plus raw leaf body. */
    char *large = malloc(large_size + 1); assert(large);
    memset(large, 'a', large_size); large[large_size] = 0;
    cJSON_AddStringToObject(f.item_json, "title", large);
    finalize(&f, &out, large);
    CeCslFinalized *old = out;
    CeCslOutputAllocator invalid = {0};
    assert(!ce_csl_finalize(f.result, &invalid, &out, error, sizeof(error)) && out == old);
    ce_csl_finalized_free(out); fixture_free(&f); free(large);
}
typedef struct Allocation { size_t calls, fail_at, live; } Allocation;
static void *allocate(void *context, size_t bytes) {
    Allocation *a = context; if (++a->calls == a->fail_at) return NULL;
    void *p = malloc(bytes); if (p) ++a->live; return p;
}
static void release(void *context, void *p) { Allocation *a = context; if (p) { assert(a->live); --a->live; free(p); } }
static void test_oom(void) {
    Fixture f; CeCslFinalized *old = NULL, *probe = NULL;
    fixture(&f, "<locale xml:lang='en-US'><style-options punctuation-in-quote='true'/></locale><macro name='entry'><group delimiter='; '><names variable='author'><name initialize-with='. '/></names>"
        "<text variable='title' quotes='true' suffix='.'/><date variable='issued'><date-part name='year'/></date></group></macro>",
        "<bibliography><layout><text macro='entry' text-case='uppercase'/></layout></bibliography>",
        "{\"author\":[{\"family\":\"Straße\",\"given\":\"John\"}],\"title\":\"A <i>book</i>\",\"issued\":{\"date-parts\":[[2025]]}}", NULL);
    finalize(&f, &old, "J. STRASSE; “A BOOK.”; 2025");
    Allocation a = {0}; CeCslOutputAllocator hooks = {&a, allocate, release};
    assert(ce_csl_finalize(f.result, &hooks, &probe, error, sizeof(error)));
    size_t calls = a.calls; ce_csl_finalized_free(probe); assert(!a.live);
    for (size_t i = 1; i <= calls; ++i) {
        a = (Allocation){.fail_at = i}; probe = old;
        bool ok = ce_csl_finalize(f.result, &hooks, &probe, error, sizeof(error));
        if (ok || probe != old || a.live || !*error) fprintf(stderr, "failure point %zu/%zu: ok %d live %zu %s\n", i, calls, ok, a.live, error);
        assert(!ok && probe == old && !a.live && *error);
    }
    assert(!ce_csl_finalize(NULL, NULL, &old, error, sizeof(error)) && old);
    ce_csl_finalized_free(old); fixture_free(&f);
    printf("finalizer allocation failure points: %zu\n", calls);
}
static void test_independent_unicode_spans(void) {
    Fixture f;
    CeCslFinalized *out = NULL;
    fixture(&f, "<macro name='m'><text value='I'/><text value='\u0307' font-style='italic'/><text value='A'/></macro>",
        "<bibliography><layout><text macro='m' text-case='lowercase'/></layout></bibliography>",
        "{\"language\":\"tr\"}", NULL);
    finalize(&f, &out, "ia");
    bool deletion = false;
    for (size_t i = 0; i < ce_csl_finalized_span_count(out); ++i) {
        const CeCslFinalSpan *s = ce_csl_finalized_span(out, i);
        const char *value = ce_csl_xml_attribute(ce_csl_eval_node_info(s->node)->occurrence.source, "value");
        if (value && !strcmp(value, "\u0307")) {
            assert(s->offset == 1 && s->length == 0);
            deletion = true;
        }
    }
    assert(deletion);
    ce_csl_finalized_free(out); out = NULL; fixture_free(&f);
    fixture(&f, "<locale xml:lang='en-US'><style-options punctuation-in-quote='true'/></locale><macro name='inner'><text variable='title' quotes='true'/></macro><macro name='outer'><text macro='inner' quotes='true' suffix='.'/></macro>",
        "<bibliography><layout><text macro='outer' text-case='uppercase'/></layout></bibliography>",
        "{\"title\":\"ß🙂\"}", NULL);
    finalize(&f, &out, "“‘SS🙂.’”");
    ce_csl_finalized_free(out); out = NULL; fixture_free(&f);
    fixture(&f, "<locale xml:lang='en-US'><style-options punctuation-in-quote='true'/></locale>",
        "<citation><layout><text variable='title' quotes='true'/><text variable='publisher'/></layout></citation>",
        "{\"title\":\"中文🙂\",\"publisher\":\", B\"}", NULL);
    finalize(&f, &out, "“中文🙂”, B");
    ce_csl_finalized_free(out); out = NULL; fixture_free(&f);
    fixture(&f, NULL,
        "<citation><layout><group><text variable='title'/><text variable='publisher'/></group></layout></citation>",
        "{\"title\":\"A,\",\"publisher\":\",B\"}", NULL);
    finalize(&f, &out, "A,,B");
    ce_csl_finalized_free(out); out = NULL; fixture_free(&f);
    fixture(&f,
        "<locale xml:lang='en-US'><style-options punctuation-in-quote='true'/></locale><macro name='m'><text variable='title' font-style='italic' quotes='true' suffix='.'/></macro>",
        "<bibliography><layout><text macro='m' text-case='uppercase'/></layout></bibliography>",
        "{\"title\":\"中<span class='nocase'>iPhone</span>🙂\"}", NULL);
    finalize(&f, &out, "“中iPhone🙂.”");
    const CeCslInline *rich = ce_csl_finalized_inline(out);
    size_t rich_size;
    const char *rich_text = ce_csl_inline_text(rich, &rich_size);
    bool period = false, protected = false, quote = false;
    for (size_t i = 0; i < ce_csl_inline_run_count(rich); ++i) {
        const CeCslInlineRun *run = ce_csl_inline_run(rich, i);
        if (run->offset <= rich_size - 4 && run->offset + run->length > rich_size - 4) {
            assert(!(run->format & CE_CSL_ITALIC));
            period = true;
        }
        if (run->nocase) {
            assert(run->length == 6 && !memcmp(rich_text + run->offset, "iPhone", 6));
            protected = true;
        }
    }
    for (size_t i = 0; i < ce_csl_finalized_span_count(out); ++i) {
        const CeCslFinalSpan *s = ce_csl_finalized_span(out, i);
        if (s->kind == CE_CSL_FINAL_CLOSE_QUOTE) {
            assert(s->offset == rich_size - 3 && s->length == 3);
            quote = true;
        }
    }
    assert(period && protected && quote);
    ce_csl_finalized_free(out); out = NULL; fixture_free(&f);
    fixture(&f,
        "<macro name='m'><names variable='author' prefix='[' suffix=']'><name initialize-with='. '/></names></macro>",
        "<bibliography><layout><text macro='m' text-case='lowercase' prefix='outer '/></layout></bibliography>",
        "{\"language\":\"tr\",\"author\":[{\"family\":\"I\u0307X\",\"given\":\"Jane\"}]}", NULL);
    finalize(&f, &out, "outer [j. ix]");
    bool found_person = false;
    for (size_t i = 0; i < ce_csl_finalized_span_count(out); ++i) {
        const CeCslFinalSpan *s = ce_csl_finalized_span(out, i);
        if (s->kind == CE_CSL_FINAL_PERSON) {
            assert(s->offset == 7 && s->length == 5 && s->person_index == 0);
            const CeCslNameRendered *person = ce_csl_names_result_person(ce_csl_eval_node_names(s->node), 0);
            assert(!strcmp(person->identity->family, "I\u0307X"));
            found_person = true;
        }
    }
    assert(found_person);
    ce_csl_finalized_free(out); out = NULL; fixture_free(&f);
    puts("Independent finalizer review: zero-length Unicode spans, expansion+nested quotes, metadata punctuation passed");
}

int main(void) {
    test_structure(); test_case_and_persons(); test_quotes(); test_boundaries(); test_limits_and_empty(); test_independent_unicode_spans(); test_oom();
    puts("CSL finalization: semantic spans, case, rich fields, quotation boundaries and atomic OOM passed"); return 0;
}
