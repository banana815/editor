#include "csl_output.h"
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char error[256];
#define OK(call) do { bool success = (call); if (!success) fprintf(stderr, "%s: %s\n", #call, error); assert(success); } while (0)
static CeCslOutput *create(void) {
    CeCslOutput *output = ce_csl_output_new(NULL, error, sizeof error);
    assert(output && !*error);
    return output;
}
static void append(CeCslOutput *output, const char *text, unsigned format, const char *href) {
    OK(ce_csl_output_append(output, text, strlen(text), format, href, error, sizeof error));
}
static void expect_text(const CeCslOutput *output, const char *expected) {
    CeCslOutputString text = {0};
    OK(ce_csl_output_plain(output, &text, error, sizeof error));
    assert(text.data && text.length == strlen(expected) && !strcmp(text.data, expected));
    assert(ce_csl_output_text_bytes(output) == text.length);
    ce_csl_output_string_free(&text);
    assert(!text.data && !text.length);
}
static void test_text_and_html(void) {
    CeCslOutput *output = create();
    CeCslOutputString html = {0}, empty = {0};
    OK(ce_csl_output_plain(output, &empty, error, sizeof error));
    assert(empty.data && !empty.length && !*empty.data);
    OK(ce_csl_output_html(output, &html, error, sizeof error));
    assert(!strcmp(html.data, "<span style=\"white-space:pre-wrap\"></span>"));
    OK(ce_csl_output_append(output, NULL, 0, 0, NULL, error, sizeof error));
    assert(!ce_csl_output_run_count(output));

    char text[] = "  中文 e\xcc\x81 👩‍💻 &<>\"'\t\r\n ";
    char link[] = "HTTPS://example.org:443/中文?x=\"'&y=%26";
    unsigned formats = CE_CSL_BOLD | CE_CSL_ITALIC | CE_CSL_UNDERLINE | CE_CSL_SMALL_CAPS | CE_CSL_SUPERSCRIPT;
    append(output, text, formats, link);
    const CeCslOutputRun *run = ce_csl_output_run(output, 0);
    assert(run && run->format == formats && run->byte_length == strlen(text));
    assert(run->text != text && run->href != link && !strcmp(run->href, link));
    text[0] = '!'; link[0] = 'x';
    assert(run->text[0] == ' ' && run->href[0] == 'H');
    append(output, "2", CE_CSL_SUBSCRIPT, "");
    assert(!ce_csl_output_run(output, 1)->href && !ce_csl_output_run(output, 2));
    expect_text(output, "  中文 e\xcc\x81 👩‍💻 &<>\"'\t\r\n 2");
    OK(ce_csl_output_html(output, &html, error, sizeof error));
    const char *expected = "<span style=\"white-space:pre-wrap\">"
        "<a href=\"HTTPS://example.org:443/中文?x=&quot;&#39;&amp;y=%26\" rel=\"noopener noreferrer\">"
        "<strong><em><u><span style=\"font-variant:small-caps\"><sup>"
        "  中文 e\xcc\x81 👩‍💻 &amp;&lt;&gt;&quot;&#39;\t&#13;\n "
        "</sup></span></u></em></strong></a><sub>2</sub></span>";
    assert(html.length == strlen(expected) && !strcmp(html.data, expected));
    CeCslOutput *copy = ce_csl_output_clone(output, error, sizeof error);
    assert(copy && ce_csl_output_run_count(copy) == 2);
    assert(ce_csl_output_run(copy, 0)->text != run->text);
    assert(ce_csl_output_run(copy, 0)->href != run->href);
    append(output, " changed", 0, NULL);
    ce_csl_output_free(output);
    expect_text(copy, "  中文 e\xcc\x81 👩‍💻 &<>\"'\t\r\n 2");
    ce_csl_output_free(copy);
    /* Exported storage owns its data even after its originating IR is gone. */
    assert(!strcmp(html.data, expected));
    ce_csl_output_string_free(&html);
    ce_csl_output_string_free(&empty);
}

