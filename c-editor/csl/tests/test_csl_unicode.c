#include "csl_unicode.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <utf8proc.h>

#define COUNT(a) (sizeof(a) / sizeof((a)[0]))
typedef struct MappingFixture {
    uint32_t codepoint, expected[3];
    unsigned count;
} MappingFixture;
typedef struct ScriptFixture { uint32_t first,last; const char *script; } ScriptFixture;
#include "../data/unicode-test-vectors.inc"

static void expect_bytes(const char *input, size_t size, CeCslUnicodeOperation operation,
                         const char *language, const char *expected, size_t length) {
    CeCslUnicodeText out = {0};
    CeCslUnicodeStatus status = ce_csl_unicode_transform(input, size, operation, language, NULL, &out);
    if (status != CE_CSL_UNICODE_OK || out.size != length ||
        (length && memcmp(out.data, expected, length))) {
        fprintf(stderr, "Unicode mismatch: op=%d lang=%s status=%s size=%zu expected=%zu\n",
            (int)operation, language ? language : "root", ce_csl_unicode_status_string(status), out.size, length);
        if (out.data) {
            size_t i;
            fprintf(stderr, "actual:");
            for (i = 0; i < out.size; ++i) fprintf(stderr, " %02x", (unsigned char)out.data[i]);
            fprintf(stderr, "\nexpected:");
            for (i = 0; i < length; ++i) fprintf(stderr, " %02x", (unsigned char)expected[i]);
            fprintf(stderr, "\n");
        }
        abort();
    }
    assert(out.data && out.data[length] == '\0');
    ce_csl_unicode_text_free(&out);
    assert(!out.data && !out.size && !out.deallocate && !out.allocator_context);
    ce_csl_unicode_text_free(&out);
}
static void expect(const char *input, CeCslUnicodeOperation operation,
                   const char *language, const char *expected) {
    expect_bytes(input, strlen(input), operation, language, expected, strlen(expected));
}

static void test_utf8(void) {
    static const struct { unsigned char bytes[6]; size_t size, error; } invalid[] = {
        {{0x80},1,0}, {{0xc0,0xaf},2,0}, {{0xc1,0xbf},2,0},
        {{0xe0,0x80,0xaf},3,0}, {{0xf0,0x80,0x80,0xaf},4,0},
        {{0xed,0xa0,0x80},3,0}, {{0xed,0xbf,0xbf},3,0},
        {{0xf4,0x90,0x80,0x80},4,0}, {{0xf5,0x80,0x80,0x80},4,0},
        {{0xff},1,0}, {{0xfe},1,0}, {{0xc2},1,0}, {{0xe2,0x82},2,0},
        {{0xf0,0x9f,0x99},3,0}, {{'a',0xc2,'b'},3,1},
        {{'x',0xe2,0x28,0xa1},4,1}, {{0xf8,0x88,0x80,0x80,0x80},5,0}
    };
    size_t i, bad, offset;
    uint32_t cp;
    char encoded[4] = {1,2,3,4};
    for (i = 0; i < COUNT(invalid); ++i) {
        CeCslUnicodeText out = {0};
        const char *text = (const char *)invalid[i].bytes;
        bad = 999;
        assert(ce_csl_unicode_validate(text, invalid[i].size, &bad) == CE_CSL_UNICODE_INVALID_UTF8);
        assert(bad == invalid[i].error);
        offset = bad; cp = 0x1234;
        assert(ce_csl_unicode_next(text, invalid[i].size, &offset, &cp) == CE_CSL_UNICODE_INVALID_UTF8);
        assert(offset == bad && cp == 0x1234);
        assert(ce_csl_unicode_transform(text, invalid[i].size, CE_CSL_UNICODE_UPPER, NULL, NULL, &out) == CE_CSL_UNICODE_INVALID_UTF8);
        assert(ce_csl_unicode_transform(text, invalid[i].size, CE_CSL_UNICODE_NFD, NULL, NULL, &out) == CE_CSL_UNICODE_INVALID_UTF8);
        assert(!out.data);
    }
    assert(ce_csl_unicode_validate(NULL, 0, &bad) == CE_CSL_UNICODE_OK && bad == 0);
    assert(ce_csl_unicode_validate(NULL, 1, NULL) == CE_CSL_UNICODE_INVALID_ARGUMENT);
    offset = 0; cp = 42;
    assert(ce_csl_unicode_next(NULL, 0, &offset, &cp) == CE_CSL_UNICODE_END && offset == 0 && cp == 42);
    assert(!ce_csl_unicode_encode(0xd800, encoded) && encoded[0] == 1);
    assert(!ce_csl_unicode_encode(0x110000, encoded) && encoded[0] == 1);
    /* Every Unicode scalar roundtrips, including NUL, noncharacters, unassigned
     * values and supplementary scripts. Surrogates are not Unicode scalars. */
    for (cp = 0; cp <= 0x10ffff; ++cp) {
        uint32_t decoded = UINT32_MAX;
        size_t n = ce_csl_unicode_encode(cp, encoded);
        if (cp >= 0xd800 && cp <= 0xdfff) { assert(!n); continue; }
        assert(n >= 1 && n <= 4);
        offset = 0;
        assert(ce_csl_unicode_next(encoded, n, &offset, &decoded) == CE_CSL_UNICODE_OK);
        assert(offset == n && decoded == cp);
    }
    expect_bytes("A\0B", 3, CE_CSL_UNICODE_LOWER, NULL, "a\0b", 3);
    expect_bytes("A\0B", 3, CE_CSL_UNICODE_NFC, NULL, "A\0B", 3);
    expect_bytes("A\0B", 3, CE_CSL_UNICODE_NFD, NULL, "A\0B", 3);
}

