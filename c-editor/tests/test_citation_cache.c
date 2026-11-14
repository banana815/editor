#include "citation_cache.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define HASH64 "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef"
#define HASH40 "0123456789abcdef0123456789abcdef01234567"
#define HEADER                                                                                               \
    "\"version\":1,\"processorProfile\":\"notelab-csl/1\",\"sourceDigest\":\"" HASH64                        \
    "\",\"style\":{\"profile\":\"apa\",\"id\":\"http://www.zotero.org/styles/apa\",\"sha256\":\"" HASH64     \
    "\",\"commit\":\"" HASH40 "\",\"locale\":\"en-us\",\"localeCommit\":\"" HASH40 "\"}"
static char error[512];
static CeCitationCacheHeader header = {CE_CITATION_PROCESSOR_PROFILE,
                                       "apa",
                                       "http://www.zotero.org/styles/apa",
                                       HASH64,
                                       HASH40,
                                       "en-us",
                                       HASH40,
                                       HASH64};
static const char *citation_json = "{" HEADER ",\"kind\":\"citation\",\"citationId\":\"c1\",\"runs\":["
                                   "{\"text\":\"中文 <script>literal</script>\",\"format\":1},{\"text\":\" "
                                   "linked\",\"format\":6,\"href\":\"https://example.invalid/a?x=1&y=2\"}]}";
static const char *bib_json = "{" HEADER ",\"kind\":\"bibliography\",\"options\":{\"alignment\":\"flush\","
                              "\"hangingIndent\":false,\"lineSpacing\":2,\"entrySpacing\":0},\"entries\":[{"
                              "\"sourceItemId\":\"source:a\","
                              "\"number\":1,\"visible\":true,\"rows\":[{\"kind\":\"two-columns\",\"left\":[{"
                              "\"text\":\"[1]\",\"format\":0}],"
                              "\"body\":[{\"text\":\"标题\",\"format\":8}]},{\"kind\":\"block\",\"body\":[{"
                              "\"text\":\"Block\",\"format\":64}]},"
                              "{\"kind\":\"indent\",\"body\":[{\"text\":\"Indented\",\"format\":128}]},{"
                              "\"kind\":\"flow\",\"body\":[{\"text\":\"Flow\",\"format\":16}]}]},"
                              "{\"sourceItemId\":\"source:b\",\"number\":2,\"visible\":false,\"rows\":[]}]}";