static void test_rejections(void) {
    CeCslOutput *output = create();
    append(output, "unchanged", CE_CSL_BOLD, NULL);
    const char *original = ce_csl_output_run(output, 0)->text;
    const struct { const char *text; size_t size; } bad[] = {
        {"\xc0\xaf", 2}, {"\x80", 1}, {"\xed\xa0\x80", 3},
        {"\xf4\x90\x80\x80", 4}, {"\xe4\xb8", 2}, {"x\0y", 3},
        {"\x01", 1}, {"\xff", 1}, {"\xf0\x28\x8c\x28", 4}
    };
    for (size_t i = 0; i < sizeof bad / sizeof *bad; ++i) {
        assert(!ce_csl_output_append(output, bad[i].text, bad[i].size, 0, NULL, error, sizeof error));
        assert(*error && ce_csl_output_run_count(output) == 1 && ce_csl_output_run(output, 0)->text == original);
    }
    assert(!ce_csl_output_append(output, "x", 1, CE_CSL_SUPERSCRIPT | CE_CSL_SUBSCRIPT, NULL, error, sizeof error));
    assert(!ce_csl_output_append(output, "x", 1, 1u << 12, NULL, error, sizeof error));
    assert(!ce_csl_output_append(output, NULL, 1, 0, NULL, error, sizeof error));
    assert(!ce_csl_output_append(output, "x", CE_CSL_OUTPUT_MAX_BYTES + 1, 0, NULL, error, sizeof error));
    assert(!ce_csl_output_concat(output, NULL, NULL, 0));
    assert(!ce_csl_output_clone(NULL, error, sizeof error));
    assert(!ce_csl_output_run(NULL, 0) && !ce_csl_output_run_count(NULL) && !ce_csl_output_text_bytes(NULL));
    char tiny[] = {'x', 'y'};
    assert(!ce_csl_output_append(NULL, "x", 1, 0, NULL, tiny, 1));
    assert(tiny[0] == 0 && tiny[1] == 'y');
    expect_text(output, "unchanged");
    ce_csl_output_free(output);
    ce_csl_output_free(NULL);
    ce_csl_output_string_free(NULL);
}

static void test_links(void) {
    CeCslOutput *output = create();
    const char *good[] = {"http://example.org", "https://example.org/a?b=1&c=2#ref",
        "HTTPS://LOCALHOST:65535/x", "http://127.0.0.1:80/", "https://example.org/中文?q=%E4%B8%AD",
        "https://example.org/'\"<>&"};
    for (size_t i = 0; i < sizeof good / sizeof *good; ++i) append(output, "link", 0, good[i]);
    size_t count = ce_csl_output_run_count(output);
    const char *bad[] = {"javascript:alert(1)", "data:text/html,<script>x</script>", "file:///tmp/a",
        "//example.org/a", "https:/example.org", "http://", "https:///a", "https://user:pass@example.org/",
        "https://example.org@evil.org", "https://[::1]/", "https://host:0/", "https://host:65536/",
        "https://host:/", "https://host:abc/", "https://.host/", "https://host../", "https://-host/",
        "https://host-/", "https://host/a b", "https://host/\t", "https://host/\\evil", "https://host/%",
        "https://host/%0", "https://host/%xz", "https://host/%00", "https://host/%0a", "https://host/%20",
        "https://host/%7f", "https://host/%5c", "https://host/\xc2\xa0", "https://host/\xe2\x80\xae",
        "https://host/\xc0\xaf", " https://host/"};
    for (size_t i = 0; i < sizeof bad / sizeof *bad; ++i) {
        assert(!ce_csl_output_append(output, "bad", 3, 0, bad[i], error, sizeof error));
        assert(*error && ce_csl_output_run_count(output) == count);
    }
    char *long_link = malloc(CE_CSL_OUTPUT_MAX_LINK_BYTES + 2);
    assert(long_link);
    memset(long_link, 'x', CE_CSL_OUTPUT_MAX_LINK_BYTES + 1);
    memcpy(long_link, "https://host/", 13);
    long_link[CE_CSL_OUTPUT_MAX_LINK_BYTES + 1] = 0;
    assert(!ce_csl_output_append(output, "bad", 3, 0, long_link, error, sizeof error));
    long_link[CE_CSL_OUTPUT_MAX_LINK_BYTES] = 0;
    append(output, "max link", 0, long_link);
    free(long_link);
    ce_csl_output_free(output);
}