static void mappings(const MappingFixture *fixtures, size_t count,
                     CeCslUnicodeOperation operation, const char *language) {
    size_t i;
    for (i = 0; i < count; ++i) {
        char input[4], expected[12];
        size_t j, n = ce_csl_unicode_encode(fixtures[i].codepoint, input), size = 0;
        for (j = 0; j < fixtures[i].count; ++j)
            size += ce_csl_unicode_encode(fixtures[i].expected[j], expected + size);
        expect_bytes(input, n, operation, language, expected, size);
    }
}

static void test_casing(void) {
    mappings(upper_fixtures, COUNT(upper_fixtures), CE_CSL_UNICODE_UPPER, NULL);
    mappings(lower_fixtures, COUNT(lower_fixtures), CE_CSL_UNICODE_LOWER, NULL);
    mappings(title_fixtures, COUNT(title_fixtures), CE_CSL_UNICODE_TITLE_MAPPING, NULL);
    expect("ßﬃǳ", CE_CSL_UNICODE_TITLE_MAPPING, NULL, "SsFfiǲ");
    expect("i\u0307", CE_CSL_UNICODE_TITLE_MAPPING, "lt", "I");
    mappings(fold_fixtures, COUNT(fold_fixtures), CE_CSL_UNICODE_CASEFOLD, NULL);
    mappings(turkic_fixtures, COUNT(turkic_fixtures), CE_CSL_UNICODE_CASEFOLD, "tr");
    mappings(turkic_fixtures, COUNT(turkic_fixtures), CE_CSL_UNICODE_CASEFOLD, "az");
    expect("Straße ﬃ ﬁ ǰ ΐ", CE_CSL_UNICODE_UPPER, NULL, "STRASSE FFI FI J\u030c Ι\u0308\u0301");
    expect("Straße STRASSE ﬃ", CE_CSL_UNICODE_CASEFOLD, NULL, "strasse strasse ffi");
    expect("Iİıi", CE_CSL_UNICODE_LOWER, NULL, "ii\u0307ıi");
    expect("Iİıi", CE_CSL_UNICODE_UPPER, NULL, "IİII");
    expect("Iİıi", CE_CSL_UNICODE_LOWER, "TR_tr", "ıiıi");
    expect("Iİıi", CE_CSL_UNICODE_UPPER, "az-Latn-AZ", "IİIİ");
    expect("Iİıi", CE_CSL_UNICODE_CASEFOLD, "lt", "ii\u0307ıi");
    expect("Iİıi", CE_CSL_UNICODE_LOWER, "tricky", "ii\u0307ıi");
    expect("Iİıi", CE_CSL_UNICODE_LOWER, "t", "ii\u0307ıi");
    expect("I\u0307", CE_CSL_UNICODE_LOWER, "tr", "i");
    expect("I\u0323\u0307", CE_CSL_UNICODE_LOWER, "az", "i\u0323");
    expect("I\u0301\u0307", CE_CSL_UNICODE_LOWER, "tr", "ı\u0301\u0307");
    expect("I\u034f\u0307", CE_CSL_UNICODE_LOWER, "tr", "ı\u034f\u0307");
    /* Turkic folding uses T mappings, not contextual lowercase; it does not
     * preserve canonical equivalence for I + dot. */
    expect("I\u0307", CE_CSL_UNICODE_CASEFOLD, "tr", "ı\u0307");
    expect("I\u0301 J\u0300 Į\u0323\u0301 ÌÍĨ", CE_CSL_UNICODE_LOWER, "lt-LT",
           "i\u0307\u0301 j\u0307\u0300 į\u0307\u0323\u0301 i\u0307\u0300i\u0307\u0301i\u0307\u0303");
    expect("I\u0323", CE_CSL_UNICODE_LOWER, "lt", "i\u0323");
    expect("I\u034f\u0301", CE_CSL_UNICODE_LOWER, "lt", "i\u034f\u0301");
    expect("i\u0307 j\u0323\u0307 į\u0307", CE_CSL_UNICODE_UPPER, "lt", "I J\u0323 Į");
    expect("i\u0301\u0307", CE_CSL_UNICODE_UPPER, "lt", "I\u0301\u0307");
    expect("i\u034f\u0307", CE_CSL_UNICODE_UPPER, "lt", "I\u034f\u0307");
    expect("i\u0307", CE_CSL_UNICODE_UPPER, NULL, "I\u0307");
    expect("ΟΣ ΟΣΑ Σ ΟΣ\u0301 ΟΣ' ΟΣ'Α", CE_CSL_UNICODE_LOWER, NULL,
           "ος οσα σ ος\u0301 ος' οσ'α");
    expect("\u0345Σ Α\u0345Σ ΑΣ\u0345 ΑΣ\u0345Α", CE_CSL_UNICODE_LOWER, NULL,
           "\u0345σ α\u0345ς ας\u0345 ασ\u0345α");
    expect("ΑΣ\u200d ΑΣ\u200dΑ", CE_CSL_UNICODE_LOWER, NULL, "ας\u200d ασ\u200dα");
    expect("ΟΣ ςΣσ", CE_CSL_UNICODE_CASEFOLD, NULL, "οσ σσσ");
    expect("中文🙂\U00010400", CE_CSL_UNICODE_LOWER, NULL, "中文🙂\U00010428");
}

