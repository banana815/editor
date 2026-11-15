#include "csl_inline.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define COUNT(a) (sizeof(a) / sizeof((a)[0]))
static char error[256];
static CeCslInline *parse(const char *s, unsigned format) {
    CeCslInline *t = NULL;
    assert(ce_csl_inline_parse(s, strlen(s), format, NULL, &t, NULL, error, sizeof(error)));
    assert(!*error);
    return t;
}
static void text_is(const CeCslInline *t, const char *expected) {
    size_t n;
    const char *s = ce_csl_inline_text(t, &n);
    if (!s || n != strlen(expected) || memcmp(s, expected, n))
        fprintf(stderr, "actual <%s> expected <%s>\n", s ? s : "NULL", expected);
    assert(s && n == strlen(expected) && !memcmp(s, expected, n));
    size_t offset = 0;
    for (size_t i = 0; i < ce_csl_inline_run_count(t); ++i) {
        const CeCslInlineRun *r = ce_csl_inline_run(t, i);
        assert(r && r->length && r->offset == offset);
        offset += r->length;
    }
    assert(offset == n);
}
static unsigned format_at(const CeCslInline *t, size_t offset) {
    for (size_t i = 0; i < ce_csl_inline_run_count(t); ++i) {
        const CeCslInlineRun *r = ce_csl_inline_run(t, i);
        if (offset >= r->offset && offset < r->offset + r->length)
            return r->format;
    }
    assert(0);
    return 0;
}
static bool protected_at(const CeCslInline *t, size_t offset) {
    for (size_t i = 0; i < ce_csl_inline_run_count(t); ++i) {
        const CeCslInlineRun *r = ce_csl_inline_run(t, i);
        if (offset >= r->offset && offset < r->offset + r->length)
            return r->nocase;
    }
    assert(0);
    return false;
}
static void test_parse(void) {
    CeCslInline *t = parse("A<i>B<i>C</i>D</i>E", 0);
    text_is(t, "ABCDE");
    assert(format_at(t, 0) == 0 && format_at(t, 1) == CE_CSL_ITALIC && format_at(t, 2) == 0 &&
           format_at(t, 3) == CE_CSL_ITALIC && format_at(t, 4) == 0);
    ce_csl_inline_free(t);
    t = parse("A<i>B</i>C", CE_CSL_ITALIC);
    text_is(t, "ABC");
    assert(format_at(t, 0) == CE_CSL_ITALIC && format_at(t, 1) == 0 && format_at(t, 2) == CE_CSL_ITALIC);
    ce_csl_inline_free(t);
    t = parse("<i>x</i><b>y</b>", CE_CSL_OBLIQUE | CE_CSL_LIGHT);
    text_is(t, "xy");
    assert(format_at(t, 0) == (CE_CSL_ITALIC | CE_CSL_LIGHT) &&
           format_at(t, 1) == (CE_CSL_OBLIQUE | CE_CSL_BOLD));
    ce_csl_inline_free(t);
    t = parse("<b>x<b>y</b>z</b>", CE_CSL_BOLD);
    text_is(t, "xyz");
    assert(format_at(t, 0) == 0 && format_at(t, 1) == CE_CSL_BOLD && format_at(t, 2) == 0);
    ce_csl_inline_free(t);
    t = parse("<sc>x<span style=' font-variant : small-caps ; '>y</span>z</sc>", 0);
    text_is(t, "xyz");
    assert(format_at(t, 0) == CE_CSL_SMALL_CAPS && format_at(t, 1) == 0 &&
           format_at(t, 2) == CE_CSL_SMALL_CAPS);
    ce_csl_inline_free(t);
    t = parse("<sup>x<sub>y</sub>z</sup>", CE_CSL_SUBSCRIPT);
    text_is(t, "xyz");
    assert(format_at(t, 0) == CE_CSL_SUPERSCRIPT && format_at(t, 1) == CE_CSL_SUBSCRIPT &&
           format_at(t, 2) == CE_CSL_SUPERSCRIPT);
    ce_csl_inline_free(t);
    t = parse("<span class=\"nocase\">x<i>y<span class='nocase'>z</span></i></span>w", 0);
    text_is(t, "xyzw");
    assert(protected_at(t, 0) && protected_at(t, 1) && protected_at(t, 2) && !protected_at(t, 3));
    assert(format_at(t, 1) == CE_CSL_ITALIC);
    ce_csl_inline_free(t);
    t = parse("<i>x</i><i>y</i>", 0);
    text_is(t, "xy");
    assert(ce_csl_inline_run_count(t) == 1);
    ce_csl_inline_free(t);
    t = parse("<i></i><b></b>", 0);
    text_is(t, "");
    assert(!ce_csl_inline_run_count(t));
    ce_csl_inline_free(t);
    t = parse(" \t中<i>文</i>🙂\r\n ", 0);
    text_is(t, " \t中文🙂\r\n ");
    ce_csl_inline_free(t);
    const char *literal[] = {"<script>alert(1)</script>",
                             "<I>x</I>",
                             "<span class='nodecor'>x</span>",
                             "a<br/>b",
                             "&amp; &lt; &#x3B1;",
                             "<span style='color:red'>x</span>",
                             "<span class='nocase' onclick='bad'>x</span>"};
    for (size_t i = 0; i < COUNT(literal); ++i) {
        CeCslInlineDiagnostic d = {0};
        t = NULL;
        assert(ce_csl_inline_parse(literal[i], strlen(literal[i]), 0, NULL, &t, &d, error, sizeof(error)));
        text_is(t, literal[i]);
        assert(!d.malformed_markup);
        ce_csl_inline_free(t);
    }
    const char *bad[] = {"<i>x",
                         "x</i>",
                         "<i>x</b>",
                         "<i>a<b>b</i>c</b>",
                         "<i>a</i> and <b>unclosed",
                         "<span class='nocase'>x"};
    for (size_t i = 0; i < COUNT(bad); ++i) {
        CeCslInlineDiagnostic d = {0};
        t = NULL;
        assert(ce_csl_inline_parse(bad[i], strlen(bad[i]), 0, NULL, &t, &d, error, sizeof(error)));
        text_is(t, bad[i]);
        assert(d.malformed_markup);
        ce_csl_inline_free(t);
    }
    t = parse("<span class='nocase'>a<span class='unknown'>b</span>c</span>d", 0);
    text_is(t, "a<span class='unknown'>b</span>cd");
    assert(protected_at(t, 0) && protected_at(t, 20) &&
           !protected_at(t, strlen(ce_csl_inline_text(t, NULL)) - 1));
    ce_csl_inline_free(t);
}
static void converted(const char *raw, CeCslTextCase mode, const char *lang, const char *expected) {
    CeCslInline *t = parse(raw, 0), *out = NULL;
    assert(ce_csl_inline_case(t, mode, lang, NULL, NULL, &out, NULL, error, sizeof(error)));
    text_is(out, expected);
    ce_csl_inline_free(t);
    ce_csl_inline_free(out);
}
static void test_case_and_maps(void) {
    converted("fo<span class='nocase'>oB</span>ar", CE_CSL_CASE_UPPER, NULL, "FOoBAR");
    converted("the <span class='nocase'>iPhone</span> AND SCIENCE", CE_CSL_CASE_TITLE, "en",
              "The iPhone and SCIENCE");
    converted("THE <span class='nocase'>IPHONE</span> AND SCIENCE", CE_CSL_CASE_TITLE, "en",
              "The IPHONE and Science");
    converted("<sc>small</sc>", CE_CSL_CASE_UPPER, NULL,
              "SMALL"); /* small-caps is a font, not implicit nocase. */
    CeCslInline *t = parse("<i>ΟΣ</i>Α", 0), *out = NULL;
    CeCslInlineMap map = {0};
    assert(ce_csl_inline_case(t, CE_CSL_CASE_LOWER, NULL, NULL, NULL, &out, &map, error, sizeof(error)));
    text_is(out, "οσα");
    assert(format_at(out, 0) == CE_CSL_ITALIC && format_at(out, 2) == CE_CSL_ITALIC &&
           format_at(out, 4) == 0);
    assert(map.count == 3 && map.source_size == 6 && map.output_size == 6);
    ce_csl_inline_free(t);
    ce_csl_inline_free(out);
    ce_csl_inline_map_free(&map);
    out = NULL;
    t = parse("<b>ß</b><span class='nocase'>ß</span>x", 0);
    assert(ce_csl_inline_case(t, CE_CSL_CASE_UPPER, NULL, NULL, NULL, &out, &map, error, sizeof(error)));
    text_is(out, "SSßX");
    assert(map.count == 3 && map.spans[0].source_length == 2 && map.spans[0].output_length == 2 &&
           map.spans[1].output_length == 2);
    assert(format_at(out, 1) == CE_CSL_BOLD && protected_at(out, 2));
    ce_csl_inline_free(t);
    ce_csl_inline_free(out);
    ce_csl_inline_map_free(&map);
    out = NULL;
    t = parse("I<span class='nocase'>\u0307</span>A", 0);
    assert(ce_csl_inline_case(t, CE_CSL_CASE_LOWER, "tr", NULL, NULL, &out, &map, error, sizeof(error)));
    text_is(out, "i\u0307a");
    assert(map.spans[1].source_offset == 1 && map.spans[1].output_offset == 1 &&
           map.spans[1].output_length == 2);
    assert(map.spans[2].output_offset == 3);
    ce_csl_inline_free(t);
    ce_csl_inline_free(out);
    ce_csl_inline_map_free(&map);
    out = NULL;
    t = parse("<i>I\u0307</i>A", 0);
    assert(ce_csl_inline_case(t, CE_CSL_CASE_LOWER, "tr", NULL, NULL, &out, &map, error, sizeof(error)));
    text_is(out, "ia");
    assert(map.spans[1].output_length == 0);
    size_t start = 777, len = 888;
    assert(ce_csl_inline_map_range(&map, 1, 2, &start, &len) && start == 1 && len == 0);
    assert(ce_csl_inline_map_range(&map, 3, 1, &start, &len) && start == 1 && len == 1);
    start = 777;
    len = 888;
    assert(!ce_csl_inline_map_range(&map, 2, 1, &start, &len) && start == 777 && len == 888);
    assert(ce_csl_inline_map_range(&map, 4, 0, &start, &len) && start == 2 && len == 0);
    ce_csl_inline_free(t);
    ce_csl_inline_free(out);
    ce_csl_inline_map_free(&map);
    out = NULL;
    t = parse("<span class='nocase'>iPhone</span> hello", 0);
    assert(ce_csl_inline_case(t, CE_CSL_CASE_UPPER, NULL, NULL, NULL, &t, &map, error, sizeof(error)));
    text_is(t, "iPhone HELLO");
    assert(protected_at(t, 0));
    assert(ce_csl_inline_case(t, CE_CSL_CASE_LOWER, NULL, NULL, NULL, &t, &map, error, sizeof(error)));
    text_is(t, "iPhone hello");
    assert(protected_at(t, 0));
    ce_csl_inline_free(t);
    ce_csl_inline_map_free(&map);
    t = parse("", 0);
    assert(ce_csl_inline_case(t, CE_CSL_CASE_TITLE, NULL, NULL, NULL, &t, &map, error, sizeof(error)));
    text_is(t, "");
    assert(!map.count && ce_csl_inline_map_range(&map, 0, 0, &start, &len) && !start && !len);
    ce_csl_inline_free(t);
    ce_csl_inline_map_free(&map);
}
static void test_concat_output(void) {
    CeCslInline *a = parse("<span class='nocase'>iPhone</span>", 0), *b = parse("<i> and α</i>", 0),
                *clone = NULL;
    assert(ce_csl_inline_clone(a, NULL, &clone, error, sizeof(error)));
    assert(ce_csl_inline_concat(a, b, error, sizeof(error)));
    assert(ce_csl_inline_case(a, CE_CSL_CASE_UPPER, NULL, NULL, NULL, &a, NULL, error, sizeof(error)));
    text_is(a, "iPhone AND Α");
    assert(protected_at(a, 0));
    text_is(clone, "iPhone");
    assert(ce_csl_inline_concat(clone, clone, error, sizeof(error)));
    text_is(clone, "iPhoneiPhone");
    assert(ce_csl_inline_run_count(clone) == 1);
    ce_csl_inline_free(clone);
    ce_csl_inline_free(a);
    ce_csl_inline_free(b);
    a = parse("<i>x</i>y", 0);
    assert(ce_csl_inline_concat(a, a, error, sizeof(error)));
    text_is(a, "xyxy");
    assert(ce_csl_inline_run_count(a) == 4);
    ce_csl_inline_free(a);
    CeCslOutput *ir = ce_csl_output_new(NULL, error, sizeof(error)),
                *final = ce_csl_output_new(NULL, error, sizeof(error));
    assert(ir && final);
    assert(
        ce_csl_output_append(ir, "a&", 2, CE_CSL_BOLD, "https://example.com/?x=1&y=2", error, sizeof(error)));
    assert(
        ce_csl_output_append(ir, "b", 1, CE_CSL_BOLD, "https://example.com/?x=1&y=2", error, sizeof(error)));
    a = NULL;
    assert(ce_csl_inline_from_output(ir, true, NULL, &a, error, sizeof(error)));
    assert(ce_csl_inline_run_count(a) == 1);
    ce_csl_output_free(ir);
    assert(ce_csl_inline_case(a, CE_CSL_CASE_UPPER, NULL, NULL, NULL, &a, NULL, error, sizeof(error)));
    text_is(a, "a&b");
    assert(ce_csl_inline_to_output(a, final, error, sizeof(error)));
    CeCslOutputString html = {0};
    assert(ce_csl_output_html(final, &html, error, sizeof(error)));
    assert(strstr(html.data, "https://example.com/?x=1&amp;y=2") && strstr(html.data, "a&amp;b"));
    ce_csl_output_string_free(&html);
    ce_csl_output_free(final);
    ce_csl_inline_free(a);
    a = parse("<script>evil</script>", 0);
    final = ce_csl_output_new(NULL, error, sizeof(error));
    assert(final && ce_csl_inline_to_output(a, final, error, sizeof(error)));
    assert(ce_csl_output_html(final, &html, error, sizeof(error)));
    assert(strstr(html.data, "&lt;script&gt;evil&lt;/script&gt;") && !strstr(html.data, "<script>"));
    ce_csl_output_string_free(&html);
    ce_csl_output_free(final);
    ce_csl_inline_free(a);
}
static void test_slice(void) {
    CeCslInline *source = parse("a<i>中🙂</i><span class='nocase'>e\u0301</span>z", 0), *out = NULL;
    assert(ce_csl_inline_slice(source, 1, 8, NULL, &out, error, sizeof(error)));
    text_is(out, "中🙂e");
    assert(ce_csl_inline_run_count(out) == 2 && format_at(out, 0) == CE_CSL_ITALIC);
    assert(protected_at(out, 7));
    /* Scalar boundaries allow a combining character without its base. */
    assert(ce_csl_inline_slice(source, 9, 2, NULL, &out, error, sizeof(error)));
    text_is(out, "\u0301");
    assert(protected_at(out, 0));
    CeCslInline *old = out;
    const size_t bad[][2] = {{2, 0}, {1, 1}, {5, 3}, {9, 1}, {13, 0}, {12, 1}, {SIZE_MAX, 1}, {0, SIZE_MAX}};
    for (size_t i = 0; i < COUNT(bad); ++i) {
        assert(!ce_csl_inline_slice(source, bad[i][0], bad[i][1], NULL, &out, error, sizeof(error)));
        assert(out == old);
        text_is(out, "\u0301");
    }
    assert(ce_csl_inline_slice(source, 12, 0, NULL, &out, error, sizeof(error)));
    text_is(out, "");
    assert(!ce_csl_inline_run_count(out));
    assert(ce_csl_inline_slice(source, 4, 4, NULL, &source, error, sizeof(error)));
    text_is(source, "🙂");
    assert(ce_csl_inline_run_count(source) == 1 && format_at(source, 0) == CE_CSL_ITALIC);
    ce_csl_inline_free(source);
    assert(ce_csl_inline_slice(out, 0, 0, NULL, &out, error, sizeof(error)));
    text_is(out, "");
    ce_csl_inline_free(out);
}
static void test_strip_periods(void) {
    CeCslInline *source = parse("A.<span class='nocase'>b.</span><i>中.🙂</i>.\u0301", 0), *out = NULL;
    CeCslInlineMap map = {0};
    assert(ce_csl_inline_strip_periods(source, NULL, &out, &map, error, sizeof(error)));
    text_is(source, "A.b.中.🙂.\u0301");
    text_is(out, "Ab中🙂\u0301");
    assert(map.count == 9 && map.source_size == 15 && map.output_size == 11);
    assert(map.spans[1].source_offset == 1 && map.spans[1].output_offset == 1 &&
           map.spans[1].output_length == 0);
    assert(map.spans[4].source_offset == 4 && map.spans[4].source_length == 3 &&
           map.spans[4].output_offset == 2 && map.spans[4].output_length == 3);
    assert(map.spans[6].source_offset == 8 && map.spans[6].source_length == 4 &&
           map.spans[6].output_offset == 5 && map.spans[6].output_length == 4);
    assert(map.spans[7].source_offset == 12 && map.spans[7].output_offset == 9 &&
           map.spans[7].output_length == 0);
    assert(protected_at(out, 1) && !protected_at(out, 0) && format_at(out, 2) == CE_CSL_ITALIC);
    size_t start = 777, length = 888;
    assert(ce_csl_inline_map_range(&map, 1, 3, &start, &length) && start == 1 && length == 1);
    assert(ce_csl_inline_map_range(&map, 7, 1, &start, &length) && start == 5 && length == 0);
    assert(!ce_csl_inline_map_range(&map, 5, 1, &start, &length) && start == 5 && length == 0);
    assert(ce_csl_inline_map_range(&map, 15, 0, &start, &length) && start == 11 && length == 0);
    assert(ce_csl_inline_strip_periods(out, NULL, &out, &map, error, sizeof(error)));
    text_is(out, "Ab中🙂\u0301");
    assert(map.count == 5 && map.source_size == 11 && map.output_size == 11);
    ce_csl_inline_free(source);
    ce_csl_inline_free(out);
    ce_csl_inline_map_free(&map);
    source = parse("<i>A</i><b>.</b><i>B</i>", 0);
    assert(ce_csl_inline_strip_periods(source, NULL, &source, NULL, error, sizeof(error)));
    text_is(source, "AB");
    assert(ce_csl_inline_run_count(source) == 1 && format_at(source, 0) == CE_CSL_ITALIC);
    ce_csl_inline_free(source);
    source = parse("<i>.</i><span class='nocase'>..</span>", 0);
    assert(ce_csl_inline_strip_periods(source, NULL, &source, &map, error, sizeof(error)));
    text_is(source, "");
    assert(!ce_csl_inline_run_count(source) && map.count == 3 && map.output_size == 0);
    for (size_t i = 0; i < 3; ++i)
        assert(map.spans[i].source_offset == i && map.spans[i].source_length == 1 &&
               !map.spans[i].output_offset && !map.spans[i].output_length);
    assert(ce_csl_inline_strip_periods(source, NULL, &source, &map, error, sizeof(error)));
    assert(!map.count && !map.source_size && !map.output_size);
    ce_csl_inline_free(source);
    ce_csl_inline_map_free(&map);
    source = parse(" A。B．C…D&#46; .\t\n ", 0);
    assert(ce_csl_inline_strip_periods(source, NULL, &source, NULL, error, sizeof(error)));
    text_is(source, " A。B．C…D&#46; \t\n ");
    ce_csl_inline_free(source);
}
static CeCslInline *linked_periods(void) {
    CeCslOutput *ir = ce_csl_output_new(NULL, error, sizeof(error));
    assert(ir);
    const char *link = "https://one.example/?x=.";
    assert(ce_csl_output_append(ir, "A.B", 3, CE_CSL_BOLD, link, error, sizeof(error)));
    assert(ce_csl_output_append(ir, ".", 1, CE_CSL_ITALIC, "https://two.example", error, sizeof(error)));
    assert(ce_csl_output_append(ir, "C.D", 3, CE_CSL_BOLD, link, error, sizeof(error)));
    CeCslInline *source = NULL;
    assert(ce_csl_inline_from_output(ir, true, NULL, &source, error, sizeof(error)));
    ce_csl_output_free(ir);
    return source;
}
static void test_slice_strip_links(void) {
    CeCslInline *source = linked_periods(), *slice = NULL, *out = NULL;
    assert(ce_csl_inline_slice(source, 2, 3, NULL, &slice, error, sizeof(error)));
    text_is(slice, "B.C");
    assert(ce_csl_inline_run_count(slice) == 3);
    assert(ce_csl_inline_strip_periods(source, NULL, &out, NULL, error, sizeof(error)));
    ce_csl_inline_free(source);
    text_is(out, "ABCD");
    assert(ce_csl_inline_run_count(out) == 1 && protected_at(out, 0));
    assert(!strcmp(ce_csl_inline_run(out, 0)->href, "https://one.example/?x=."));
    assert(!strcmp(ce_csl_inline_run(slice, 1)->href, "https://two.example"));
    assert(ce_csl_inline_strip_periods(slice, NULL, &slice, NULL, error, sizeof(error)));
    text_is(slice, "BC");
    assert(ce_csl_inline_run_count(slice) == 1 && protected_at(slice, 0));
    ce_csl_inline_free(slice);
    ce_csl_inline_free(out);
}
typedef union Allocation {
    struct {
        size_t size;
    } block;
    long double alignment;
    void *pointer;
} Allocation;
typedef struct Memory {
    size_t calls, fail_at, live, bytes, peak;
} Memory;
static void *allocate(void *ctx, size_t n) {
    Memory *m = ctx;
    if (m->calls++ == m->fail_at)
        return NULL;
    Allocation *a = malloc(sizeof(*a) + n);
    if (!a)
        return NULL;
    a->block.size = n;
    m->bytes += n;
    ++m->live;
    if (m->peak < m->bytes)
        m->peak = m->bytes;
    return a + 1;
}
static void release(void *ctx, void *p) {
    Memory *m = ctx;
    Allocation *a = (Allocation *)p - 1;
    assert(m->live && m->bytes >= a->block.size);
    --m->live;
    m->bytes -= a->block.size;
    free(a);
}
static void test_oom(void) {
    CeCslInline *old = parse("old", 0);
    size_t success = 0;
    for (size_t i = 0; i < 200; ++i) {
        Memory m = {.fail_at = i};
        CeCslOutputAllocator allocator = {&m, allocate, release};
        CeCslInline *out = old;
        CeCslInlineDiagnostic d = {true, true};
        bool ok = ce_csl_inline_parse("a<i>b</i><span class='nocase'>c</span>",
                                      strlen("a<i>b</i><span class='nocase'>c</span>"), 0, &allocator, &out,
                                      &d, error, sizeof(error));
        if (ok) {
            old = NULL;
            m.fail_at = SIZE_MAX;
            text_is(out, "abc");
            assert(!d.literal_markup && !d.malformed_markup);
            ce_csl_inline_free(out);
            assert(!m.live);
            success = i;
            break;
        }
        assert(out == old && d.literal_markup && d.malformed_markup && !m.live);
        text_is(old, "old");
    }
    assert(success >= 3);
    ce_csl_inline_free(old);
    CeCslInline *source = parse("<b>ß</b> I<span class='nocase'>\u0307</span> ΟΣΑ", 0);
    old = parse("old", 0);
    CeCslInlineMap oldmap = {0};
    CeCslInline *map_source = parse("q", 0), *discard = NULL;
    assert(ce_csl_inline_case(map_source, CE_CSL_CASE_UPPER, NULL, NULL, NULL, &discard, &oldmap, error,
                              sizeof(error)));
    ce_csl_inline_free(map_source);
    ce_csl_inline_free(discard);
    success = 0;
    for (size_t i = 0; i < 300; ++i) {
        Memory m = {.fail_at = i};
        CeCslOutputAllocator allocator = {&m, allocate, release};
        CeCslInline *out = old;
        CeCslInlineMap map = oldmap;
        bool ok = ce_csl_inline_case(source, CE_CSL_CASE_LOWER, "tr", NULL, &allocator, &out, &map, error,
                                     sizeof(error));
        if (ok) {
            old = NULL;
            memset(&oldmap, 0, sizeof(oldmap));
            m.fail_at = SIZE_MAX;
            text_is(out, "ß i\u0307 οσα");
            ce_csl_inline_free(out);
            ce_csl_inline_map_free(&map);
            assert(!m.live && m.peak <= CE_CSL_INLINE_MAX_WORKSPACE);
            success = i;
            break;
        }
        assert(out == old && map.spans == oldmap.spans && map.count == oldmap.count && !m.live);
        text_is(old, "old");
    }
    assert(success > 0);
    ce_csl_inline_free(old);
    ce_csl_inline_map_free(&oldmap);
    ce_csl_inline_free(source);
    for (size_t point = 0; point < 30; ++point) {
        Memory m = {.fail_at = SIZE_MAX};
        CeCslOutputAllocator allocator = {&m, allocate, release};
        CeCslInline *a = NULL, *b = parse("<b>second</b><i>third</i>", 0);
        assert(ce_csl_inline_parse("first", 5, 0, &allocator, &a, NULL, error, sizeof(error)));
        size_t before = m.live;
        m.calls = 0;
        m.fail_at = point;
        bool ok = ce_csl_inline_concat(a, b, error, sizeof(error));
        m.fail_at = SIZE_MAX;
        if (!ok) {
            text_is(a, "first");
            assert(m.live == before);
        } else
            text_is(a, "firstsecondthird");
        ce_csl_inline_free(a);
        ce_csl_inline_free(b);
        assert(!m.live);
        if (ok)
            break;
        assert(point < 29);
    }
}
static void test_link_oom(void) {
    CeCslOutput *ir = ce_csl_output_new(NULL, error, sizeof(error));
    assert(ir);
    assert(ce_csl_output_append(ir, "one", 3, CE_CSL_BOLD, "https://one.example", error, sizeof(error)));
    assert(ce_csl_output_append(ir, "two", 3, CE_CSL_ITALIC, "https://two.example", error, sizeof(error)));
    CeCslInline *source = NULL;
    assert(ce_csl_inline_from_output(ir, true, NULL, &source, error, sizeof(error)));
    ce_csl_output_free(ir);
    bool succeeded = false;
    for (size_t point = 0; point < 20; ++point) {
        Memory m = {.fail_at = SIZE_MAX};
        CeCslOutputAllocator a = {&m, allocate, release};
        CeCslInline *destination = NULL;
        assert(ce_csl_inline_parse("old", 3, 0, &a, &destination, NULL, error, sizeof(error)));
        size_t live = m.live;
        m.calls = 0;
        m.fail_at = point;
        bool ok = ce_csl_inline_concat(destination, source, error, sizeof(error));
        m.fail_at = SIZE_MAX;
        if (ok) {
            text_is(destination, "oldonetwo");
            assert(ce_csl_inline_run(destination, 1)->href);
            succeeded = true;
        } else {
            text_is(destination, "old");
            assert(m.live == live);
        }
        ce_csl_inline_free(destination);
        assert(!m.live);
        if (ok)
            break;
    }
    assert(succeeded);
    CeCslInline *old = parse("old", 0);
    succeeded = false;
    for (size_t point = 0; point < 30; ++point) {
        Memory m = {.fail_at = point};
        CeCslOutputAllocator a = {&m, allocate, release};
        CeCslInline *out = old;
        bool ok = ce_csl_inline_clone(source, &a, &out, error, sizeof(error));
        if (ok) {
            old = NULL;
            m.fail_at = SIZE_MAX;
            text_is(out, "onetwo");
            ce_csl_inline_free(out);
            succeeded = true;
        } else {
            assert(out == old);
            text_is(old, "old");
        }
        assert(!m.live);
        if (ok)
            break;
    }
    assert(succeeded);
    ce_csl_inline_free(old);
    succeeded = false;
    for (size_t point = 0; point < 30; ++point) {
        Memory m = {.fail_at = SIZE_MAX};
        CeCslOutputAllocator a = {&m, allocate, release};
        CeCslOutput *dest = ce_csl_output_new(&a, error, sizeof(error));
        assert(dest && ce_csl_output_append(dest, "old", 3, 0, NULL, error, sizeof(error)));
        size_t live = m.live;
        m.calls = 0;
        m.fail_at = point;
        bool ok = ce_csl_inline_to_output(source, dest, error, sizeof(error));
        m.fail_at = SIZE_MAX;
        CeCslOutputString plain = {0};
        assert(ce_csl_output_plain(dest, &plain, error, sizeof(error)));
        assert(!strcmp(plain.data, ok ? "oldonetwo" : "old"));
        ce_csl_output_string_free(&plain);
        if (!ok)
            assert(m.live == live);
        ce_csl_output_free(dest);
        assert(!m.live);
        if (ok) {
            succeeded = true;
            break;
        }
    }
    assert(succeeded);
    ce_csl_inline_free(source);
}
static void test_slice_strip_oom(void) {
    CeCslInline *source = linked_periods();
    for (size_t operation = 0; operation < 2; ++operation) {
        CeCslInline *old = parse("old", 0), *seed = parse("q.", 0), *discard = NULL;
        CeCslInlineMap oldmap = {0};
        assert(ce_csl_inline_strip_periods(seed, NULL, &discard, &oldmap, error, sizeof(error)));
        ce_csl_inline_free(seed);
        ce_csl_inline_free(discard);
        bool succeeded = false;
        for (size_t point = 0; point < 40; ++point) {
            Memory m = {.fail_at = point};
            CeCslOutputAllocator a = {&m, allocate, release};
            CeCslInline *out = old;
            CeCslInlineMap map = oldmap;
            bool ok = operation ? ce_csl_inline_strip_periods(source, &a, &out, &map, error, sizeof(error))
                                : ce_csl_inline_slice(source, 1, 5, &a, &out, error, sizeof(error));
            if (ok) {
                old = NULL;
                if (operation)
                    memset(&oldmap, 0, sizeof(oldmap));
                m.fail_at = SIZE_MAX;
                text_is(out, operation ? "ABCD" : ".B.C.");
                assert(ce_csl_inline_run(out, 0)->href && protected_at(out, 0));
                ce_csl_inline_free(out);
                if (operation) {
                    assert(map.count == 7 && map.spans[1].output_length == 0);
                    ce_csl_inline_map_free(&map);
                }
                succeeded = true;
            } else {
                assert(out == old && map.spans == oldmap.spans && map.count == 2);
                assert(map.source_size == 2 && map.output_size == 1 && map.spans[1].output_length == 0);
                text_is(old, "old");
            }
            assert(!m.live && m.peak <= CE_CSL_INLINE_MAX_WORKSPACE);
            text_is(source, "A.B.C.D");
            if (ok)
                break;
        }
        assert(succeeded);
        ce_csl_inline_free(old);
        ce_csl_inline_map_free(&oldmap);
    }
    ce_csl_inline_free(source);
}
static void test_limits(void) {
    CeCslInline *out = parse("old", 0), *saved = out;
    CeCslInlineDiagnostic diagnostic = {true, true};
    const char bad[] = {(char)0xff};
    assert(!ce_csl_inline_parse(bad, 1, 0, NULL, &out, &diagnostic, error, sizeof(error)) && out == saved);
    assert(!ce_csl_inline_parse("a\0b", 3, 0, NULL, &out, &diagnostic, error, sizeof(error)) && out == saved);
    assert(!ce_csl_inline_parse("x", 1, CE_CSL_ITALIC | CE_CSL_OBLIQUE, NULL, &out, NULL, error,
                                sizeof(error)) &&
           out == saved);
    char nesting[1024];
    size_t n = 0;
    for (size_t i = 0; i < 65; ++i) {
        memcpy(nesting + n, "<i>", 3);
        n += 3;
    }
    for (size_t i = 0; i < 65; ++i) {
        memcpy(nesting + n, "</i>", 4);
        n += 4;
    }
    assert(!ce_csl_inline_parse(nesting, n, 0, NULL, &out, NULL, error, sizeof(error)) && out == saved);
    size_t bytes = (CE_CSL_INLINE_MAX_FRAGMENTS / 2 + 1) * 7;
    char *many = malloc(bytes);
    assert(many);
    for (size_t i = 0; i < bytes; i += 7)
        memcpy(many + i, "<i></i>", 7);
    assert(!ce_csl_inline_parse(many, bytes, 0, NULL, &out, NULL, error, sizeof(error)) && out == saved);
    free(many);
    char *huge = malloc(CE_CSL_INLINE_MAX_BYTES + 1);
    assert(huge);
    memset(huge, 'A', CE_CSL_INLINE_MAX_BYTES + 1);
    assert(
        !ce_csl_inline_parse(huge, CE_CSL_INLINE_MAX_BYTES + 1, 0, NULL, &out, NULL, error, sizeof(error)) &&
        out == saved);
    CeCslInline *large = NULL;
    assert(ce_csl_inline_parse(huge, CE_CSL_INLINE_MAX_BYTES, 0, NULL, &large, NULL, error, sizeof(error)));
    assert(!ce_csl_inline_concat(out, large, error, sizeof(error)) && out == saved);
    text_is(out, "old");
    Memory m = {.fail_at = SIZE_MAX};
    CeCslOutputAllocator a = {&m, allocate, release};
    CeCslInline *changed = NULL;
    CeCslInlineMap map = {0};
    assert(
        ce_csl_inline_case(large, CE_CSL_CASE_LOWER, NULL, NULL, &a, &changed, &map, error, sizeof(error)));
    assert(map.count == CE_CSL_INLINE_MAX_BYTES && m.peak <= CE_CSL_INLINE_MAX_WORKSPACE);
    ce_csl_inline_free(changed);
    ce_csl_inline_map_free(&map);
    assert(!m.live);
    m.peak = 0;
    memset(huge, '.', CE_CSL_INLINE_MAX_BYTES);
    CeCslInline *periods = NULL, *stripped = NULL;
    assert(ce_csl_inline_parse(huge, CE_CSL_INLINE_MAX_BYTES, 0, NULL, &periods, NULL, error, sizeof(error)));
    assert(ce_csl_inline_strip_periods(periods, &a, &stripped, &map, error, sizeof(error)));
    text_is(stripped, "");
    assert(map.count == CE_CSL_INLINE_MAX_BYTES && map.source_size == CE_CSL_INLINE_MAX_BYTES);
    assert(!map.output_size && !map.spans[map.count - 1].output_length);
    assert(m.peak <= CE_CSL_INLINE_MAX_WORKSPACE);
    ce_csl_inline_map_free(&map);
    ce_csl_inline_free(stripped);
    ce_csl_inline_free(periods);
    assert(!m.live);
    ce_csl_inline_free(large);
    free(huge);
    ce_csl_inline_free(out);
    out = parse("<i>x</i>y", 0);
    while (ce_csl_inline_run_count(out) < CE_CSL_INLINE_MAX_RUNS)
        assert(ce_csl_inline_concat(out, out, error, sizeof(error)));
    assert(ce_csl_inline_run_count(out) == CE_CSL_INLINE_MAX_RUNS);
    size_t saved_size;
    ce_csl_inline_text(out, &saved_size);
    assert(!ce_csl_inline_concat(out, out, error, sizeof(error)));
    size_t new_size;
    ce_csl_inline_text(out, &new_size);
    assert(saved_size == new_size && ce_csl_inline_run_count(out) == CE_CSL_INLINE_MAX_RUNS);
    /* A finalizer can visit many distant ranges without rescanning preceding
     * runs. Each scalar slice independently locates its run by binary search. */
    CeCslInline *part = NULL;
    for (size_t i = saved_size; i > 0; --i) {
        assert(ce_csl_inline_slice(out, i - 1, 1, NULL, &part, error, sizeof(error)));
        text_is(part, (i - 1) % 2 ? "y" : "x");
        assert(format_at(part, 0) == ((i - 1) % 2 ? 0 : CE_CSL_ITALIC));
    }
    ce_csl_inline_free(part);
    ce_csl_inline_free(out);
}
int main(void) {
    test_parse();
    test_case_and_maps();
    test_concat_output();
    test_slice();
    test_strip_periods();
    test_slice_strip_links();
    test_oom();
    test_link_oom();
    test_slice_strip_oom();
    test_limits();
    puts("CSL inline: whitelist/flip, literal fallback, whole-case/nocase maps, links, concat, slice, "
         "strip-periods, bounds and atomic OOM passed");
    return 0;
}