static void test_concat_and_limits(void) {
    CeCslOutput *output = create(), *source = create();
    append(output, "A ", CE_CSL_BOLD, NULL);
    append(source, "中文", CE_CSL_ITALIC, "https://example.org");
    append(source, " B", 0, NULL);
    const char *old_payload = ce_csl_output_run(output, 0)->text;
    OK(ce_csl_output_concat(output, source, error, sizeof error));
    assert(ce_csl_output_run(output, 0)->text == old_payload);
    assert(ce_csl_output_run(output, 1)->text != ce_csl_output_run(source, 0)->text);
    ce_csl_output_free(source);
    OK(ce_csl_output_concat(output, output, error, sizeof error));
    expect_text(output, "A 中文 BA 中文 B");
    assert(ce_csl_output_run(output, 0)->text == old_payload);
    assert(ce_csl_output_run(output, 3)->text != old_payload);
    ce_csl_output_free(output);

    char *large = malloc(CE_CSL_OUTPUT_MAX_BYTES + 1);
    assert(large);
    memset(large, 'x', CE_CSL_OUTPUT_MAX_BYTES);
    large[CE_CSL_OUTPUT_MAX_BYTES] = 0;
    output = create();
    OK(ce_csl_output_append(output, large, CE_CSL_OUTPUT_MAX_BYTES, 0, NULL, error, sizeof error));
    assert(!ce_csl_output_append(output, "x", 1, 0, NULL, error, sizeof error));
    assert(!ce_csl_output_concat(output, output, error, sizeof error));
    expect_text(output, large);
    CeCslOutput *copy = ce_csl_output_clone(output, error, sizeof error);
    assert(copy);
    expect_text(copy, large);
    ce_csl_output_free(copy);
    ce_csl_output_free(output);
    output = create();
    const char *link = "https://host";
    size_t text_bytes = CE_CSL_OUTPUT_MAX_BYTES - strlen(link);
    OK(ce_csl_output_append(output, large, text_bytes, 0, link, error, sizeof error));
    assert(!ce_csl_output_append(output, "x", 1, 0, NULL, error, sizeof error));
    assert(ce_csl_output_text_bytes(output) == text_bytes);
    ce_csl_output_free(output);
    free(large);

    output = create();
    for (size_t i = 0; i < CE_CSL_OUTPUT_MAX_RUNS; ++i) append(output, "x", CE_CSL_SMALL_CAPS, NULL);
    assert(!ce_csl_output_append(output, "x", 1, 0, NULL, error, sizeof error));
    assert(ce_csl_output_run_count(output) == CE_CSL_OUTPUT_MAX_RUNS);
    assert(ce_csl_output_text_bytes(output) == CE_CSL_OUTPUT_MAX_RUNS);
    ce_csl_output_free(output);
}

/* Per-instance injection covers every allocation without global hooks. A
 * prefixed allocation deliberately cannot be passed to plain free(payload). */