static void normalization_columns(const char *const column[5], const size_t lengths[5]) {
    size_t i;
    for (i = 0; i < 5; ++i) {
        size_t expected = i < 3 ? 1 : 3;
        expect_bytes(column[i], lengths[i], CE_CSL_UNICODE_NFC, NULL,
                     column[expected], lengths[expected]);
        expected = i < 3 ? 2 : 4;
        expect_bytes(column[i], lengths[i], CE_CSL_UNICODE_NFD, NULL,
                     column[expected], lengths[expected]);
    }
}
static void test_normalization(void) {
    size_t i, j;
    for (i = 0; i < COUNT(normalization_fixtures); ++i) {
        size_t sizes[5];
        for (j = 0; j < 5; ++j) sizes[j] = strlen(normalization_fixtures[i][j]);
        normalization_columns(normalization_fixtures[i], sizes);
    }
    expect("e\u0301", CE_CSL_UNICODE_NFC, NULL, "é");
    expect("\u1100\u1161\u11a8", CE_CSL_UNICODE_NFC, NULL, "각");
    expect("ﬃ ①", CE_CSL_UNICODE_NFC, NULL, "ﬃ ①"); /* NFC is not NFKC. */
    expect("a\u0315\u0300", CE_CSL_UNICODE_NFC, NULL, "à\u0315");
    expect("\u0301\u0323", CE_CSL_UNICODE_NFC, NULL, "\u0323\u0301");
    expect("é", CE_CSL_UNICODE_NFD, NULL, "e\u0301");
    expect("각", CE_CSL_UNICODE_NFD, NULL, "\u1100\u1161\u11a8");
    expect("ﬃ ①", CE_CSL_UNICODE_NFD, NULL, "ﬃ ①"); /* NFD is not NFKD. */
    expect("a\u0315\u0300", CE_CSL_UNICODE_NFD, NULL, "a\u0300\u0315");
    expect("\u0301\u0323", CE_CSL_UNICODE_NFD, NULL, "\u0323\u0301");
    /* Equal combining classes remain stable; a CCC=0 mark is a boundary. */
    expect("a\u0315\u0301\u0300\u034f\u0315\u0323", CE_CSL_UNICODE_NFD,
        NULL, "a\u0301\u0300\u0315\u034f\u0323\u0315");
    /* A long reversed non-starter run catches accidental quadratic sorting. */
    {
        const size_t n = 20000;
        char *input = malloc(1 + n * 4), *expected = malloc(n * 4);
        assert(input && expected);
        input[0] = 'a';
        memcpy(expected, "à", 2);
        for (i = 0; i < n; ++i) {
            memcpy(input + 1 + i * 2, "\u0315", 2);
            memcpy(input + 1 + n * 2 + i * 2, "\u0300", 2);
            if (i < n - 1) memcpy(expected + 2 + i * 2, "\u0300", 2);
            memcpy(expected + n * 2 + i * 2, "\u0315", 2);
        }
        expect_bytes(input, 1 + n * 4, CE_CSL_UNICODE_NFC, NULL, expected, n * 4);
        free(input); free(expected);
    }
    /* A late low-CCC mark must move across the entire run, not just the last
     * fixed-size chunk. Include both sides of the former candidate's 32 limit. */
    {
        const size_t lengths[] = {31, 32, 33, 65, 20000};
        for (j = 0; j < COUNT(lengths); ++j) {
            size_t n = lengths[j], bytes = 1 + n * 2 + 4;
            char *input = malloc(bytes), *expected = malloc(bytes);
            assert(input && expected);
            input[0] = expected[0] = 'a';
            for (i = 0; i < n; ++i) {
                memcpy(input + 1 + i * 2, "\u0315", 2);
                memcpy(expected + 5 + i * 2, "\u0315", 2);
            }
            memcpy(input + 1 + n * 2, "\u0301\u0300", 4);
            memcpy(expected + 1, "\u0301\u0300", 4);
            expect_bytes(input, bytes, CE_CSL_UNICODE_NFD, NULL, expected, bytes);
            /* Leading combining sequences have the same ordering obligation. */
            expect_bytes(input + 1, bytes - 1, CE_CSL_UNICODE_NFD, NULL, expected + 1, bytes - 1);
            free(input); free(expected);
        }
    }
}