static void plain_is(const CeCslOutput *output, const char *expected) {
    CeCslOutputString text = {0};
    assert(ce_csl_output_plain(output, &text, error, sizeof error));
    assert(!strcmp(text.data, expected));
    ce_csl_output_string_free(&text);
}
static void test_roundtrip(void) {
    CeCitationCache *cache = NULL, *decoded = NULL;
    CeCslOutputString json = {0};
    assert(ce_citation_cache_decode(citation_json, strlen(citation_json), NULL, &cache, error, sizeof error));
    assert(ce_citation_cache_kind(cache) == CE_CITATION_CACHE_CITATION &&
           !strcmp(ce_citation_cache_citation_id(cache), "c1"));
    assert(!strcmp(ce_citation_cache_header(cache)->source_digest, HASH64));
    const CeCslOutput *output = ce_citation_cache_citation_output(cache);
    plain_is(output, "中文 <script>literal</script> linked");
    assert(ce_csl_output_run(output, 0)->format == CE_CSL_BOLD);
    assert(!strcmp(ce_csl_output_run(output, 1)->href, "https://example.invalid/a?x=1&y=2"));
    assert(!ce_citation_cache_bibliography_options(cache) && !ce_citation_cache_entry(cache, 0));
    assert(ce_citation_cache_encode(cache, &json, error, sizeof error));
    assert(ce_citation_cache_decode(json.data, json.length, NULL, &decoded, error, sizeof error));
    ce_citation_cache_free(cache);
    cache = NULL;
    ce_csl_output_string_free(&json);
    plain_is(ce_citation_cache_citation_output(decoded), "中文 <script>literal</script> linked");
    ce_citation_cache_free(decoded);
    decoded = NULL;
    assert(ce_citation_cache_decode(bib_json, strlen(bib_json), NULL, &cache, error, sizeof error));
    const CeCslBibliographyOptions *options = ce_citation_cache_bibliography_options(cache);
    assert(options && options->alignment == CE_CSL_BIBLIOGRAPHY_ALIGN_FLUSH && options->line_spacing == 2 &&
           !options->entry_spacing);
    assert(ce_citation_cache_entry_count(cache) == 2 && !ce_citation_cache_citation_output(cache));
    const CeCitationCacheEntry *entry = ce_citation_cache_entry(cache, 0);
    assert(entry && entry->row_count == 4 && entry->visible);
    assert(entry->rows[0].kind == CE_CSL_BIBLIOGRAPHY_TWO_COLUMNS);
    plain_is(entry->rows[0].left, "[1]");
    plain_is(entry->rows[0].body, "标题");
    assert(ce_csl_output_run(entry->rows[0].body, 0)->format == CE_CSL_SMALL_CAPS);
    assert(entry->rows[1].kind == CE_CSL_BIBLIOGRAPHY_BLOCK &&
           entry->rows[2].kind == CE_CSL_BIBLIOGRAPHY_INDENT);
    assert(entry->rows[3].kind == CE_CSL_BIBLIOGRAPHY_FLOW && !entry->rows[3].left);
    assert(!ce_citation_cache_entry(cache, 1)->visible && !ce_citation_cache_entry(cache, 1)->row_count);
    assert(ce_citation_cache_encode(cache, &json, error, sizeof error));
    assert(ce_citation_cache_decode(json.data, json.length, NULL, &decoded, error, sizeof error));
    ce_citation_cache_free(cache);
    cache = NULL;
    assert(ce_citation_cache_entry(decoded, 0)->row_count == 4);
    ce_csl_output_string_free(&json);
    ce_citation_cache_free(decoded);
}
static cJSON *get(cJSON *object, const char *key) {
    return cJSON_GetObjectItemCaseSensitive(object, key);
}
static void replace(cJSON *object, const char *key, cJSON *value) {
    assert(value && cJSON_ReplaceItemInObjectCaseSensitive(object, key, value));
}
static void rejected(CeCitationCache *old, const char *text) {
    CeCitationCache *cache = old;
    const CeCslOutput *view = ce_citation_cache_citation_output(old);
    assert(!ce_citation_cache_decode(text, strlen(text), NULL, &cache, error, sizeof error));
    assert(*error && cache == old && ce_citation_cache_citation_output(cache) == view);
}
static void test_invalid(void) {
    CeCitationCache *cache = NULL;
    assert(ce_citation_cache_decode(citation_json, strlen(citation_json), NULL, &cache, error, sizeof error));
    const char *invalid[] = {
        "null",          "{}",        "{\"x\":1,\"x\":2}", "{\"s\":\"\\u0000\"}", "{\"s\":\"\\ud800\"}",
        "{\"n\":1e999}", "{\"x\":01}"};
    for (size_t i = 0; i < sizeof invalid / sizeof *invalid; ++i)
        rejected(cache, invalid[i]);
    const char *numeric_keys[] = {"\"version\":", "\"format\":"};
    const char *numeric_values[] = {"1.0000000000000001", "1e-9999"};
    for (size_t i = 0; i < 2; ++i) {
        const char *start = strstr(citation_json, numeric_keys[i]);
        assert(start);
        size_t prefix = (size_t)(start - citation_json) + strlen(numeric_keys[i]);
        size_t n = strlen(citation_json), replacement = strlen(numeric_values[i]);
        char *changed = malloc(n + replacement);
        assert(changed);
        memcpy(changed, citation_json, prefix);
        memcpy(changed + prefix, numeric_values[i], replacement);
        memcpy(changed + prefix + replacement, citation_json + prefix + 1, n - prefix);
        rejected(cache, changed);
        free(changed);
    }
    for (size_t i = 0; i < 11; ++i) {
        cJSON *root = cJSON_Parse(citation_json);
        assert(root);
        cJSON *run = cJSON_GetArrayItem(get(root, "runs"), 0);
        if (i == 0)
            replace(root, "version", cJSON_CreateNumber(2));
        if (i == 1)
            replace(root, "processorProfile", cJSON_CreateString("future/9"));
        if (i == 2)
            replace(run, "format", cJSON_CreateNumber(CE_CSL_BOLD | CE_CSL_LIGHT));
        if (i == 3)
            replace(run, "format", cJSON_CreateNumber(CE_CSL_ITALIC | CE_CSL_OBLIQUE));
        if (i == 4)
            replace(run, "format", cJSON_CreateNumber(CE_CSL_SUPERSCRIPT | CE_CSL_SUBSCRIPT));
        if (i == 5)
            replace(run, "format", cJSON_CreateNumber(256));
        if (i == 6)
            replace(run, "format", cJSON_CreateNumber(1.5));
        if (i == 7)
            assert(cJSON_AddStringToObject(run, "href", "javascript:alert(1)"));
        if (i == 8)
            assert(cJSON_AddStringToObject(root, "html", "<script>bad</script>"));
        if (i == 9)
            replace(root, "sourceDigest", cJSON_CreateString("wrong"));
        if (i == 10)
            replace(root, "citationId", cJSON_CreateString(""));
        char *json = cJSON_PrintUnformatted(root);
        assert(json);
        rejected(cache, json);
        cJSON_free(json);
        cJSON_Delete(root);
    }
    for (size_t i = 0; i < 9; ++i) {
        cJSON *root = cJSON_Parse(bib_json);
        assert(root);
        cJSON *entries = get(root, "entries"), *entry = cJSON_GetArrayItem(entries, 0);
        cJSON *row = cJSON_GetArrayItem(get(entry, "rows"), 0), *second = cJSON_GetArrayItem(entries, 1);
        if (i == 0)
            replace(entry, "visible", cJSON_CreateFalse());
        if (i == 1)
            replace(entry, "number", cJSON_CreateNumber(0));
        if (i == 2)
            replace(entry, "number", cJSON_CreateNumber(50001));
        if (i == 3)
            replace(row, "kind", cJSON_CreateString("nested"));
        if (i == 4)
            replace(row, "kind", cJSON_CreateString("flow"));
        if (i == 5)
            cJSON_DeleteItemFromObjectCaseSensitive(row, "left");
        if (i == 6)
            replace(get(root, "options"), "lineSpacing", cJSON_CreateNumber(0));
        if (i == 7)
            replace(second, "sourceItemId", cJSON_CreateString("source:a"));
        if (i == 8)
            replace(second, "number", cJSON_CreateNumber(1));
        char *json = cJSON_PrintUnformatted(root);
        assert(json);
        rejected(cache, json);
        cJSON_free(json);
        cJSON_Delete(root);
    }
    CeCitationCache *old = cache;
    assert(
        !ce_citation_cache_decode("{}", CE_CITATION_CACHE_MAX_BYTES + 1, NULL, &cache, error, sizeof error) &&
        cache == old);
    ce_citation_cache_free(cache);
}
static void test_copy_and_limits(void) {
    CeCslOutput *source = ce_csl_output_new(NULL, error, sizeof error);
    assert(source);
    assert(ce_csl_output_append(source, "ABC", 3, CE_CSL_BOLD | CE_CSL_ITALIC, NULL, error, sizeof error));
    CeCitationCache *cache = NULL;
    assert(ce_citation_cache_from_citation(&header, "c", source, NULL, &cache, error, sizeof error));
    ce_csl_output_free(source);
    plain_is(ce_citation_cache_citation_output(cache), "ABC");
    CeCitationCache *before = cache;
    CeCitationCacheHeader bad = header;
    bad.source_digest = "not-a-hash";
    assert(!ce_citation_cache_from_citation(&bad, "c", ce_citation_cache_citation_output(cache), NULL, &cache,
                                            error, sizeof error) &&
           cache == before);
    assert(ce_citation_cache_from_citation(&header, "self", ce_citation_cache_citation_output(cache), NULL,
                                           &cache, error, sizeof error));
    plain_is(ce_citation_cache_citation_output(cache), "ABC");
    assert(!strcmp(ce_citation_cache_citation_id(cache), "self"));
    CeCslBibliographyOptions options = {CE_CSL_BIBLIOGRAPHY_ALIGN_NONE, true, 2, 0};
    assert(
        ce_citation_cache_from_bibliography(&header, &options, NULL, 0, NULL, &cache, error, sizeof error));
    assert(!ce_citation_cache_entry_count(cache));
    CeCslOutputString json = {0};
    assert(ce_citation_cache_encode(cache, &json, error, sizeof error));
    assert(ce_citation_cache_decode(json.data, json.length, NULL, &cache, error, sizeof error));
    ce_csl_output_string_free(&json);
    ce_citation_cache_free(cache);
}
static void test_text_boundary(void) {
    size_t n = CE_CSL_OUTPUT_MAX_BYTES;
    char *text = malloc(n + 2);
    assert(text);
    memset(text, 'x', n + 1);
    text[n + 1] = 0;
    CeCslOutput *source = ce_csl_output_new(NULL, error, sizeof error);
    assert(source);
    assert(ce_csl_output_append(source, text, n, 0, NULL, error, sizeof error));
    CeCitationCache *cache = NULL;
    assert(ce_citation_cache_from_citation(&header, "boundary", source, NULL, &cache, error, sizeof error));
    ce_csl_output_free(source);
    assert(ce_csl_output_text_bytes(ce_citation_cache_citation_output(cache)) == n);
    CeCslOutputString json = {0};
    assert(ce_citation_cache_encode(cache, &json, error, sizeof error));
    assert(ce_citation_cache_decode(json.data, json.length, NULL, &cache, error, sizeof error));
    cJSON *root = cJSON_Parse(json.data);
    assert(root);
    cJSON *run = cJSON_GetArrayItem(get(root, "runs"), 0);
    replace(run, "text", cJSON_CreateString(text));
    char *too_long = cJSON_PrintUnformatted(root);
    assert(too_long);
    rejected(cache, too_long);
    cJSON_free(too_long);
    cJSON_Delete(root);
    free(text);
    ce_csl_output_string_free(&json);
    ce_citation_cache_free(cache);
}
typedef struct Fault {
    size_t calls, at, live;
} Fault;
static void *fault_allocate(void *context, size_t size) {
    Fault *f = context;
    ++f->calls;
    if (f->at && f->calls >= f->at)
        return NULL;
    void *p = malloc(size);
    if (p)
        ++f->live;
    return p;
}
static void fault_free(void *context, void *p) {
    if (p) {
        Fault *f = context;
        assert(f->live);
        --f->live;
        free(p);
    }
}
static Fault json_fault;
static void *json_allocate(size_t size) {
    return fault_allocate(&json_fault, size);
}
static void json_free(void *memory) {
    fault_free(&json_fault, memory);
}
static void test_oom(void) {
    CeCitationCache *old = NULL;
    assert(ce_citation_cache_decode(citation_json, strlen(citation_json), NULL, &old, error, sizeof error));
    size_t total = 0;
    const char *fixtures[] = {citation_json, bib_json};
    for (size_t f = 0; f < 2; ++f) {
        Fault fault = {0};
        CeCslOutputAllocator allocator = {&fault, fault_allocate, fault_free};
        CeCitationCache *cache = NULL;
        assert(ce_citation_cache_decode(fixtures[f], strlen(fixtures[f]), &allocator, &cache, error,
                                        sizeof error));
        size_t calls = fault.calls;
        ce_citation_cache_free(cache);
        assert(!fault.live);
        for (size_t at = 1; at <= calls; ++at) {
            fault = (Fault){0, at, 0};
            cache = old;
            assert(!ce_citation_cache_decode(fixtures[f], strlen(fixtures[f]), &allocator, &cache, error,
                                             sizeof error));
            assert(cache == old && *error && !fault.live);
        }
        total += calls;
    }
    cJSON_Hooks hooks = {json_allocate, json_free};
    cJSON_InitHooks(&hooks);
    json_fault = (Fault){0};
    CeCitationCache *cache = NULL;
    assert(ce_citation_cache_decode(bib_json, strlen(bib_json), NULL, &cache, error, sizeof error));
    size_t calls = json_fault.calls;
    ce_citation_cache_free(cache);
    assert(!json_fault.live);
    for (size_t at = 1; at <= calls; ++at) {
        json_fault = (Fault){0, at, 0};
        cache = old;
        assert(!ce_citation_cache_decode(bib_json, strlen(bib_json), NULL, &cache, error, sizeof error));
        assert(cache == old && *error && !json_fault.live);
    }
    total += calls;
    json_fault = (Fault){0};
    CeCslOutputString encoded = {0};
    assert(ce_citation_cache_encode(old, &encoded, error, sizeof error));
    calls = json_fault.calls;
    assert(!json_fault.live);
    char *saved = encoded.data;
    for (size_t at = 1; at <= calls; ++at) {
        json_fault = (Fault){0, at, 0};
        assert(!ce_citation_cache_encode(old, &encoded, error, sizeof error));
        assert(*error && encoded.data == saved && !json_fault.live);
    }
    total += calls;
    cJSON_InitHooks(NULL);
    ce_csl_output_string_free(&encoded);
    ce_citation_cache_free(old);
    printf("citation cache: %zu persistent allocation failures preserved prior results\n", total);
}
int main(void) {
    test_roundtrip();
    test_invalid();
    test_copy_and_limits();
    test_text_boundary();
    test_oom();
    puts("controlled citation cache roundtrip, flags/links/rows, hostile JSON and atomic failures passed");
    return 0;
}
