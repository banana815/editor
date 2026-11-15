/* Include this implementation with test-only allocation interposition so its
 * malloc/calloc/realloc paths, and cJSON hooks, can fail persistently together.
 * Returned blocks still use ordinary malloc: borrowed dependency buffers can
 * safely be freed here without process-global allocator changes in production. */
#include "citation_process.h"
#include "citation_digest.h"
#include "csl_document.h"
#include "csl_json.h"
#include <assert.h>
#include <inttypes.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
typedef struct TestAllocation {
    void *pointer;
    size_t size;
} TestAllocation;
static TestAllocation test_allocations[8192];
static size_t test_calls, test_fail_at, test_live, test_bytes;
static size_t test_find(void *p) {
    for (size_t i = 0; i < test_live; ++i)
        if (test_allocations[i].pointer == p)
            return i;
    return SIZE_MAX;
}
static bool test_reject(void) {
    ++test_calls;
    return test_fail_at && test_calls >= test_fail_at;
}
static void *test_malloc(size_t size) {
    if (test_reject())
        return NULL;
    void *p = malloc(size);
    if (p) {
        assert(test_live < 8192);
        test_allocations[test_live++] = (TestAllocation){p, size};
        test_bytes += size;
    }
    return p;
}
static void test_free(void *p) {
    if (!p)
        return;
    size_t i = test_find(p);
    if (i != SIZE_MAX) {
        test_bytes -= test_allocations[i].size;
        test_allocations[i] = test_allocations[--test_live];
    }
    free(p);
}
static void *test_calloc(size_t n, size_t size) {
    assert(!n || size <= SIZE_MAX / n);
    void *p = test_malloc(n * size);
    if (p)
        memset(p, 0, n * size);
    return p;
}
static void *test_realloc(void *p, size_t size) {
    if (!p)
        return test_malloc(size);
    if (test_reject())
        return NULL;
    size_t i = test_find(p);
    assert(i != SIZE_MAX);
    void *next = realloc(p, size);
    if (!next)
        return NULL;
    test_bytes = test_bytes - test_allocations[i].size + size;
    test_allocations[i] = (TestAllocation){next, size};
    return next;
}
#define malloc test_malloc
#define calloc test_calloc
#define realloc test_realloc
#define free test_free
#include "../src/citation_process.c"
#undef malloc
#undef calloc
#undef realloc
#undef free

static char test_error[512];
static const char *test_item = "{\"id\":\"a\",\"type\":\"book\",\"title\":\"Example "
                               "work\",\"author\":[{\"family\":\"Doe\",\"given\":\"Jane\"}],\"issued\":{"
                               "\"date-parts\":[[2020]]},\"publisher\":\"Press\"}";
static const char *test_reordered =
    "{\"publisher\":\"Press\",\"issued\":{\"date-parts\":[[2020]]},\"author\":[{\"given\":\"Jane\","
    "\"family\":\"Doe\"}],\"title\":\"Example work\",\"type\":\"book\",\"id\":\"transport-other\"}";