typedef struct AllocatorState { size_t calls, fail_at, live, largest; } AllocatorState;
static void *test_allocate(void *context, size_t size) {
    AllocatorState *a = context;
    void *p;
    if (a->calls++ == a->fail_at) return NULL;
    p = malloc(size);
    if (p) { ++a->live; if (size > a->largest) a->largest = size; }
    return p;
}
static void test_deallocate(void *context, void *pointer) {
    AllocatorState *a = context;
    assert(pointer && a->live);
    --a->live;
    free(pointer);
}
static void test_failures(void) {
    CeCslUnicodeOptions options = {0};
    CeCslUnicodeText out = {0};
    CeCslUnicodeStatus status;
    size_t i;
    assert(ce_csl_unicode_transform("x", 1, (CeCslUnicodeOperation)99, NULL, NULL, &out) == CE_CSL_UNICODE_INVALID_ARGUMENT);
    options.allocate = test_allocate;
    assert(ce_csl_unicode_transform("x", 1, CE_CSL_UNICODE_NFC, NULL, &options, &out) == CE_CSL_UNICODE_INVALID_ARGUMENT);
    memset(&options, 0, sizeof(options));
    options.max_input_bytes = 1;
    assert(ce_csl_unicode_transform("xx", 2, CE_CSL_UNICODE_NFC, NULL, &options, &out) == CE_CSL_UNICODE_LIMIT);
    options.max_input_bytes = 99; options.max_output_bytes = 2;
    assert(ce_csl_unicode_transform("ﬃ", 3, CE_CSL_UNICODE_UPPER, NULL, &options, &out) == CE_CSL_UNICODE_LIMIT);
    options.max_output_bytes = 3;
    assert(ce_csl_unicode_transform("ﬃ", 3, CE_CSL_UNICODE_UPPER, NULL, &options, &out) == CE_CSL_UNICODE_OK);
    assert(out.size == 3 && !strcmp(out.data, "FFI"));
    /* Existing outputs and inputs are unchanged when the caller fails. */
    {
        char *pointer = out.data;
        assert(ce_csl_unicode_transform("a", 1, CE_CSL_UNICODE_LOWER, NULL, NULL, &out) == CE_CSL_UNICODE_INVALID_ARGUMENT);
        assert(out.data == pointer && !strcmp(pointer, "FFI"));
    }
    ce_csl_unicode_text_free(&out);
    options.max_workspace_bytes = 3;
    assert(ce_csl_unicode_transform("x", 1, CE_CSL_UNICODE_UPPER, NULL, &options, &out) == CE_CSL_UNICODE_LIMIT);
    options.max_workspace_bytes = 7;
    assert(ce_csl_unicode_transform("x", 1, CE_CSL_UNICODE_NFC, NULL, &options, &out) == CE_CSL_UNICODE_LIMIT);
    {
        AllocatorState a = {0, SIZE_MAX, 0, 0};
        memset(&options, 0, sizeof(options));
        options.allocate = test_allocate; options.deallocate = test_deallocate;
        options.allocator_context = &a;
        options.max_output_bytes = 1;
        assert(ce_csl_unicode_transform("e\u0301", 3, CE_CSL_UNICODE_NFC, NULL, &options, &out) == CE_CSL_UNICODE_LIMIT);
        assert(a.live == 0 && a.calls == 1 && !out.data);
        options.max_output_bytes = 2;
        assert(ce_csl_unicode_transform("e\u0301", 3, CE_CSL_UNICODE_NFC, NULL, &options, &out) == CE_CSL_UNICODE_OK);
        assert(out.size == 2 && a.live == 1);
        ce_csl_unicode_text_free(&out);
        a.calls = 0; a.fail_at = 0;
        assert(ce_csl_unicode_transform(NULL, 0, CE_CSL_UNICODE_NFC, NULL, &options, &out) == CE_CSL_UNICODE_OOM);
        assert(a.calls == 1 && !a.live && !out.data);
        a.calls = 0;
        assert(ce_csl_unicode_transform("\xff", 1, CE_CSL_UNICODE_UPPER, NULL, &options, &out) == CE_CSL_UNICODE_INVALID_UTF8);
        assert(!a.calls && !a.live);
    }
    memset(&options, 0, sizeof(options));
    options.max_output_bytes = 2;
    assert(ce_csl_unicode_transform("é", 2, CE_CSL_UNICODE_NFD, NULL, &options, &out) == CE_CSL_UNICODE_LIMIT);
    options.max_output_bytes = 3; options.max_workspace_bytes = 15;
    assert(ce_csl_unicode_transform("é", 2, CE_CSL_UNICODE_NFD, NULL, &options, &out) == CE_CSL_UNICODE_LIMIT);
    options.max_workspace_bytes = 16;
    assert(ce_csl_unicode_transform("é", 2, CE_CSL_UNICODE_NFD, NULL, &options, &out) == CE_CSL_UNICODE_OK);
    assert(out.size == 3 && !memcmp(out.data, "e\u0301", 3));
    ce_csl_unicode_text_free(&out);
    for (i = 0; i <= CE_CSL_UNICODE_NFD; ++i) {
        size_t fail;
        for (fail = 0; fail < 5; ++fail) {
            AllocatorState a = {0, fail, 0, 0};
            char input[] = "İ Straße e\u0301";
            char saved[sizeof(input)];
            memcpy(saved, input, sizeof(input));
            memset(&options, 0, sizeof(options));
            options.allocate = test_allocate; options.deallocate = test_deallocate;
            options.allocator_context = &a;
            out.size = 777;
            status = ce_csl_unicode_transform(input, strlen(input), (CeCslUnicodeOperation)i, NULL, &options, &out);
            assert(!memcmp(input, saved, sizeof(input)));
            if (status == CE_CSL_UNICODE_OK) {
                assert(a.live == 1);
                ce_csl_unicode_text_free(&out);
                assert(a.live == 0);
                break;
            }
            assert(status == CE_CSL_UNICODE_OOM && !out.data && out.size == 777 && a.live == 0);
        }
        assert(fail < 5);
    }
    expect_bytes(NULL, 0, CE_CSL_UNICODE_NFC, NULL, "", 0);
    expect_bytes(NULL, 0, CE_CSL_UNICODE_NFD, NULL, "", 0);
    expect_bytes(NULL, 0, CE_CSL_UNICODE_LOWER, "lt", "", 0);
    assert(ce_csl_unicode_transform("x", SIZE_MAX, CE_CSL_UNICODE_NFC, NULL, NULL, &out) == CE_CSL_UNICODE_LIMIT);
}

