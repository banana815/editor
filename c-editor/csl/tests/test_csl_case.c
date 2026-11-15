#include "csl_case.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char error[256];
#define OK(call) do { bool success = (call); if (!success) fprintf(stderr, "%s: %s\n", #call, error); assert(success); } while (0)
static void check_map(const char *source, size_t size, const CeCslCaseMappedText *out) {
    size_t input = 0, output = 0;
    for (size_t i = 0; i < out->span_count; ++i) {
        const CeCslCaseSpan *span = &out->spans[i];
        size_t before = input; uint32_t cp;
        assert(ce_csl_unicode_next(source, size, &input, &cp) == CE_CSL_UNICODE_OK);
        assert(span->source_offset == before && span->source_length == input - before);
        assert(span->output_offset == output && span->output_length <= out->text.length - output);
        assert(ce_csl_unicode_validate(out->text.data + output, span->output_length, NULL) == CE_CSL_UNICODE_OK);
        output += span->output_length;
    }
    assert(input == size && output == out->text.length && !out->text.data[output]);
}
static void expect(const char *text, CeCslTextCase mode, const char *item, const char *style, const char *expected) {
    CeCslCaseMappedText mapped = {0};
    OK(ce_csl_case_transform_mapped(text, strlen(text), mode, item, style, NULL, &mapped, error, sizeof error));
    if (strcmp(mapped.text.data, expected)) fprintf(stderr, "Case %d input '%s': got '%s', expected '%s'\n", (int)mode, text, mapped.text.data, expected);
    assert(!strcmp(mapped.text.data, expected) && mapped.text.length == strlen(expected));
    check_map(text, strlen(text), &mapped);
    CeCslOutputString plain = {0};
    OK(ce_csl_case_transform(text, strlen(text), mode, item, style, NULL, &plain, error, sizeof error));
    assert(!strcmp(plain.data, expected));
    ce_csl_output_string_free(&plain); ce_csl_case_mapped_free(&mapped);
    assert(!mapped.text.data && !mapped.spans && !mapped.span_count);
}
static void test_specification_cases(void) {
    expect("the history of science and art", CE_CSL_CASE_TITLE, NULL, NULL, "The History of Science and Art");
    expect("THE HISTORY OF SCIENCE AND ART", CE_CSL_CASE_TITLE, NULL, NULL, "The History of Science and Art");
    expect("THE HISTORY OF SCIENCE AND ART", CE_CSL_CASE_SENTENCE, NULL, NULL, "The history of science and art");
    expect("THE HISTORY", CE_CSL_CASE_CAPITALIZE_ALL, NULL, NULL, "THE HISTORY");
    expect("the iPhone and NASA report", CE_CSL_CASE_TITLE, NULL, NULL, "The iPhone and NASA Report");
    expect("THE iPhone OF NASA", CE_CSL_CASE_TITLE, NULL, NULL, "THE iPhone of NASA");
    expect("the iPhone and NASA report", CE_CSL_CASE_SENTENCE, NULL, NULL, "The iPhone and NASA report");
    expect("iPhone and NASA", CE_CSL_CASE_CAPITALIZE_FIRST, NULL, NULL, "iPhone and NASA");
    expect(" lower Mixed UPPER ", CE_CSL_CASE_CAPITALIZE_ALL, NULL, NULL, " Lower Mixed UPPER ");
    expect("\"hello world\"", CE_CSL_CASE_CAPITALIZE_FIRST, NULL, NULL, "\"Hello world\"");
    expect("  THE AND  ", CE_CSL_CASE_SENTENCE, NULL, NULL, "  The and  ");
    expect("中文 hello world", CE_CSL_CASE_TITLE, NULL, NULL, "中文 Hello World");
    expect("123abc test", CE_CSL_CASE_TITLE, NULL, NULL, "123abc Test");
    expect("hello/world_again", CE_CSL_CASE_TITLE, NULL, NULL, "Hello/World_again");
    expect("hello—world hello–world", CE_CSL_CASE_TITLE, NULL, NULL, "Hello—World Hello–World");
    expect("the two-thirds solution", CE_CSL_CASE_TITLE, NULL, NULL, "The Two-Thirds Solution");
    expect("state-of-the-art", CE_CSL_CASE_TITLE, NULL, NULL, "State-of-the-Art");
    expect("a tale: the and the", CE_CSL_CASE_TITLE, NULL, NULL, "A Tale: The and The");
    expect("a tale: “the case”", CE_CSL_CASE_TITLE, NULL, NULL, "A Tale: “The Case”");
    expect("a tale; the case", CE_CSL_CASE_TITLE, NULL, NULL, "A Tale; the Case");
    expect("a tale? the case", CE_CSL_CASE_TITLE, NULL, NULL, "A Tale? the Case");
    expect("the and ", CE_CSL_CASE_TITLE, NULL, NULL, "The And ");
    expect("\talpha\r\nbeta  gamma\t", CE_CSL_CASE_TITLE, NULL, NULL, "\tAlpha\r\nBeta  Gamma\t");
    expect("... punctuation 🙂 123", CE_CSL_CASE_NONE, NULL, NULL, "... punctuation 🙂 123");
    expect("🙂 — 123", CE_CSL_CASE_TITLE, NULL, NULL, "🙂 — 123");
    expect("", CE_CSL_CASE_TITLE, NULL, NULL, "");
}
static void test_stop_phrases(void) {
    /* The pinned 117-entry schema includes multiword, hyphenated and apostrophe
     * entries. These are not approximated with an old ASCII single-word list. */
    expect("words according to others", CE_CSL_CASE_TITLE, NULL, NULL, "Words according to Others");
    expect("words ACCORDING TO others", CE_CSL_CASE_TITLE, NULL, NULL, "Words according to Others");
    expect("words ahead of others", CE_CSL_CASE_TITLE, NULL, NULL, "Words ahead of Others");
    expect("as regards the case", CE_CSL_CASE_TITLE, NULL, NULL, "As regards the Case");
    expect("a case according to", CE_CSL_CASE_TITLE, NULL, NULL, "A Case according To");
    expect("words according\t\u00a0to others", CE_CSL_CASE_TITLE, NULL, NULL, "Words according\t\u00a0to Others");
    expect("words according-to others", CE_CSL_CASE_TITLE, NULL, NULL, "Words According-to Others");
    expect("working VIS-À-VIS others", CE_CSL_CASE_TITLE, NULL, NULL, "Working vis-à-vis Others");
    expect("vis-à-vis the case", CE_CSL_CASE_TITLE, NULL, NULL, "Vis-à-vis the Case");
    expect("the works of d'artagnan", CE_CSL_CASE_TITLE, NULL, NULL, "The Works of d'Artagnan");
    expect("d'artagnan and l'architecture", CE_CSL_CASE_TITLE, NULL, NULL, "D'Artagnan and L'architecture");
    expect("the d’Artagnan affair", CE_CSL_CASE_TITLE, NULL, NULL, "The d’Artagnan Affair");
    expect("don't stop 'tis now", CE_CSL_CASE_TITLE, NULL, NULL, "Don't Stop 'Tis Now");
    expect("x v. y and x vs. y", CE_CSL_CASE_TITLE, NULL, NULL, "X v. Y and X vs. Y");
    expect("x versus the", CE_CSL_CASE_TITLE, NULL, NULL, "X versus The");
    expect("a sanderson anderson candy", CE_CSL_CASE_TITLE, NULL, NULL, "A Sanderson Anderson Candy");
}
static void test_language_and_unicode(void) {
    expect("the history of art", CE_CSL_CASE_TITLE, NULL, "de-DE", "the history of art");
    expect("the history of art", CE_CSL_CASE_TITLE, "", "de-DE", "the history of art");
    expect("the history of art", CE_CSL_CASE_TITLE, "en-GB", "de-DE", "The History of Art");
    expect("the history of art", CE_CSL_CASE_TITLE, "EN-us", "de-DE", "The History of Art");
    expect("the history of art", CE_CSL_CASE_TITLE, "en_US", "de-DE", "The History of Art");
    expect("the history of art", CE_CSL_CASE_TITLE, "english", NULL, "the history of art");
    expect("the history of art", CE_CSL_CASE_TITLE, "enochian", NULL, "the history of art");
    expect("the history of art", CE_CSL_CASE_TITLE, "zh-CN", "en-US", "the history of art");
    expect("中文 e\u0301cole", CE_CSL_CASE_TITLE, "zh-CN", NULL, "中文 e\u0301cole");
    expect("straße und ßeta ﬃeld ǳone", CE_CSL_CASE_TITLE, NULL, NULL, "Straße Und Sseta Ffield ǲone");
    expect("e\u0301cole i\u0307 test", CE_CSL_CASE_CAPITALIZE_ALL, NULL, NULL, "E\u0301cole I\u0307 Test");
    expect("i\u0307 test", CE_CSL_CASE_CAPITALIZE_FIRST, "lt", NULL, "I test");
    expect("istanbul ı", CE_CSL_CASE_UPPER, "tr", NULL, "İSTANBUL I");
    expect("I İ ISTANBUL", CE_CSL_CASE_LOWER, "tr", NULL, "ı i ıstanbul");
    expect("Straße", CE_CSL_CASE_UPPER, NULL, NULL, "STRASSE");
    expect("ΟΣΑ ΟΣ", CE_CSL_CASE_LOWER, NULL, NULL, "οσα ος");
    expect("ΣΟΣ ΣΊΓΜΑ", CE_CSL_CASE_TITLE, NULL, NULL, "Σος Σίγμα");
    expect("İ AND ΟΣ", CE_CSL_CASE_TITLE, NULL, NULL, "I\u0307 and Ος");
    expect("I\u0307A", CE_CSL_CASE_SENTENCE, "tr", NULL, "İa");
}
static void test_source_maps(void) {
    CeCslCaseMappedText out = {0};
    const char *source = "I\u0307A";
    OK(ce_csl_case_transform_mapped(source, strlen(source), CE_CSL_CASE_SENTENCE, "tr", NULL, NULL, &out, error, sizeof error));
    assert(!strcmp(out.text.data, "İa") && out.span_count == 3);
    assert(out.spans[0].source_offset == 0 && out.spans[0].source_length == 1 && out.spans[0].output_offset == 0 && out.spans[0].output_length == 2);
    assert(out.spans[1].source_offset == 1 && out.spans[1].source_length == 2 && out.spans[1].output_offset == 2 && out.spans[1].output_length == 0);
    assert(out.spans[2].source_offset == 3 && out.spans[2].source_length == 1 && out.spans[2].output_offset == 2 && out.spans[2].output_length == 1);
    check_map(source, strlen(source), &out);
    source = "ΟΣΑ"; /* Source font runs may split after ΟΣ; final sigma still sees Α. */
    OK(ce_csl_case_transform_mapped(source, strlen(source), CE_CSL_CASE_LOWER, NULL, NULL, NULL, &out, error, sizeof error));
    assert(!strcmp(out.text.data, "οσα") && out.span_count == 3);
    assert(out.spans[1].source_offset == 2 && out.spans[1].output_offset == 2);
    assert(!memcmp(out.text.data + out.spans[1].output_offset, "σ", out.spans[1].output_length));
    check_map(source, strlen(source), &out);
    source = "i\u0307 test";
    OK(ce_csl_case_transform_mapped(source, strlen(source), CE_CSL_CASE_CAPITALIZE_FIRST, "lt", NULL, NULL, &out, error, sizeof error));
    assert(!strcmp(out.text.data, "I test") && out.spans[1].output_length == 0);
    check_map(source, strlen(source), &out);
    source = "İ AND ΟΣ";
    OK(ce_csl_case_transform_mapped(source, strlen(source), CE_CSL_CASE_TITLE, NULL, NULL, NULL, &out, error, sizeof error));
    assert(out.spans[0].source_length == 2 && out.spans[0].output_length == 3);
    check_map(source, strlen(source), &out);
    source = "ǰ";
    OK(ce_csl_case_transform_mapped(source, strlen(source), CE_CSL_CASE_UPPER, NULL, NULL, NULL, &out, error, sizeof error));
    assert(!strcmp(out.text.data, "J\u030c") && out.span_count == 1 && out.spans[0].output_length == 3);
    ce_csl_case_mapped_free(&out);
}
static void test_rejections_and_limits(void) {
    CeCslCaseMappedText out = {0};
    OK(ce_csl_case_transform_mapped("old", 3, CE_CSL_CASE_NONE, NULL, NULL, NULL, &out, error, sizeof error));
    char *old = out.text.data;
    CeCslCaseSpan *old_spans = out.spans;
    const struct { const char *text; size_t size; } bad[] = {
        {"\xc0\xaf", 2}, {"\xed\xa0\x80", 3}, {"\xf4\x90\x80\x80", 4}, {"\xe4\xb8", 2},
        {"a\0b", 3}, {"\1", 1}, {NULL, 1}, {"x", CE_CSL_OUTPUT_MAX_BYTES + 1}
    };
    for (size_t i = 0; i < sizeof bad / sizeof *bad; ++i) {
        assert(!ce_csl_case_transform_mapped(bad[i].text, bad[i].size, CE_CSL_CASE_TITLE, NULL, NULL, NULL, &out, error, sizeof error));
        assert(*error && out.text.data == old && out.spans == old_spans && !strcmp(out.text.data, "old"));
    }
    assert(!ce_csl_case_transform_mapped("a", 1, (CeCslTextCase)99, NULL, NULL, NULL, &out, error, sizeof error));
    char *text = malloc(CE_CSL_OUTPUT_MAX_BYTES);
    assert(text);
    for (size_t i = 0; i < CE_CSL_OUTPUT_MAX_BYTES; i += 2) memcpy(text + i, "ŉ", 2);
    assert(!ce_csl_case_transform_mapped(text, CE_CSL_OUTPUT_MAX_BYTES, CE_CSL_CASE_UPPER, NULL, NULL, NULL, &out, error, sizeof error));
    assert(out.text.data == old && out.spans == old_spans && *error);
    memset(text, 'a', CE_CSL_OUTPUT_MAX_BYTES);
    OK(ce_csl_case_transform_mapped(text, CE_CSL_OUTPUT_MAX_BYTES, CE_CSL_CASE_NONE, NULL, NULL, NULL, &out, error, sizeof error));
    assert(out.span_count == CE_CSL_OUTPUT_MAX_BYTES && out.text.length == CE_CSL_OUTPUT_MAX_BYTES);
    check_map(text, CE_CSL_OUTPUT_MAX_BYTES, &out);
    free(text); ce_csl_case_mapped_free(&out);
    OK(ce_csl_case_transform_mapped(NULL, 0, CE_CSL_CASE_NONE, NULL, NULL, NULL, &out, error, sizeof error));
    assert(out.text.data && !*out.text.data && !out.span_count && !out.spans);
    ce_csl_case_mapped_free(&out);
}
typedef union Allocation {
    struct { size_t bytes, magic; } header;
    long double alignment;
    void *pointer;
} Allocation;
typedef struct Memory { size_t calls, fail_at, live, bytes, peak; } Memory;
static void *test_allocate(void *context, size_t bytes) {
    Memory *memory = context;
    if (++memory->calls == memory->fail_at) return NULL;
    Allocation *allocation = malloc(sizeof *allocation + bytes);
    assert(allocation); allocation->header.bytes = bytes; allocation->header.magic = 0xca5e;
    ++memory->live; memory->bytes += bytes;
    if (memory->bytes > memory->peak) memory->peak = memory->bytes;
    return allocation + 1;
}
static void test_deallocate(void *context, void *pointer) {
    Memory *memory = context;
    Allocation *allocation = (Allocation *)pointer - 1;
    assert(pointer && allocation->header.magic == 0xca5e && memory->live);
    assert(memory->bytes >= allocation->header.bytes);
    --memory->live; memory->bytes -= allocation->header.bytes; allocation->header.magic = 0;
    free(allocation);
}
static void test_oom(void) {
    const struct { const char *text, *language; CeCslTextCase mode; } cases[] = {
        {"İ AND ΟΣ", NULL, CE_CSL_CASE_TITLE}, {"I\u0307A", "tr", CE_CSL_CASE_SENTENCE},
        {"the ßeta d'artagnan vis-à-vis item", NULL, CE_CSL_CASE_TITLE},
        {"i\u0307 test", "lt", CE_CSL_CASE_CAPITALIZE_ALL}, {"Straße", NULL, CE_CSL_CASE_UPPER},
        {"ΟΣΑ", NULL, CE_CSL_CASE_LOWER}, {"copy 中文", NULL, CE_CSL_CASE_NONE}
    };
    for (size_t example = 0; example < sizeof cases / sizeof *cases; ++example) {
        bool succeeded = false;
        for (size_t point = 1; point < 180; ++point) {
            Memory memory = {0}, previous_memory = {0};
            CeCslOutputAllocator allocator = {&memory, test_allocate, test_deallocate};
            CeCslOutputAllocator previous = {&previous_memory, test_allocate, test_deallocate};
            CeCslCaseMappedText out = {0};
            OK(ce_csl_case_transform_mapped("old", 3, CE_CSL_CASE_NONE, NULL, NULL, &previous, &out, error, sizeof error));
            char *old = out.text.data; CeCslCaseSpan *spans = out.spans;
            size_t old_live = previous_memory.live;
            memory.fail_at = point;
            bool success = ce_csl_case_transform_mapped(cases[example].text, strlen(cases[example].text), cases[example].mode,
                cases[example].language, NULL, &allocator, &out, error, sizeof error);
            if (!success) {
                assert(*error && out.text.data == old && out.spans == spans && !strcmp(out.text.data, "old"));
                assert(!memory.live && !memory.bytes && previous_memory.live == old_live);
            } else {
                assert(!previous_memory.live && !previous_memory.bytes);
                assert(memory.live == 2 && memory.peak <= CE_CSL_CASE_MAX_WORKSPACE);
                check_map(cases[example].text, strlen(cases[example].text), &out);
                succeeded = true;
            }
            ce_csl_case_mapped_free(&out);
            assert(!memory.live && !memory.bytes && !previous_memory.live && !previous_memory.bytes);
            if (succeeded) break;
        }
        assert(succeeded);
    }
    Memory memory = {0};
    CeCslOutputAllocator allocator = {&memory, test_allocate, test_deallocate};
    CeCslOutputString out = {0};
    OK(ce_csl_case_transform("ßeta", strlen("ßeta"), CE_CSL_CASE_CAPITALIZE_FIRST, NULL, NULL, &allocator, &out, error, sizeof error));
    assert(memory.live == 1 && !strcmp(out.data, "Sseta"));
    char *old = out.data; memory.fail_at = memory.calls + 1;
    assert(!ce_csl_case_transform("next", 4, CE_CSL_CASE_TITLE, NULL, NULL, &allocator, &out, error, sizeof error));
    assert(out.data == old && memory.live == 1);
    allocator.allocate = NULL;
    assert(!ce_csl_case_transform("next", 4, CE_CSL_CASE_TITLE, NULL, NULL, &allocator, &out, error, sizeof error));
    assert(out.data == old); ce_csl_output_string_free(&out);
    assert(!memory.live && !memory.bytes);
}
int main(int argc, char **argv) {
    if (argc == 6 && !strcmp(argv[1], "--probe")) {
        CeCslOutputString out = {0};
        OK(ce_csl_case_transform(argv[5], strlen(argv[5]), (CeCslTextCase)atoi(argv[2]),
            strcmp(argv[3], "-") ? argv[3] : NULL, strcmp(argv[4], "-") ? argv[4] : NULL,
            NULL, &out, error, sizeof error));
        fwrite(out.data, 1, out.length, stdout); ce_csl_output_string_free(&out); return 0;
    }
    test_specification_cases();
    test_stop_phrases();
    test_language_and_unicode();
    test_source_maps();
    test_rejections_and_limits();
    test_oom();
    puts("C CSL text-case rules, pinned stop phrases, full Unicode title mappings, source spans, bounds and atomic OOM passed");
    return 0;
}