static const char *test_uri = "http://zotero.org/users/local/items/AAAA";
static cJSON *test_field(cJSON *o, const char *key) {
    return cJSON_GetObjectItemCaseSensitive(o, key);
}
static void test_put(cJSON *o, const char *key, cJSON *v) {
    assert(v);
    assert(put(o, key, v));
}
static cJSON *test_root(const CeDocument *doc) {
    char *json = ce_document_export_json(doc);
    assert(json);
    cJSON *root = cJSON_Parse(json);
    free(json);
    assert(root);
    return root;
}
static void test_import(CeDocument *doc, cJSON *root) {
    char *json = cJSON_PrintUnformatted(root);
    assert(json);
    assert(ce_document_import(doc, json, CE_FORMAT_JSON, test_error, sizeof test_error));
    cJSON_free(json);
}
static cJSON *test_atom(cJSON *root, size_t index) {
    return cJSON_GetArrayItem(test_field(root, "content"), (int)index + 1);
}
static cJSON *test_add_section(cJSON *root, const char *entries) {
    cJSON *section = cJSON_CreateObject(), *attrs = cJSON_CreateObject();
    assert(section && attrs);
    test_put(section, "type", cJSON_CreateString(BIBLIOGRAPHY));
    test_put(section, "attrs", attrs);
    test_put(attrs, "style", cJSON_CreateString("ieee"));
    test_put(attrs, "entriesJson", cJSON_CreateString(entries));
    assert(cJSON_AddItemToArray(test_field(root, "content"), section));
    return attrs;
}
static CeDocument *test_document(const char *first, const char *second, bool duplicate_id) {
    CeDocument *doc = ce_document_new();
    assert(doc);
    assert(ce_document_import(doc, "Prose", CE_FORMAT_TEXT, test_error, sizeof test_error));
    cJSON *root = test_root(doc), *content = test_field(root, "content");
    cJSON *attrs = cJSON_CreateObject();
    assert(attrs);
    test_put(attrs, "customRoot", cJSON_CreateString("保留根数据"));
    test_put(root, "attrs", attrs);
    const char *snapshots[] = {first, second};
    size_t count = second ? 2 : 1;
    for (size_t i = 0; i < count; ++i) {
        cJSON *node = cJSON_CreateObject(), *a = cJSON_CreateObject();
        assert(node && a);
        test_put(node, "type", cJSON_CreateString(CITATION));
        test_put(node, "attrs", a);
        test_put(a, "citationId", cJSON_CreateString(i && !duplicate_id ? "c2" : "c1"));
        test_put(a, "itemUri", cJSON_CreateString(test_uri));
        test_put(a, "sourceId", cJSON_CreateString("a"));
        test_put(a, "libraryId", cJSON_CreateString("9223372036854775807"));
        test_put(a, "title", cJSON_CreateString("Display title, not a CSL source"));
        test_put(a, "note", cJSON_CreateString("User quote\n第二行"));
        test_put(a, "page", cJSON_CreateString("101-109"));
        test_put(a, "customCitation", cJSON_CreateBool(true));
        if (snapshots[i])
            test_put(a, "cslJson", cJSON_CreateString(snapshots[i]));
        assert(cJSON_AddItemToArray(content, node));
    }
    test_import(doc, root);
    cJSON_Delete(root);
    ce_document_mark_saved(doc);
    return doc;
}
static void test_unchanged(const CeDocument *doc, const char *before) {
    char *after = ce_document_export_json(doc);
    assert(after && !strcmp(after, before));
    free(after);
}
static void test_success(CeDocument *doc, CeCitationStyle *style, CeCitationPrepared **out) {
    bool ok = ce_citations_prepare(doc, style, NULL, NULL, out, test_error, sizeof test_error);
    if (!ok)
        fprintf(stderr, "prepare: %s\n", test_error);
    assert(ok);
}
static void test_profiles_and_idempotence(void) {
    for (int p = 0; p < 3; ++p) {
        CeCitationStyle *style = NULL;
        assert(
            ce_citation_style_load((CeCitationProfile)p, NULL, NULL, &style, test_error, sizeof test_error));
        CeDocument *doc = test_document(test_item, test_reordered, true);
        char *before = ce_document_export_json(doc);
        assert(before);
        CeCitationPrepared *prepared = NULL;
        test_success(doc, style, &prepared);
        test_unchanged(doc, before);
        assert(!ce_document_is_modified(doc) && !ce_document_can_undo(doc));
        const CeCitationProcessStats *stats = ce_citation_prepared_stats(prepared);
        assert(stats->citation_count == 2 && stats->reference_count == 1 && stats->repaired_ids == 1 &&
               !stats->missing_term);
        const CeCitationCache *citation = ce_citation_prepared_citation(prepared, "c1");
        assert(citation);
        assert(ce_citation_prepared_citation(prepared, "cite-c-1"));
        assert(ce_citation_prepared_bibliography(prepared));
        CeCslOutputString plain = {0};
        assert(ce_csl_output_plain(ce_citation_cache_citation_output(citation), &plain, test_error,
                                   sizeof test_error));
        if (p == 0)
            assert(strstr(plain.data, "Doe, 2020") && strstr(plain.data, "101"));
        else
            assert(strstr(plain.data, "1"));
        assert(!strstr(plain.data, "User quote"));
        ce_csl_output_string_free(&plain);
        assert(ce_document_apply_json(doc, ce_citation_prepared_json(prepared)));
        cJSON *root = test_root(doc), *a = test_field(test_atom(root, 0), "attrs"),
              *ra = test_field(root, "attrs");
        assert(!strcmp(test_field(a, "cslJson")->valuestring, test_item));
        assert(cJSON_IsTrue(test_field(a, "customCitation")));
        assert(!strcmp(test_field(a, "note")->valuestring, "User quote\n第二行"));
        assert(test_field(ra, "customRoot"));
        assert(test_field(ra, "citationProfile") && !test_field(ra, "citationLocale"));
        cJSON *section = cJSON_GetArrayItem(test_field(root, "content"), 3);
        assert(!strcmp(text(field(section, "attrs"), "bibliographyMode"), "citations"));
        cJSON_Delete(root);
        char digest[65];
        assert(ce_citations_source_digest(doc, style, NULL, digest, test_error, sizeof test_error));
        assert(!strcmp(digest, ce_citation_prepared_digest(prepared)));
        char *once = copy(ce_citation_prepared_json(prepared));
        assert(once);
        test_success(doc, style, &prepared);
        assert(!strcmp(once, ce_citation_prepared_json(prepared)) &&
               !ce_citation_prepared_stats(prepared)->repaired_ids);
        test_free(once);
        assert(ce_document_undo(doc));
        test_unchanged(doc, before);
        assert(!ce_document_is_modified(doc));
        free(before);
        ce_citation_prepared_free(prepared);
        ce_document_free(doc);
        ce_citation_style_free(style);
    }
    assert(!test_live && !test_bytes);
}
static void test_digest_fields(void) {
    CeCitationStyle *style = NULL;
    assert(
        ce_citation_style_load(CE_CITATION_PROFILE_APA, NULL, NULL, &style, test_error, sizeof test_error));
    CeDocument *doc = test_document(test_item, NULL, false);
    char initial[65], next[65];
    assert(ce_citations_source_digest(doc, style, NULL, initial, test_error, sizeof test_error));
    cJSON *root = test_root(doc);
    test_put(test_field(test_atom(root, 0), "attrs"), "note", cJSON_CreateString("Different quote"));
    cJSON *paragraph = cJSON_GetArrayItem(test_field(root, "content"), 0),
          *text_node = cJSON_GetArrayItem(test_field(paragraph, "content"), 0);
    test_put(text_node, "text", cJSON_CreateString("Different prose"));
    test_import(doc, root);
    cJSON_Delete(root);
    assert(ce_citations_source_digest(doc, style, NULL, next, test_error, sizeof test_error) &&
           !strcmp(initial, next));
    root = test_root(doc);
    test_put(test_field(test_atom(root, 0), "attrs"), "page", cJSON_CreateString("99"));
    test_import(doc, root);
    cJSON_Delete(root);
    assert(ce_citations_source_digest(doc, style, NULL, next, test_error, sizeof test_error) &&
           strcmp(initial, next));
    root = test_root(doc);
    cJSON_DeleteItemFromObjectCaseSensitive(test_field(test_atom(root, 0), "attrs"), "cslJson");
    test_import(doc, root);
    cJSON_Delete(root);
    memset(next, 'q', sizeof next);
    char saved[65];
    memcpy(saved, next, sizeof saved);
    assert(!ce_citations_source_digest(doc, style, NULL, next, test_error, sizeof test_error) &&
           !memcmp(next, saved, sizeof next));
    ce_document_free(doc);
    ce_citation_style_free(style);
    assert(!test_live && !test_bytes);
}
static void test_conflicts_and_fallback(void) {
    CeCitationStyle *style = NULL;
    assert(
        ce_citation_style_load(CE_CITATION_PROFILE_APA, NULL, NULL, &style, test_error, sizeof test_error));
    CeDocument *base = test_document(test_item, NULL, false);
    CeCitationPrepared *prepared = NULL;
    test_success(base, style, &prepared);
    const char *bad[][2] = {
        {"{\"id\":\"a\",\"title\":\"A\"}", "{\"id\":\"b\",\"title\":\"B\"}"},
        {"{\"id\":\"a\",\"title\":\"A\",\"custom\":9007199254740992}",
         "{\"id\":\"a\",\"title\":\"A\",\"custom\":9007199254740993}"},
        {"{\"id\":\"a\",\"title\":\"A\",\"custom\":4503599627370496}",
         "{\"id\":\"a\",\"title\":\"A\",\"custom\":4503599627370497}"},
        {"{\"id\":\"a\",\"title\":\"A\",\"custom\":1.5}", "{\"custom\":1.5,\"title\":\"A\",\"id\":\"a\"}"},
        {"{\"id\":\"a\",\"title\":\"A\",\"custom\":1.0000000000000001}",
         "{\"id\":\"a\",\"title\":\"A\",\"custom\":1}"},
        {"{\"id\":\"a\",\"title\":\"A\",\"custom\":1e-9999}", "{\"id\":\"a\",\"title\":\"A\",\"custom\":0}"},
        {"{\"id\":\"a\",\"title\":\"A\",\"title\":\"A\"}", test_item}};
    for (size_t i = 0; i < sizeof bad / sizeof *bad; ++i) {
        CeDocument *doc = test_document(bad[i][0], bad[i][1], false);
        char *before = ce_document_export_json(doc);
        assert(before);
        CeCitationPrepared *old = prepared;
        assert(!ce_citations_prepare(doc, style, NULL, NULL, &prepared, test_error, sizeof test_error));
        assert(*test_error && prepared == old);
        test_unchanged(doc, before);
        free(before);
        ce_document_free(doc);
    }
    CeDocument *doc = test_document(NULL, NULL, false);
    CeZoteroItem item = {0};
    item.item_uri = (char *)test_uri;
    item.csl_json = (char *)test_item;
    CeZoteroLibrary library = {0};
    library.items = &item;
    library.item_count = 1;
    char *before = ce_document_export_json(doc);
    assert(before);
    assert(ce_citations_prepare(doc, style, &library, NULL, &prepared, test_error, sizeof test_error));
    test_unchanged(doc, before);
    char digest[65];
    assert(ce_citations_source_digest(doc, style, &library, digest, test_error, sizeof test_error));
    assert(!strcmp(digest, ce_citation_prepared_digest(prepared)));
    assert(ce_document_apply_json(doc, ce_citation_prepared_json(prepared)));
    assert(ce_citations_source_digest(doc, style, NULL, digest, test_error, sizeof test_error) &&
           !strcmp(digest, ce_citation_prepared_digest(prepared)));
    assert(ce_document_undo(doc));
    CeZoteroItem duplicate[] = {item, item};
    library.items = duplicate;
    library.item_count = 2;
    CeCitationPrepared *old = prepared;
    assert(!ce_citations_prepare(doc, style, &library, NULL, &prepared, test_error, sizeof test_error) &&
           prepared == old);
    test_unchanged(doc, before);
    free(before);
    ce_document_free(doc);
    ce_document_free(base);
    ce_citation_prepared_free(prepared);
    ce_citation_style_free(style);
    assert(!test_live && !test_bytes);
}
static void test_empty_and_explicit_locale(void) {
    CeCitationStyle *style = NULL;
    assert(
        ce_citation_style_load(CE_CITATION_PROFILE_IEEE, NULL, NULL, &style, test_error, sizeof test_error));
    CeDocument *doc = ce_document_new();
    assert(doc);
    assert(ce_document_import(doc, "Plain note", CE_FORMAT_TEXT, test_error, sizeof test_error));
    char *before = ce_document_export_json(doc);
    assert(before);
    CeCitationPrepared *prepared = NULL;
    test_success(doc, style, &prepared);
    assert(!strcmp(before, ce_citation_prepared_json(prepared)) &&
           !ce_citation_prepared_bibliography(prepared));
    CeCitationProcessOptions options = {true};
    assert(ce_citations_prepare(doc, style, NULL, &options, &prepared, test_error, sizeof test_error));
    cJSON *root = cJSON_Parse(ce_citation_prepared_json(prepared));
    assert(root);
    assert(!strcmp(text(field(root, "attrs"), "citationProfile"), "ieee") &&
           !field(field(root, "attrs"), "citationLocale"));
    assert(cJSON_GetArraySize(test_field(root, "content")) == 1);
    cJSON_Delete(root);
    free(before);
    ce_document_free(doc);
    doc = test_document(test_item, NULL, false);
    root = test_root(doc);
    test_put(test_field(root, "attrs"), "citationLocale", cJSON_CreateString("en-US"));
    test_import(doc, root);
    cJSON_Delete(root);
    test_success(doc, style, &prepared);
    root = cJSON_Parse(ce_citation_prepared_json(prepared));
    assert(root);
    assert(!strcmp(text(field(root, "attrs"), "citationLocale"), "en-US"));
    cJSON_Delete(root);
    ce_document_free(doc);
    ce_citation_prepared_free(prepared);
    ce_citation_style_free(style);
    assert(!test_live && !test_bytes);
}
static void test_identity_scopes_and_order(void) {
    CeCitationStyle *style = NULL;
    assert(
        ce_citation_style_load(CE_CITATION_PROFILE_IEEE, NULL, NULL, &style, test_error, sizeof test_error));
    CeDocument *doc = test_document(test_item, test_item, false);
    cJSON *root = test_root(doc);
    for (size_t i = 0; i < 2; ++i)
        cJSON_DeleteItemFromObjectCaseSensitive(test_field(test_atom(root, i), "attrs"), "itemUri");
    test_put(test_field(test_atom(root, 1), "attrs"), "libraryId", cJSON_CreateString("2"));
    test_import(doc, root);
    cJSON_Delete(root);
    CeCitationPrepared *prepared = NULL;
    test_success(doc, style, &prepared);
    assert(ce_citation_prepared_stats(prepared)->reference_count == 2);
    const CeCitationCache *bib = ce_citation_prepared_bibliography(prepared);
    assert(ce_citation_cache_entry_count(bib) == 2);
    assert(strstr(ce_citation_cache_entry(bib, 0)->source_item_id, "9223372036854775807"));
    char first[65], reordered[65];
    assert(ce_citations_source_digest(doc, style, NULL, first, test_error, sizeof test_error));
    root = test_root(doc);
    cJSON *content = test_field(root, "content"), *moved = cJSON_DetachItemFromArray(content, 1);
    assert(moved && cJSON_AddItemToArray(content, moved));
    test_import(doc, root);
    cJSON_Delete(root);
    assert(ce_citations_source_digest(doc, style, NULL, reordered, test_error, sizeof test_error) &&
           strcmp(first, reordered));
    root = test_root(doc);
    for (size_t i = 0; i < 2; ++i)
        test_put(test_field(test_atom(root, i), "attrs"), "itemUri", cJSON_CreateString(test_uri));
    test_import(doc, root);
    cJSON_Delete(root);
    test_success(doc, style, &prepared);
    assert(ce_citation_prepared_stats(prepared)->reference_count == 1);
    ce_citation_prepared_free(prepared);
    ce_document_free(doc);
    ce_citation_style_free(style);
    assert(!test_live && !test_bytes);
}
static void test_legacy_metadata(void) {
    CeCitationStyle *style = NULL;
    assert(
        ce_citation_style_load(CE_CITATION_PROFILE_IEEE, NULL, NULL, &style, test_error, sizeof test_error));
    CeDocument *doc = test_document(test_item, NULL, false);
    cJSON *root = test_root(doc);
    cJSON *section = cJSON_CreateObject(), *a = cJSON_CreateObject();
    assert(section && a);
    test_put(section, "type", cJSON_CreateString(BIBLIOGRAPHY));
    test_put(section, "attrs", a);
    test_put(a, "customSection", cJSON_CreateString("Keep this"));
    test_put(a, "entriesJson",
             cJSON_CreateString("[{\"citationId\":\"c1\",\"customEntry\":{\"label\":\"Keep "
                                "me\",\"rank\":7,\"large\":4503599627370501}}]"));
    assert(cJSON_AddItemToArray(test_field(root, "content"), section));
    test_import(doc, root);
    cJSON_Delete(root);
    CeCitationPrepared *prepared = NULL;
    test_success(doc, style, &prepared);
    root = cJSON_Parse(ce_citation_prepared_json(prepared));
    assert(root);
    section =
        cJSON_GetArrayItem(test_field(root, "content"), cJSON_GetArraySize(test_field(root, "content")) - 1);
    a = test_field(section, "attrs");
    assert(!strcmp(text(a, "customSection"), "Keep this"));
    assert(!strcmp(text(a, "bibliographyMode"), "citations"));
    cJSON *entries = cJSON_Parse(text(a, "entriesJson"));
    assert(entries);
    cJSON *entry = cJSON_GetArrayItem(entries, 0);
    assert(!strcmp(text(field(entry, "customEntry"), "label"), "Keep me"));
    assert(field(field(entry, "customEntry"), "large")->valuedouble == 4503599627370501.0);
    assert(strstr(text(a, "entriesJson"), "4503599627370501"));
    assert(!strcmp(text(entry, "formatting"), "csl") && strstr(text(entry, "entryHtml"), "Example work"));
    cJSON_Delete(entries);
    cJSON_Delete(root);
    const char *bad[] = {"[{\"citationId\":\"c1\",\"custom\":9007199254740993}]",
                         "[{\"citationId\":\"c1\",\"custom\":1.0000000000000001}]",
                         "[{\"citationId\":\"c1\",\"custom\":1e-9999}]",
                         "[{\"citationId\":\"unknown\",\"custom\":\"Cannot discard\"}]"};
    for (size_t i = 0; i < sizeof bad / sizeof *bad; ++i) {
        root = test_root(doc);
        section = cJSON_GetArrayItem(test_field(root, "content"), 2);
        a = test_field(section, "attrs");
        test_put(a, "entriesJson", cJSON_CreateString(bad[i]));
        test_import(doc, root);
        cJSON_Delete(root);
        char *before = ce_document_export_json(doc);
        assert(before);
        CeCitationPrepared *old = prepared;
        assert(!ce_citations_prepare(doc, style, NULL, NULL, &prepared, test_error, sizeof test_error) &&
               prepared == old && *test_error);
        test_unchanged(doc, before);
        free(before);
    }
    ce_document_free(doc);
    ce_citation_prepared_free(prepared);
    ce_citation_style_free(style);
    assert(!test_live && !test_bytes);
}
static void test_bibliography_intent(void) {
    CeCitationStyle *style = NULL;
    assert(
        ce_citation_style_load(CE_CITATION_PROFILE_IEEE, NULL, NULL, &style, test_error, sizeof test_error));
    CeDocument *base = test_document(test_item, NULL, false);
    CeCitationPrepared *prepared = NULL;
    test_success(base, style, &prepared);
    /* A cache is never evidence that an old bibliography is derived. Every
     * unmarked entry must survive reconciliation, including entries in later
     * sections and ordinary entries with no custom metadata. */
    for (unsigned variant = 0; variant < 8; ++variant) {
        CeDocument *doc = test_document(test_item, NULL, false);
        cJSON *root = test_root(doc), *content = test_field(root, "content");
        if (variant == 0 || variant == 1)
            cJSON_DeleteItemFromArray(content, 1);
        cJSON *attrs =
            test_add_section(root, variant == 2 ? "[{\"citationId\":\"c1\"},{\"citationId\":\"uncited\","
                                                  "\"entryHtml\":\"Independent source\"}]"
                                   : variant >= 3 && variant <= 5 ? "[{\"citationId\":\"c1\"}]"
                                                                  : "[{\"citationId\":\"uncited\","
                                                                    "\"entryHtml\":\"Independent source\"}]");
        if (variant == 1)
            test_put(attrs, "cslRenderJson", cJSON_CreateString("{\"kind\":\"bibliography\"}"));
        if (variant >= 3 && variant <= 5) {
            test_put(attrs, "bibliographyMode", cJSON_CreateString("citations"));
            attrs = test_add_section(root, "[{\"citationId\":\"uncited\","
                                           "\"entryHtml\":\"Later independent source\"}]");
        }
        if (variant == 4)
            test_put(attrs, "customSection", cJSON_CreateString("Keep section metadata"));
        if (variant == 5)
            test_put(attrs, "entriesJson",
                     cJSON_CreateString("[{\"citationId\":\"uncited\",\"custom\":\"Keep entry\"}]"));
        if (variant == 6) {
            test_put(attrs, "bibliographyMode", cJSON_CreateString("citations"));
            test_put(attrs, "entriesJson",
                     cJSON_CreateString("[{\"citationId\":\"uncited\",\"custom\":\"Keep entry\"}]"));
        }
        if (variant == 7) {
            test_put(attrs, "entriesJson", cJSON_CreateString("[{\"citationId\":\"c1\"}]"));
            test_add_section(root, "[{\"citationId\":\"c1\"}]");
        }
        test_import(doc, root);
        cJSON_Delete(root);
        char *before = ce_document_export_json(doc);
        assert(before);
        CeCitationPrepared *old = prepared;
        assert(!ce_citations_prepare(doc, style, NULL, NULL, &prepared, test_error, sizeof test_error));
        assert(prepared == old && strstr(test_error, variant == 7 ? "Ambiguous" : "retained"));
        test_unchanged(doc, before);
        free(before);
        ce_document_free(doc);
    }
    /* Missing mode is distinct from every unsupported explicit value, even on
     * an empty later section. Neither a future value nor a bad type is guessed. */
    const char *bad_modes[] = {"\"future\"", "\"\"", "null", "true", "1", "{}", "[]"};
    for (size_t i = 0; i < sizeof bad_modes / sizeof *bad_modes; ++i) {
        for (unsigned later = 0; later < 2; ++later) {
            CeDocument *doc = test_document(test_item, NULL, false);
            cJSON *root = test_root(doc);
            if (later)
                test_add_section(root, "[{\"citationId\":\"c1\"}]");
            cJSON *attrs = test_add_section(root, later ? "[]" : "[{\"citationId\":\"c1\"}]");
            test_put(attrs, "bibliographyMode", cJSON_Parse(bad_modes[i]));
            test_import(doc, root);
            cJSON_Delete(root);
            char *before = ce_document_export_json(doc);
            assert(before);
            CeCitationPrepared *old = prepared;
            assert(!ce_citations_prepare(doc, style, NULL, NULL, &prepared, test_error, sizeof test_error));
            assert(prepared == old && strstr(test_error, "bibliographyMode"));
            test_unchanged(doc, before);
            free(before);
            ce_document_free(doc);
        }
    }
    /* Fully matched legacy sections can be reconciled without losing entries;
     * the resulting intent is explicit and repeated preparation is identical. */
    CeDocument *doc = test_document(test_item, test_item, false);
    cJSON *root = test_root(doc);
    test_put(test_field(test_atom(root, 1), "attrs"), "itemUri",
             cJSON_CreateString("http://zotero.org/users/local/items/BBBB"));
    test_add_section(root, "[{\"citationId\":\"c1\"}]");
    test_add_section(root, "[{\"citationId\":\"c2\"}]");
    test_import(doc, root);
    cJSON_Delete(root);
    test_success(doc, style, &prepared);
    assert(ce_citation_prepared_stats(prepared)->reference_count == 2 &&
           ce_citation_prepared_stats(prepared)->removed_sections == 1);
    assert(ce_document_apply_json(doc, ce_citation_prepared_json(prepared)));
    char *once = ce_document_export_json(doc);
    assert(once);
    test_success(doc, style, &prepared);
    assert(!strcmp(once, ce_citation_prepared_json(prepared)));
    free(once);
    root = test_root(doc);
    cJSON *content = test_field(root, "content");
    cJSON_DeleteItemFromArray(content, 2);
    cJSON_DeleteItemFromArray(content, 1);
    test_import(doc, root);
    cJSON_Delete(root);
    test_success(doc, style, &prepared);
    assert(!ce_citation_prepared_bibliography(prepared) &&
           ce_citation_prepared_stats(prepared)->removed_sections == 1 &&
           ce_citation_prepared_stats(prepared)->reference_count == 0);
    root = cJSON_Parse(ce_citation_prepared_json(prepared));
    assert(root && cJSON_GetArraySize(test_field(root, "content")) == 1);
    cJSON_Delete(root);
    ce_document_free(doc);
    ce_document_free(base);
    ce_citation_prepared_free(prepared);
    ce_citation_style_free(style);
    assert(!test_live && !test_bytes);
}
static void test_envelope_integer_preservation(void) {
    CeCitationStyle *style = NULL;
    assert(
        ce_citation_style_load(CE_CITATION_PROFILE_IEEE, NULL, NULL, &style, test_error, sizeof test_error));
    CeDocument *doc =
        test_document("{\"id\":\"a\",\"title\":\"A\",\"custom\":4503599627370501}", NULL, false);
    Source source = {0};
    assert(source_build(doc, style, NULL, false, &source, test_error, sizeof test_error));
    const CeCslInputItem *item = ce_csl_input_item(source.input, 0);
    assert(item);
    assert(field(item->data, "custom")->valuedouble == 4503599627370501.0);
    source_free(&source);
    ce_document_free(doc);
    ce_citation_style_free(style);
    assert(!test_live && !test_bytes);
}
static void test_cumulative_budget(void) {
    size_t used = SOURCE_LIMIT - 33;
    assert(json_string_budget(&used, "x", 1, test_error, sizeof test_error) && used == SOURCE_LIMIT);
    assert(!json_string_budget(&used, "x", 1, test_error, sizeof test_error) && used == SOURCE_LIMIT);
    used = SOURCE_LIMIT - 33;
    assert(!json_string_budget(&used, "\"", 1, test_error, sizeof test_error) && used == SOURCE_LIMIT - 33);
    const char *hash = "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
    const char *commit = "0123456789abcdef0123456789abcdef01234567";
    CeCitationCacheHeader h = {CE_CITATION_PROCESSOR_PROFILE,
                               "apa",
                               "http://www.zotero.org/styles/apa",
                               hash,
                               commit,
                               "en-us",
                               commit,
                               hash};
    size_t n = CE_CSL_OUTPUT_MAX_BYTES;
    char *text_buffer = malloc(n);
    assert(text_buffer);
    memset(text_buffer, 'x', n);
    CeCslOutput *output = ce_csl_output_new(NULL, test_error, sizeof test_error);
    assert(output);
    assert(ce_csl_output_append(output, text_buffer, n, 0, NULL, test_error, sizeof test_error));
    free(text_buffer);
    CeCitationCache *cache = NULL;
    assert(ce_citation_cache_from_citation(&h, "large", output, NULL, &cache, test_error, sizeof test_error));
    ce_csl_output_free(output);
    CeCitationPrepared p = {0};
    cJSON *attrs = cJSON_CreateObject();
    assert(attrs);
    size_t accepted = 0;
    while (attach_cache(attrs, cache, &p, test_error, sizeof test_error))
        ++accepted;
    assert(accepted > 0 && accepted < 16 && strstr(test_error, "cache"));
    assert(p.generated_bytes <= SOURCE_LIMIT);
    cJSON_Delete(attrs);
    ce_citation_cache_free(cache);
    assert(!test_live && !test_bytes);
}
static CeDocument *test_selected_document(bool body,bool repeated) {
    CeDocument *doc=test_document(test_item,NULL,false);
    cJSON *root=test_root(doc),*content=test_field(root,"content");
    cJSON *attrs=cJSON_Duplicate(test_field(test_atom(root,0),"attrs"),true); assert(attrs);
    const char *remove[]={"citationId","page","note","endnoteNumber"};
    for(size_t i=0;i<sizeof remove/sizeof *remove;i++) cJSON_DeleteItemFromObjectCaseSensitive(attrs,remove[i]);
    cJSON *sources=cJSON_CreateArray(); assert(sources && cJSON_AddItemToArray(sources,attrs));
    char *json=ce_csl_json_print_exact(sources); assert(json); cJSON_Delete(sources);
    if(!body) cJSON_DeleteItemFromArray(content,1);
    for(size_t i=0;i<(repeated?2u:1u);i++) {
        cJSON *section_attrs=test_add_section(root,"[]");
        test_put(section_attrs,"bibliographyMode",cJSON_CreateString("selected"));
        test_put(section_attrs,"selectedItemsJson",cJSON_CreateString(json));
        test_put(section_attrs,"sharedCustom",cJSON_CreateString("preserve"));
    }
    cJSON_free(json); test_import(doc,root); cJSON_Delete(root); return doc;
}
static void test_selected_sources(void) {
    CeCitationStyle *style=NULL; assert(ce_citation_style_load(CE_CITATION_PROFILE_APA,NULL,NULL,&style,test_error,sizeof test_error));
    CeDocument *doc=test_selected_document(false,true); CeCitationPrepared *prepared=NULL;
    test_success(doc,style,&prepared);
    assert(!prepared->stats.citation_count && prepared->stats.reference_count==1 && prepared->stats.selected_count==1 && prepared->stats.selected_bibliography);
    assert(ce_document_apply_json(doc,prepared->json));
    char *once=ce_document_export_json(doc); test_success(doc,style,&prepared); assert(!strcmp(once,prepared->json)); free(once);
    cJSON *root=test_root(doc),*section_node=cJSON_GetArrayItem(test_field(root,"content"),1);
    cJSON *entries=NULL; const char *encoded=text(field(section_node,"attrs"),"entriesJson");
    assert(ce_csl_json_parse_strict(encoded,strlen(encoded),&entries,test_error,sizeof test_error));
    assert(!*text(entries->child,"citationId") && *text(entries->child,"sourceItemId")); cJSON_Delete(entries);
    /* A copied, explicitly derived section can contain repeated obsolete cache
     * rows; identity and original source declarations are still checked. */
    assert(cJSON_AddItemToArray(test_field(root,"content"),cJSON_Duplicate(section_node,true)));
    test_import(doc,root); cJSON_Delete(root); test_success(doc,style,&prepared);
    assert(prepared->stats.reference_count==1 && prepared->stats.selected_count==1 && prepared->stats.removed_sections==1);
    ce_document_free(doc);
    for(size_t variant=0;variant<12;variant++) {
        doc=test_selected_document(true,true); root=test_root(doc);
        cJSON *attrs=test_field(cJSON_GetArrayItem(test_field(root,"content"),3),"attrs"),*sources=NULL;
        encoded=text(attrs,"selectedItemsJson");
        assert(ce_csl_json_parse_strict(encoded,strlen(encoded),&sources,test_error,sizeof test_error));
        cJSON *source=sources->child;
        if(variant==0) test_put(source,"cslJson",cJSON_CreateString("{\"id\":\"other\",\"type\":\"book\",\"title\":\"Conflict\"}"));
        if(variant==1) test_put(source,"customCitation",cJSON_CreateBool(false));
        if(variant==2) test_put(source,"sourceId",cJSON_CreateString("different transport identity"));
        if(variant==3) test_put(source,"libraryId",cJSON_CreateNumber(1));
        if(variant==4) test_put(source,"libraryId",cJSON_CreateString("01"));
        if(variant==5) test_put(source,"cslJson",cJSON_CreateString(""));
        if(variant==10) {
            cJSON *first_attrs=test_field(cJSON_GetArrayItem(test_field(root,"content"),2),"attrs"),*first_sources=NULL;
            const char *first_text=text(first_attrs,"selectedItemsJson");
            assert(ce_csl_json_parse_strict(first_text,strlen(first_text),&first_sources,test_error,sizeof test_error));
            test_put(first_sources->child,"exactCustom",cJSON_CreateNumber(4503599627370496.0));
            char *first_json=ce_csl_json_print_exact(first_sources); assert(first_json); cJSON_Delete(first_sources);
            test_put(first_attrs,"selectedItemsJson",cJSON_CreateString(first_json)); cJSON_free(first_json);
            test_put(source,"exactCustom",cJSON_CreateNumber(4503599627370497.0));
        }
        char *json=ce_csl_json_print_exact(sources); assert(json); cJSON_Delete(sources);
        test_put(attrs,"selectedItemsJson",cJSON_CreateString(json)); cJSON_free(json);
        if(variant==6) test_put(attrs,"selectedItemsJson",cJSON_CreateString("[{\"itemUri\":\"x\",\"cslJson\":\"{}\",\"unknown\":1.0000000000000001}]"));
        if(variant==7) test_put(attrs,"selectedItemsJson",cJSON_CreateNull());
        if(variant==11) test_put(attrs,"selectedItemsJson",cJSON_CreateString("[{\"itemUri\":\"x\",\"cslJson\":\"{}\",\"unknown\":1e-9999}]"));
        if(variant==8) test_put(attrs,"sharedCustom",cJSON_CreateString("conflicting extra section"));
        if(variant==9) {
            test_put(attrs,"entriesJson",cJSON_CreateString("[{\"sourceItemId\":\"uri:http://zotero.org/users/local/items/AAAA\",\"custom\":true}]"));
        }
        test_import(doc,root); cJSON_Delete(root); char *before=ce_document_export_json(doc); CeCitationPrepared *old=prepared;
        assert(!ce_citations_prepare(doc,style,NULL,NULL,&prepared,test_error,sizeof test_error) && prepared==old);
        test_unchanged(doc,before); free(before); ce_document_free(doc);
    }
    ce_citation_prepared_free(prepared); ce_citation_style_free(style); assert(!test_live && !test_bytes);
}
static void test_prepare_oom(unsigned variant) {
    bool complex = variant == 1;
    CeCitationStyle *style = NULL;
    assert(
        ce_citation_style_load(CE_CITATION_PROFILE_IEEE, NULL, NULL, &style, test_error, sizeof test_error));
    CeDocument *doc = variant==3?test_selected_document(false,true):
        test_document(complex ? NULL : test_item, complex ? test_reordered : NULL, complex);
    CeZoteroItem item = {0};
    item.id = (char *)"a";
    item.item_uri = (char *)test_uri;
    item.item_key = (char *)"AAAA";
    item.cite_key = (char *)"Doe2020";
    item.csl_json = (char *)test_item;
    item.library_id = INT64_MAX;
    CeZoteroLibrary library = {0};
    library.items = &item;
    library.item_count = 1;
    if (complex) {
        cJSON *root = test_root(doc), *section = cJSON_CreateObject(), *a = cJSON_CreateObject();
        assert(section && a);
        test_put(section, "type", cJSON_CreateString(BIBLIOGRAPHY));
        test_put(section, "attrs", a);
        test_put(a, "customSection", cJSON_CreateBool(true));
        test_put(a, "entriesJson",
                 cJSON_CreateString("[{\"citationId\":\"c1\",\"customEntry\":\"Retain me\"}]"));
        assert(cJSON_AddItemToArray(test_field(root, "content"), section));
        cJSON *extra = test_add_section(root, "[{\"citationId\":\"removed\",\"entryHtml\":\"Obsolete\"}]");
        test_put(extra, "bibliographyMode", cJSON_CreateString("citations"));
        test_import(doc, root);
        cJSON_Delete(root);
    }
    if (variant == 2) {
        CeCitationPrepared *initial = NULL;
        test_success(doc, style, &initial);
        assert(ce_document_apply_json(doc, ce_citation_prepared_json(initial)));
        ce_citation_prepared_free(initial);
        cJSON *root = test_root(doc);
        cJSON_DeleteItemFromArray(test_field(root, "content"), 1);
        test_import(doc, root);
        cJSON_Delete(root);
    }
    CeCitationPrepared *old = NULL;
    assert(ce_citations_prepare(doc, style, &library, NULL, &old, test_error, sizeof test_error));
    char *before = ce_document_export_json(doc);
    assert(before);
    size_t live = test_live, bytes = test_bytes;
    cJSON_Hooks hooks = {test_malloc, test_free};
    cJSON_InitHooks(&hooks);
    test_calls = 0;
    CeCitationPrepared *candidate = NULL;
    assert(ce_citations_prepare(doc, style, &library, NULL, &candidate, test_error, sizeof test_error));
    size_t calls = test_calls;
    ce_citation_prepared_free(candidate);
    assert(test_live == live && test_bytes == bytes);
    for (size_t at = 1; at <= calls; ++at) {
        test_calls = 0;
        test_fail_at = at;
        candidate = old;
        bool ok = ce_citations_prepare(doc, style, &library, NULL, &candidate, test_error, sizeof test_error);
        test_fail_at = 0;
        if (ok)
            fprintf(stderr, "unexpected prepare success at allocation %zu/%zu\n", at, calls);
        assert(!ok && *test_error && candidate == old);
        assert(test_live == live && test_bytes == bytes);
    }
    cJSON_InitHooks(NULL);
    test_unchanged(doc, before);
    free(before);
    ce_citation_prepared_free(old);
    ce_document_free(doc);
    ce_citation_style_free(style);
    assert(!test_live && !test_bytes);
    printf("citation prepare%s: %zu persistent process/cJSON allocation failures preserved old result and "
           "source\n",
           complex        ? " (fallback/repair/legacy metadata)"
           : variant == 2 ? " (last citation removal)"
           : variant == 3 ? " (uncited selected sources/multiple sections)"
                          : "",
           calls);
}
int main(void) {
    test_profiles_and_idempotence();
    test_digest_fields();
    test_conflicts_and_fallback();
    test_empty_and_explicit_locale();
    test_identity_scopes_and_order();
    test_legacy_metadata();
    test_bibliography_intent();
    test_envelope_integer_preservation();
    test_cumulative_budget();
    test_selected_sources();
    test_prepare_oom(0);
    test_prepare_oom(1);
    test_prepare_oom(2);
    test_prepare_oom(3);
    puts("citation prepare: real profiles, identity/metadata conflicts, source bindings, preserved attrs, "
         "fallback and atomic failures passed");
    return 0;
}