static void test_casing_maps(void) {
    CeCslUnicodeMappedText out = {0};
    assert(ce_csl_unicode_transform_mapped("ßi\u0307", strlen("ßi\u0307"), CE_CSL_UNICODE_TITLE_MAPPING,
        "lt", NULL, &out) == CE_CSL_UNICODE_OK);
    assert(!strcmp(out.text.data, "SsI") && out.span_count == 3);
    const CeCslUnicodeSpan expected[] = {{0,2,0,2}, {2,1,2,1}, {3,2,3,0}};
    for (size_t i = 0; i < out.span_count; ++i) {
        assert(out.spans[i].source_offset == expected[i].source_offset);
        assert(out.spans[i].source_length == expected[i].source_length);
        assert(out.spans[i].output_offset == expected[i].output_offset);
        assert(out.spans[i].output_length == expected[i].output_length);
    }
    char *old = out.text.data;
    assert(ce_csl_unicode_transform_mapped("a", 1, CE_CSL_UNICODE_LOWER, NULL, NULL, &out) == CE_CSL_UNICODE_INVALID_ARGUMENT);
    assert(out.text.data == old && out.span_count == 3);
    ce_csl_unicode_mapped_free(&out);
    assert(ce_csl_unicode_transform_mapped("I\u0307A", strlen("I\u0307A"), CE_CSL_UNICODE_LOWER,
        "tr", NULL, &out) == CE_CSL_UNICODE_OK);
    assert(!strcmp(out.text.data, "ia") && out.span_count == 3);
    assert(out.spans[1].source_offset == 1 && out.spans[1].source_length == 2);
    assert(out.spans[1].output_offset == 1 && out.spans[1].output_length == 0);
    assert(out.spans[2].source_offset == 3 && out.spans[2].output_offset == 1);
    ce_csl_unicode_mapped_free(&out);
    assert(ce_csl_unicode_transform_mapped("x", 1, CE_CSL_UNICODE_NFC, NULL, NULL, &out) == CE_CSL_UNICODE_INVALID_ARGUMENT);
    out.text.size = 777;
    assert(ce_csl_unicode_transform_mapped("é", 2, CE_CSL_UNICODE_NFD, NULL, NULL, &out) == CE_CSL_UNICODE_INVALID_ARGUMENT);
    assert(!out.text.data && out.text.size == 777 && !out.spans && !out.span_count);
    CeCslUnicodeOptions options = {0};
    options.max_workspace_bytes = sizeof(uint32_t) + sizeof(CeCslUnicodeSpan) - 1;
    assert(ce_csl_unicode_transform_mapped("x", 1, CE_CSL_UNICODE_LOWER, NULL, &options, &out) == CE_CSL_UNICODE_LIMIT);
    for (size_t point = 0; point <= 3; ++point) {
        AllocatorState memory = {0, point, 0, 0};
        memset(&options, 0, sizeof options);
        options.allocate = test_allocate; options.deallocate = test_deallocate; options.allocator_context = &memory;
        out.text.size = 777;
        CeCslUnicodeStatus status = ce_csl_unicode_transform_mapped("İΣ", strlen("İΣ"), CE_CSL_UNICODE_LOWER, NULL, &options, &out);
        if (point < 3) assert(status == CE_CSL_UNICODE_OOM && !out.text.data && out.text.size == 777 && !out.spans && !memory.live);
        else {
            assert(status == CE_CSL_UNICODE_OK && memory.live == 2 && out.span_count == 2);
            assert(out.spans[0].source_length == 2 && out.spans[0].output_length == 3);
            ce_csl_unicode_mapped_free(&out); assert(!memory.live);
        }
    }
    assert(ce_csl_unicode_transform_mapped(NULL, 0, CE_CSL_UNICODE_LOWER, NULL, NULL, &out) == CE_CSL_UNICODE_OK);
    assert(out.text.data && !*out.text.data && !out.spans && !out.span_count);
    ce_csl_unicode_mapped_free(&out);
}