typedef union TestAllocation {
    struct { size_t size, magic; } header;
    long double alignment;
    void *pointer;
} TestAllocation;
typedef struct Memory {
    size_t calls, fail_at, live, bytes;
} Memory;
static void *test_allocate(void *context, size_t size) {
    Memory *memory = context;
    if (++memory->calls == memory->fail_at) return NULL;
    assert(size <= SIZE_MAX - sizeof(TestAllocation));
    TestAllocation *allocation = malloc(sizeof *allocation + size);
    assert(allocation);
    allocation->header.size = size;
    allocation->header.magic = 0xc510u;
    ++memory->live;
    memory->bytes += size;
    return allocation + 1;
}
static void test_deallocate(void *context, void *pointer) {
    Memory *memory = context;
    TestAllocation *allocation = (TestAllocation *)pointer - 1;
    assert(pointer && allocation->header.magic == 0xc510u && memory->live);
    assert(memory->bytes >= allocation->header.size);
    --memory->live;
    memory->bytes -= allocation->header.size;
    allocation->header.magic = 0;
    free(allocation);
}
static CeCslOutput *tracked(Memory *memory) {
    CeCslOutputAllocator allocator = {memory, test_allocate, test_deallocate};
    CeCslOutput *output = ce_csl_output_new(&allocator, error, sizeof error);
    assert(output);
    return output;
}
static void test_oom_append(void) {
    for (size_t point = 1; point < 12; ++point) {
        Memory memory = {0};
        CeCslOutput *output = tracked(&memory);
        for (size_t i = 0; i < 8; ++i) append(output, "x", 0, NULL);
        size_t live = memory.live, bytes = memory.bytes;
        const char *first = ce_csl_output_run(output, 0)->text;
        memory.fail_at = memory.calls + point;
        bool success = ce_csl_output_append(output, " 中文 ", strlen(" 中文 "), CE_CSL_BOLD,
                                            "https://example.org", error, sizeof error);
        if (!success) {
            assert(*error && memory.live == live && memory.bytes == bytes);
            assert(ce_csl_output_run_count(output) == 8 && ce_csl_output_text_bytes(output) == 8);
        } else assert(ce_csl_output_run_count(output) == 9);
        assert(ce_csl_output_run(output, 0)->text == first);
        ce_csl_output_free(output);
        assert(!memory.live && !memory.bytes);
        if (success) { assert(point == 4); break; }
    }
}
static void test_oom_concat(bool grow, bool self) {
    bool reached_success = false;
    for (size_t point = 1; point < 40; ++point) {
        Memory memory = {0};
        CeCslOutput *output = tracked(&memory), *source = self ? output : create();
        size_t initial = grow ? 8 : 2;
        for (size_t i = 0; i < initial; ++i) append(output, "x", CE_CSL_BOLD, "https://one.org");
        if (!self) for (size_t i = 0; i < 3; ++i) append(source, "中文", CE_CSL_ITALIC, "https://two.org");
        size_t live = memory.live, bytes = memory.bytes, source_count = ce_csl_output_run_count(source);
        const char *first = ce_csl_output_run(output, 0)->text;
        memory.fail_at = memory.calls + point;
        bool success = ce_csl_output_concat(output, source, error, sizeof error);
        if (!success) {
            assert(*error && memory.live == live && memory.bytes == bytes);
            assert(ce_csl_output_run_count(output) == initial && ce_csl_output_text_bytes(output) == initial);
            memory.fail_at = 0;
            /* A failed attempt leaves spare capacity reusable, with no double free. */
            append(output, "retry", 0, NULL);
            assert(ce_csl_output_run_count(output) == initial + 1);
        } else {
            assert(ce_csl_output_run_count(output) == initial + source_count);
            reached_success = true;
        }
        assert(ce_csl_output_run(output, 0)->text == first);
        if (!self) ce_csl_output_free(source);
        ce_csl_output_free(output);
        assert(!memory.live && !memory.bytes);
        if (success) break;
    }
    assert(reached_success);
}
static void test_oom_clone_and_exports(void) {
    Memory memory = {0};
    CeCslOutputAllocator allocator = {&memory, test_allocate, test_deallocate};
    memory.fail_at = 1;
    assert(!ce_csl_output_new(&allocator, error, sizeof error) && *error && !memory.live);
    memory.fail_at = 0;
    CeCslOutput *source = tracked(&memory);
    append(source, " 中文 ", CE_CSL_ITALIC, "https://example.org");
    append(source, " B ", 0, NULL);
    size_t live = memory.live, bytes = memory.bytes;
    bool reached_success = false;
    for (size_t point = 1; point < 20; ++point) {
        memory.fail_at = memory.calls + point;
        CeCslOutput *copy = ce_csl_output_clone(source, error, sizeof error);
        if (copy) { ce_csl_output_free(copy); reached_success = true; }
        else assert(*error);
        assert(memory.live == live && memory.bytes == bytes);
        if (reached_success) break;
    }
    assert(reached_success);
    memory.fail_at = 0;
    CeCslOutput *other = create();
    append(other, "old result", 0, NULL);
    CeCslOutputString result = {0};
    OK(ce_csl_output_plain(other, &result, error, sizeof error));
    char *old = result.data;
    memory.fail_at = memory.calls + 1;
    assert(!ce_csl_output_plain(source, &result, error, sizeof error) && *error);
    assert(result.data == old && !strcmp(result.data, "old result"));
    assert(memory.live == live && memory.bytes == bytes);
    memory.fail_at = memory.calls + 1;
    assert(!ce_csl_output_html(source, &result, error, sizeof error) && *error);
    assert(result.data == old && !strcmp(result.data, "old result"));
    assert(memory.live == live && memory.bytes == bytes);
    memory.fail_at = 0;
    OK(ce_csl_output_html(source, &result, error, sizeof error));
    assert(memory.live == live + 1 && strstr(result.data, "中文"));
    ce_csl_output_free(other);
    ce_csl_output_free(source);
    assert(memory.live == 1 && strstr(result.data, "中文"));
    ce_csl_output_string_free(&result);
    assert(!memory.live && !memory.bytes);
    allocator.allocate = NULL;
    assert(!ce_csl_output_new(&allocator, error, sizeof error) && *error);
}

int main(void) {
    test_text_and_html();
    test_rejections();
    test_links();
    test_concat_and_limits();
    test_oom_append();
    test_oom_concat(false, false);
    test_oom_concat(true, false);
    test_oom_concat(false, true);
    test_oom_concat(true, true);
    test_oom_clone_and_exports();
    puts("C CSL owned runs, exact UTF-8/whitespace, safe HTML/links, limits and atomic OOM passed");
    return 0;
}