static void test_properties(void) {
    static const uint32_t spaces[] = {9,10,11,12,13,32,0x85,0xa0,0x1680,
        0x2000,0x2001,0x2002,0x2003,0x2004,0x2005,0x2006,0x2007,0x2008,0x2009,0x200a,
        0x2028,0x2029,0x202f,0x205f,0x3000};
    size_t i;
    for (i = 0; i < COUNT(spaces); ++i) assert(ce_csl_unicode_is_space(spaces[i]));
    assert(!ce_csl_unicode_is_space(0x200b) && !ce_csl_unicode_is_space(0xfeff));
    assert(!ce_csl_unicode_is_space('x') && !ce_csl_unicode_is_space(0xd800));
    assert(ce_csl_unicode_is_letter(0x4e2d) && ce_csl_unicode_is_letter(0x10400));
    assert(ce_csl_unicode_is_mark(0x301) && ce_csl_unicode_is_number(0x0661));
    assert(ce_csl_unicode_is_word_codepoint('_') && ce_csl_unicode_is_word_codepoint(0x301));
    assert(!ce_csl_unicode_is_word_codepoint('-') && !ce_csl_unicode_is_word_codepoint(0x1f642));
    assert(ce_csl_unicode_is_cased(0x345) && ce_csl_unicode_is_case_ignorable(0x345));
    assert(ce_csl_unicode_is_cased('A') && !ce_csl_unicode_is_cased(0x4e2d));
    assert(ce_csl_unicode_is_case_ignorable('\'') && ce_csl_unicode_is_case_ignorable(0x200d));
    assert(!ce_csl_unicode_is_letter(UINT32_MAX) && !ce_csl_unicode_is_mark(0x110000));
    size_t range=0;
    for(uint32_t cp=0;cp<=0x10ffff;++cp) {
        while(range<COUNT(script_fixtures)&&cp>script_fixtures[range].last)++range;
        const char *expected=range<COUNT(script_fixtures)&&cp>=script_fixtures[range].first ? script_fixtures[range].script : "Unknown";
        assert(!strcmp(ce_csl_unicode_script(cp),expected));
    }
    assert(!strcmp(ce_csl_unicode_script(UINT32_MAX),"Unknown"));
    assert(!strcmp(ce_csl_unicode_script(0x20000),"Han"));
    assert(!strcmp(ce_csl_unicode_script(0x30fc),"Common"));
    assert(!strcmp(ce_csl_unicode_script(0x301),"Inherited"));
}

/* Optional exhaustive official corpus run, without embedding its 2.8 MB text
 * in the product: test_csl_unicode path/to/NormalizationTest.txt */
static void normalization_file(const char *path) {
    FILE *file = fopen(path, "rb");
    char line[4096];
    size_t cases = 0;
    assert(file);
    while (fgets(line, sizeof(line), file)) {
        char storage[5][1024], *p = line;
        const char *columns[5];
        size_t lengths[5] = {0}, i;
        while (*p == ' ' || *p == '\t') ++p;
        if (*p == '#' || *p == '@' || *p == '\r' || *p == '\n' || !*p) continue;
        for (i = 0; i < 5; ++i) {
            columns[i] = storage[i];
            while (*p && *p != ';') {
                char *end;
                unsigned long cp;
                while (*p == ' ' || *p == '\t') ++p;
                if (*p == ';') break;
                cp = strtoul(p, &end, 16);
                assert(end != p && cp <= 0x10ffff && lengths[i] + 4 <= sizeof(storage[i]));
                lengths[i] += ce_csl_unicode_encode((uint32_t)cp, storage[i] + lengths[i]);
                p = end;
            }
            assert(*p == ';'); ++p;
        }
        normalization_columns(columns, lengths);
        ++cases;
    }
    assert(!ferror(file));
    fclose(file);
    assert(cases > 19000);
    printf("Official NormalizationTest.txt: %zu rows / %zu NFC + %zu NFD checks passed\n", cases, cases * 5, cases * 5);
}

int main(int argc, char **argv) {
    assert(!strcmp(utf8proc_unicode_version(), CE_CSL_UNICODE_VERSION));
    test_utf8();
    test_casing();
    test_normalization();
    test_failures();
    test_casing_maps();
    test_properties();
    if (argc > 1) normalization_file(argv[1]);
    printf("Unicode 18: strict UTF-8, %zu upper/%zu lower/%zu title/%zu fold mappings, contextual tr/az/lt/sigma, %zu NFC/NFD vectors, scalar maps, limits and OOM passed\n",
        COUNT(upper_fixtures), COUNT(lower_fixtures), COUNT(title_fixtures), COUNT(fold_fixtures), COUNT(normalization_fixtures));
    return 0;
}
