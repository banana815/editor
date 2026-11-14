#include "citation_cache.h"
#include "csl_json.h"
#include "csl_unicode.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef union Allocation {
    long double alignment;
    void *pointer;
    size_t bytes;
    unsigned char padding[32];
} Allocation;
struct CeCitationCache {
    CeCslOutputAllocator allocator;
    size_t workspace, payload, runs, rows;
    CeCitationCacheKind kind;
    CeCitationCacheHeader header;
    char *citation_id;
    CeCslOutput *citation;
    CeCslBibliographyOptions options;
    CeCitationCacheEntry *entries;
    size_t entry_count;
};
static bool fail(char *error, size_t cap, const char *message) {
    if (error && cap)
        snprintf(error, cap, "%s", message);
    return false;
}
static void *normal_allocate(void *context, size_t bytes) {
    (void)context;
    return malloc(bytes);
}
static void normal_free(void *context, void *memory) {
    (void)context;
    free(memory);
}
static void *allocate(void *context, size_t bytes) {
    CeCitationCache *c = context;
    if (bytes > CE_CITATION_CACHE_MAX_WORKSPACE - sizeof(Allocation) ||
        c->workspace > CE_CITATION_CACHE_MAX_WORKSPACE - bytes - sizeof(Allocation))
        return NULL;
    Allocation *p = c->allocator.allocate(c->allocator.context, sizeof *p + bytes);
    if (!p)
        return NULL;
    p->bytes = sizeof *p + bytes;
    c->workspace += p->bytes;
    return p + 1;
}
static void release(void *context, void *memory) {
    if (!memory)
        return;
    CeCitationCache *c = context;
    Allocation *p = (Allocation *)memory - 1;
    c->workspace -= p->bytes;
    c->allocator.deallocate(c->allocator.context, p);
}
static bool payload(CeCitationCache *c, size_t size) {
    if (size > CE_CITATION_CACHE_MAX_BYTES - c->payload)
        return false;
    c->payload += size;
    return true;
}
static bool valid_string(const char *text, size_t limit, bool empty) {
    if (!text)
        return false;
    size_t size = 0;
    while (size <= limit && text[size])
        ++size;
    if (size > limit || (!empty && !size) || ce_csl_unicode_validate(text, size, NULL) != CE_CSL_UNICODE_OK)
        return false;
    for (size_t i = 0; i < size; ++i)
        if ((unsigned char)text[i] < 32 || (unsigned char)text[i] == 127)
            return false;
    return true;
}
static char *copy(CeCitationCache *c, const char *text, size_t limit) {
    if (!valid_string(text, limit, false))
        return NULL;
    size_t size = strlen(text);
    if (!payload(c, size))
        return NULL;
    char *p = allocate(c, size + 1);
    if (p)
        memcpy(p, text, size + 1);
    return p;
}
static bool hex_string(const char *text, size_t size) {
    if (!text || strlen(text) != size)
        return false;
    for (size_t i = 0; i < size; ++i)
        if (!((text[i] >= '0' && text[i] <= '9') || (text[i] >= 'a' && text[i] <= 'f')))
            return false;
    return true;
}
static bool valid_header(const CeCitationCacheHeader *h) {
    return h && h->processor_profile && !strcmp(h->processor_profile, CE_CITATION_PROCESSOR_PROFILE) &&
           valid_string(h->profile, 128, false) && valid_string(h->style_id, 4096, false) &&
           hex_string(h->style_sha256, 64) && hex_string(h->style_commit, 40) &&
           valid_string(h->locale, 63, false) && hex_string(h->locale_commit, 40) &&
           hex_string(h->source_digest, 64);
}
static CeCitationCache *create(const CeCitationCacheHeader *header, CeCitationCacheKind kind,
                               const CeCslOutputAllocator *allocator, char *error, size_t capacity) {
    if (!valid_header(header)) {
        fail(error, capacity, "Invalid or unsupported citation cache header");
        return NULL;
    }
    CeCslOutputAllocator a = {NULL, normal_allocate, normal_free};
    if (allocator)
        a = *allocator;
    if (!a.allocate || !a.deallocate) {
        fail(error, capacity, "Cache allocator needs both callbacks");
        return NULL;
    }
    CeCitationCache *c = a.allocate(a.context, sizeof *c);
    if (!c) {
        fail(error, capacity, "Out of memory creating citation cache");
        return NULL;
    }
    memset(c, 0, sizeof *c);
    c->allocator = a;
    c->kind = kind;
    c->workspace = sizeof *c;
#define FIELD(name, limit)                                                                                   \
    do {                                                                                                     \
        c->header.name = copy(c, header->name, limit);                                                       \
        if (!c->header.name)                                                                                 \
            goto bad;                                                                                        \
    } while (0)
    FIELD(processor_profile, 128);
    FIELD(profile, 128);
    FIELD(style_id, 4096);
    FIELD(style_sha256, 64);
    FIELD(style_commit, 40);
    FIELD(locale, 63);
    FIELD(locale_commit, 40);
    FIELD(source_digest, 64);
#undef FIELD
    return c;
bad:
    ce_citation_cache_free(c);
    fail(error, capacity, "Unable to copy citation cache header");
    return NULL;
}
void ce_citation_cache_free(CeCitationCache *c) {
    if (!c)
        return;
    ce_csl_output_free(c->citation);
    release(c, c->citation_id);
    for (size_t i = 0; i < c->entry_count; ++i) {
        CeCitationCacheEntry *e = &c->entries[i];
        for (size_t r = 0; r < e->row_count; ++r) {
            ce_csl_output_free((CeCslOutput *)e->rows[r].left);
            ce_csl_output_free((CeCslOutput *)e->rows[r].body);
        }
        release(c, (void *)e->rows);
        release(c, (void *)e->source_item_id);
    }
    release(c, c->entries);
#define FREE_FIELD(name) release(c, (void *)c->header.name)
    FREE_FIELD(processor_profile);
    FREE_FIELD(profile);
    FREE_FIELD(style_id);
    FREE_FIELD(style_sha256);
    FREE_FIELD(style_commit);
    FREE_FIELD(locale);
    FREE_FIELD(locale_commit);
    FREE_FIELD(source_digest);
#undef FREE_FIELD
    c->allocator.deallocate(c->allocator.context, c);
}
CeCitationCacheKind ce_citation_cache_kind(const CeCitationCache *c) {
    return c ? c->kind : CE_CITATION_CACHE_CITATION;
}
const CeCitationCacheHeader *ce_citation_cache_header(const CeCitationCache *c) {
    return c ? &c->header : NULL;
}
const char *ce_citation_cache_citation_id(const CeCitationCache *c) {
    return c && c->kind == CE_CITATION_CACHE_CITATION ? c->citation_id : NULL;
}
const CeCslOutput *ce_citation_cache_citation_output(const CeCitationCache *c) {
    return c && c->kind == CE_CITATION_CACHE_CITATION ? c->citation : NULL;
}
const CeCslBibliographyOptions *ce_citation_cache_bibliography_options(const CeCitationCache *c) {
    return c && c->kind == CE_CITATION_CACHE_BIBLIOGRAPHY ? &c->options : NULL;
}
size_t ce_citation_cache_entry_count(const CeCitationCache *c) {
    return c && c->kind == CE_CITATION_CACHE_BIBLIOGRAPHY ? c->entry_count : 0;
}
const CeCitationCacheEntry *ce_citation_cache_entry(const CeCitationCache *c, size_t i) {
    return i < ce_citation_cache_entry_count(c) ? &c->entries[i] : NULL;
}
static bool append_run(CeCitationCache *c, CeCslOutput *out, const char *text, size_t size, unsigned format,
                       const char *href, char *error, size_t capacity) {
    size_t link = href ? strlen(href) : 0;
    if (c->runs == CE_CITATION_CACHE_MAX_RUNS || !payload(c, size) || !payload(c, link))
        return fail(error, capacity, "Citation cache payload/run limit exceeded");
    ++c->runs;
    return ce_csl_output_append(out, text, size, format, href, error, capacity);
}
static CeCslOutput *copy_output(CeCitationCache *c, const CeCslOutput *source, char *error, size_t capacity) {
    if (!source) {
        fail(error, capacity, "Missing controlled citation output");
        return NULL;
    }
    CeCslOutputAllocator a = {c, allocate, release};
    CeCslOutput *out = ce_csl_output_new(&a, error, capacity);
    if (!out)
        return NULL;
    for (size_t i = 0; i < ce_csl_output_run_count(source); ++i) {
        const CeCslOutputRun *run = ce_csl_output_run(source, i);
        if (!append_run(c, out, run->text, run->byte_length, run->format, run->href, error, capacity)) {
            ce_csl_output_free(out);
            return NULL;
        }
    }
    return out;
}
static bool valid_options(const CeCslBibliographyOptions *o) {
    return o &&
           (o->alignment == CE_CSL_BIBLIOGRAPHY_ALIGN_NONE ||
            o->alignment == CE_CSL_BIBLIOGRAPHY_ALIGN_FLUSH ||
            o->alignment == CE_CSL_BIBLIOGRAPHY_ALIGN_MARGIN) &&
           o->line_spacing > 0;
}
static bool equal_options(const CeCslBibliographyOptions *a, const CeCslBibliographyOptions *b) {
    return b && a->alignment == b->alignment && a->hanging_indent == b->hanging_indent &&
           a->line_spacing == b->line_spacing && a->entry_spacing == b->entry_spacing;
}
static bool reserve_entries(CeCitationCache *c, size_t count, char *error, size_t capacity) {
    if (count > CE_CITATION_CACHE_MAX_ENTRIES)
        return fail(error, capacity, "Citation cache entry limit exceeded");
    if (!count)
        return true;
    c->entries = allocate(c, count * sizeof *c->entries);
    if (!c->entries)
        return fail(error, capacity, "Out of memory allocating cache entries");
    memset(c->entries, 0, count * sizeof *c->entries);
    c->entry_count = count;
    return true;
}
static CeCitationCacheRow *reserve_rows(CeCitationCache *c, CeCitationCacheEntry *entry, size_t count,
                                        char *error, size_t capacity) {
    if (count > CE_CITATION_CACHE_MAX_ROWS - c->rows) {
        fail(error, capacity, "Citation cache row limit exceeded");
        return NULL;
    }
    if (!count)
        return NULL;
    CeCitationCacheRow *rows = allocate(c, count * sizeof *rows);
    if (!rows) {
        fail(error, capacity, "Out of memory allocating cache rows");
        return NULL;
    }
    memset(rows, 0, count * sizeof *rows);
    entry->rows = rows;
    entry->row_count = count;
    c->rows += count;
    return rows;
}
static bool entry_visible(const CeCitationCacheEntry *e) {
    for (size_t r = 0; r < e->row_count; ++r)
        if (ce_csl_output_text_bytes(e->rows[r].left) || ce_csl_output_text_bytes(e->rows[r].body))
            return true;
    return false;
}
static int compare_id(const void *a, const void *b) {
    const CeCitationCacheEntry *const *x = a, *const *y = b;
    return strcmp((*x)->source_item_id, (*y)->source_item_id);
}
static int compare_number(const void *a, const void *b) {
    const CeCitationCacheEntry *const *x = a, *const *y = b;
    return ((*x)->number > (*y)->number) - ((*x)->number < (*y)->number);
}
static bool unique_entries(CeCitationCache *c, char *error, size_t capacity) {
    if (c->entry_count < 2)
        return true;
    const CeCitationCacheEntry **sorted = allocate(c, c->entry_count * sizeof *sorted);
    if (!sorted)
        return fail(error, capacity, "Out of memory validating cache identities");
    for (size_t i = 0; i < c->entry_count; ++i)
        sorted[i] = &c->entries[i];
    bool ok = true;
    qsort(sorted, c->entry_count, sizeof *sorted, compare_id);
    for (size_t i = 1; i < c->entry_count; ++i)
        if (!strcmp(sorted[i - 1]->source_item_id, sorted[i]->source_item_id))
            ok = false;
    qsort(sorted, c->entry_count, sizeof *sorted, compare_number);
    for (size_t i = 1; i < c->entry_count; ++i)
        if (sorted[i - 1]->number == sorted[i]->number)
            ok = false;
    release(c, sorted);
    return ok ? true : fail(error, capacity, "Duplicate bibliography cache identity or number");
}
static bool commit(CeCitationCache *candidate, CeCitationCache **out) {
    ce_citation_cache_free(*out);
    *out = candidate;
    return true;
}
bool ce_citation_cache_from_citation(const CeCitationCacheHeader *h, const char *id,
                                     const CeCslOutput *output, const CeCslOutputAllocator *allocator,
                                     CeCitationCache **out, char *error, size_t capacity) {
    if (error && capacity)
        *error = 0;
    if (!out)
        return fail(error, capacity, "Missing citation cache destination");
    CeCitationCache *c = create(h, CE_CITATION_CACHE_CITATION, allocator, error, capacity);
    if (!c)
        return false;
    c->citation_id = copy(c, id, 4096);
    c->citation = copy_output(c, output, error, capacity);
    if (!c->citation_id || !c->citation) {
        ce_citation_cache_free(c);
        return fail(error, capacity, "Unable to copy controlled citation cache");
    }
    return commit(c, out);
}
bool ce_citation_cache_from_bibliography(const CeCitationCacheHeader *h,
                                         const CeCslBibliographyOptions *options,
                                         const CeCitationCacheBibliographySource *sources, size_t count,
                                         const CeCslOutputAllocator *allocator, CeCitationCache **out,
                                         char *error, size_t capacity) {
    if (error && capacity)
        *error = 0;
    if (!out || !valid_options(options) || (count && !sources))
        return fail(error, capacity, "Invalid bibliography cache input");
    CeCitationCache *c = create(h, CE_CITATION_CACHE_BIBLIOGRAPHY, allocator, error, capacity);
    if (!c)
        return false;
    c->options = *options;
    if (!reserve_entries(c, count, error, capacity))
        goto bad;
    for (size_t i = 0; i < count; ++i) {
        const CeCitationCacheBibliographySource *s = &sources[i];
        CeCitationCacheEntry *e = &c->entries[i];
        if (!s->entry || !s->number || s->number > CE_CITATION_CACHE_MAX_ENTRIES ||
            !equal_options(options, ce_csl_bibliography_entry_options(s->entry))) {
            fail(error, capacity, "Invalid bibliography cache entry/options");
            goto bad;
        }
        e->source_item_id = copy(c, s->source_item_id, 4096);
        e->number = s->number;
        e->visible = s->visible;
        if (!e->source_item_id) {
            fail(error, capacity, "Invalid bibliography cache identity or allocation failure");
            goto bad;
        }
        size_t rows = ce_csl_bibliography_entry_row_count(s->entry);
        CeCitationCacheRow *r = reserve_rows(c, e, rows, error, capacity);
        if (rows && !r)
            goto bad;
        for (size_t j = 0; j < rows; ++j) {
            const CeCslBibliographyRow *original = ce_csl_bibliography_entry_row(s->entry, j);
            r[j].kind = original->kind;
            r[j].body = copy_output(c, original->body.output, error, capacity);
            if (!r[j].body)
                goto bad;
            if (original->kind == CE_CSL_BIBLIOGRAPHY_TWO_COLUMNS) {
                r[j].left = copy_output(c, original->left.output, error, capacity);
                if (!r[j].left)
                    goto bad;
            } else if (original->kind != CE_CSL_BIBLIOGRAPHY_FLOW &&
                       original->kind != CE_CSL_BIBLIOGRAPHY_BLOCK &&
                       original->kind != CE_CSL_BIBLIOGRAPHY_INDENT) {
                fail(error, capacity, "Unsupported bibliography cache row");
                goto bad;
            }
        }
        if (e->visible != entry_visible(e)) {
            fail(error, capacity, "Bibliography visibility contradicts controlled text");
            goto bad;
        }
    }
    if (!unique_entries(c, error, capacity))
        goto bad;
    return commit(c, out);
bad:
    ce_citation_cache_free(c);
    return false;
}
static const cJSON *field(const cJSON *object, const char *key) {
    return cJSON_GetObjectItemCaseSensitive(object, key);
}
static const char *string_field(const cJSON *object, const char *key) {
    const cJSON *v = field(object, key);
    return cJSON_IsString(v) ? v->valuestring : NULL;
}
static bool keys(const cJSON *object, const char *allowed) {
    if (!cJSON_IsObject(object))
        return false;
    for (const cJSON *v = object->child; v; v = v->next) {
        bool found = false;
        size_t n = strlen(v->string);
        for (const char *p = allowed; *p;) {
            const char *end = strchr(p, '|');
            size_t size = end ? (size_t)(end - p) : strlen(p);
            if (size == n && !memcmp(p, v->string, n))
                found = true;
            if (!end)
                break;
            p = end + 1;
        }
        if (!found)
            return false;
    }
    return true;
}
static bool unsigned_field(const cJSON *object, const char *key, uint32_t maximum, uint32_t *out) {
    const cJSON *v = field(object, key);
    if (!cJSON_IsNumber(v) || !(v->valuedouble >= 0 && v->valuedouble <= (double)maximum))
        return false;
    uint32_t n = (uint32_t)v->valuedouble;
    if ((double)n != v->valuedouble)
        return false;
    *out = n;
    return true;
}
static bool boolean_field(const cJSON *object, const char *key, bool *out) {
    const cJSON *v = field(object, key);
    if (!cJSON_IsBool(v))
        return false;
    *out = cJSON_IsTrue(v);
    return true;
}
static CeCslOutput *parse_output(CeCitationCache *c, const cJSON *array, char *error, size_t capacity) {
    if (!cJSON_IsArray(array)) {
        fail(error, capacity, "Cache runs must be an array");
        return NULL;
    }
    CeCslOutputAllocator a = {c, allocate, release};
    CeCslOutput *out = ce_csl_output_new(&a, error, capacity);
    if (!out)
        return NULL;
    for (const cJSON *v = array->child; v; v = v->next) {
        const char *text = string_field(v, "text"), *href = string_field(v, "href");
        uint32_t format;
        if (!keys(v, "text|format|href") || !text || !unsigned_field(v, "format", 255, &format) ||
            (field(v, "href") && !href)) {
            fail(error, capacity, "Invalid controlled cache run");
            goto bad;
        }
        if (!append_run(c, out, text, strlen(text), format, href, error, capacity))
            goto bad;
    }
    return out;
bad:
    ce_csl_output_free(out);
    return NULL;
}
static const char *alignment_name(CeCslBibliographyAlignment kind) {
    return kind == CE_CSL_BIBLIOGRAPHY_ALIGN_NONE    ? "none"
           : kind == CE_CSL_BIBLIOGRAPHY_ALIGN_FLUSH ? "flush"
                                                     : "margin";
}
static const char *row_name(CeCslBibliographyRowKind kind) {
    return kind == CE_CSL_BIBLIOGRAPHY_FLOW     ? "flow"
           : kind == CE_CSL_BIBLIOGRAPHY_BLOCK  ? "block"
           : kind == CE_CSL_BIBLIOGRAPHY_INDENT ? "indent"
                                                : "two-columns";
}
bool ce_citation_cache_decode(const char *json, size_t size, const CeCslOutputAllocator *allocator,
                              CeCitationCache **out, char *error, size_t capacity) {
    if (error && capacity)
        *error = 0;
    if (!out || size > CE_CITATION_CACHE_MAX_BYTES)
        return fail(error, capacity, "Invalid or oversized citation cache");
    cJSON *root = NULL;
    CeCitationCache *c = NULL;
    bool ok = false;
    bool exact_numbers = false;
    if (!ce_csl_json_parse_strict_with_numbers(json, size, &root, &exact_numbers, error, capacity))
        return false;
    if (!exact_numbers) {
        cJSON_Delete(root);
        return fail(error, capacity, "Cache numeric fields must be exact safe integers");
    }
    const char *kind = string_field(root, "kind");
    uint32_t version;
    const cJSON *style = field(root, "style");
    if (!unsigned_field(root, "version", UINT32_MAX, &version) || version != CE_CITATION_CACHE_VERSION ||
        !kind || (strcmp(kind, "citation") && strcmp(kind, "bibliography"))) {
        fail(error, capacity, "Unsupported citation cache version/kind");
        goto done;
    }
    bool citation = !strcmp(kind, "citation");
    if (!keys(root, citation ? "version|kind|processorProfile|style|sourceDigest|citationId|runs"
                             : "version|kind|processorProfile|style|sourceDigest|options|entries") ||
        !keys(style, "profile|id|sha256|commit|locale|localeCommit")) {
        fail(error, capacity, "Unknown or malformed cache fields");
        goto done;
    }
    CeCitationCacheHeader h = {string_field(root, "processorProfile"),
                               string_field(style, "profile"),
                               string_field(style, "id"),
                               string_field(style, "sha256"),
                               string_field(style, "commit"),
                               string_field(style, "locale"),
                               string_field(style, "localeCommit"),
                               string_field(root, "sourceDigest")};
    c = create(&h, citation ? CE_CITATION_CACHE_CITATION : CE_CITATION_CACHE_BIBLIOGRAPHY, allocator, error,
               capacity);
    if (!c)
        goto done;
    if (citation) {
        c->citation_id = copy(c, string_field(root, "citationId"), 4096);
        if (!c->citation_id) {
            fail(error, capacity, "Invalid cache citationId or allocation failure");
            goto done;
        }
        c->citation = parse_output(c, field(root, "runs"), error, capacity);
        if (!c->citation)
            goto done;
    } else {
        const cJSON *o = field(root, "options"), *entries = field(root, "entries");
        const char *align = string_field(o, "alignment");
        if (!keys(o, "alignment|hangingIndent|lineSpacing|entrySpacing") || !align ||
            !boolean_field(o, "hangingIndent", &c->options.hanging_indent) ||
            !unsigned_field(o, "lineSpacing", UINT32_MAX, &c->options.line_spacing) ||
            !c->options.line_spacing ||
            !unsigned_field(o, "entrySpacing", UINT32_MAX, &c->options.entry_spacing) ||
            !cJSON_IsArray(entries)) {
            fail(error, capacity, "Invalid bibliography cache options/entries");
            goto done;
        }
        if (!strcmp(align, "none"))
            c->options.alignment = CE_CSL_BIBLIOGRAPHY_ALIGN_NONE;
        else if (!strcmp(align, "flush"))
            c->options.alignment = CE_CSL_BIBLIOGRAPHY_ALIGN_FLUSH;
        else if (!strcmp(align, "margin"))
            c->options.alignment = CE_CSL_BIBLIOGRAPHY_ALIGN_MARGIN;
        else {
            fail(error, capacity, "Unknown bibliography alignment");
            goto done;
        }
        size_t count = (size_t)cJSON_GetArraySize(entries);
        if (!reserve_entries(c, count, error, capacity))
            goto done;
        const cJSON *v = entries->child;
        for (size_t i = 0; i < count; ++i, v = v->next) {
            CeCitationCacheEntry *e = &c->entries[i];
            const cJSON *rows = field(v, "rows");
            uint32_t number;
            if (!keys(v, "sourceItemId|number|visible|rows") ||
                !unsigned_field(v, "number", (uint32_t)CE_CITATION_CACHE_MAX_ENTRIES, &number) || !number ||
                !boolean_field(v, "visible", &e->visible) || !cJSON_IsArray(rows)) {
                fail(error, capacity, "Invalid bibliography cache entry");
                goto done;
            }
            e->number = number;
            e->source_item_id = copy(c, string_field(v, "sourceItemId"), 4096);
            if (!e->source_item_id) {
                fail(error, capacity, "Invalid bibliography source identity or allocation failure");
                goto done;
            }
            size_t n = (size_t)cJSON_GetArraySize(rows);
            CeCitationCacheRow *r = reserve_rows(c, e, n, error, capacity);
            if (n && !r)
                goto done;
            const cJSON *original = rows->child;
            for (size_t j = 0; j < n; ++j, original = original->next) {
                const char *name = string_field(original, "kind");
                if (!keys(original, "kind|left|body") || !name) {
                    fail(error, capacity, "Invalid bibliography row");
                    goto done;
                }
                if (!strcmp(name, "flow"))
                    r[j].kind = CE_CSL_BIBLIOGRAPHY_FLOW;
                else if (!strcmp(name, "block"))
                    r[j].kind = CE_CSL_BIBLIOGRAPHY_BLOCK;
                else if (!strcmp(name, "indent"))
                    r[j].kind = CE_CSL_BIBLIOGRAPHY_INDENT;
                else if (!strcmp(name, "two-columns"))
                    r[j].kind = CE_CSL_BIBLIOGRAPHY_TWO_COLUMNS;
                else {
                    fail(error, capacity, "Unsupported bibliography row kind");
                    goto done;
                }
                r[j].body = parse_output(c, field(original, "body"), error, capacity);
                if (!r[j].body)
                    goto done;
                if (r[j].kind == CE_CSL_BIBLIOGRAPHY_TWO_COLUMNS) {
                    r[j].left = parse_output(c, field(original, "left"), error, capacity);
                    if (!r[j].left)
                        goto done;
                } else if (field(original, "left")) {
                    fail(error, capacity, "Only two-column rows may contain left cells");
                    goto done;
                }
            }
            if (e->visible != entry_visible(e)) {
                fail(error, capacity, "Cached visibility contradicts row text");
                goto done;
            }
        }
        if (!unique_entries(c, error, capacity))
            goto done;
    }
    commit(c, out);
    c = NULL;
    ok = true;
done:
    ce_citation_cache_free(c);
    cJSON_Delete(root);
    return ok;
}
static bool put(cJSON *object, const char *key, cJSON *value) {
    if (!value)
        return false;
    if (!object || !cJSON_AddItemToObject(object, key, value)) {
        cJSON_Delete(value);
        return false;
    }
    return true;
}
static bool put_string(cJSON *object, const char *key, const char *value) {
    return value && put(object, key, cJSON_CreateString(value));
}
static bool add(cJSON *array, cJSON *value) {
    if (!value)
        return false;
    if (!array || !cJSON_AddItemToArray(array, value)) {
        cJSON_Delete(value);
        return false;
    }
    return true;
}
static cJSON *output_json(const CeCslOutput *out) {
    cJSON *a = cJSON_CreateArray();
    if (!a)
        return NULL;
    for (size_t i = 0; i < ce_csl_output_run_count(out); ++i) {
        const CeCslOutputRun *r = ce_csl_output_run(out, i);
        cJSON *v = cJSON_CreateObject();
        if (!v || !put_string(v, "text", r->text) || !put(v, "format", cJSON_CreateNumber(r->format)) ||
            (r->href && !put_string(v, "href", r->href))) {
            cJSON_Delete(v);
            cJSON_Delete(a);
            return NULL;
        }
        if (!add(a, v)) {
            cJSON_Delete(a);
            return NULL;
        }
    }
    return a;
}
static bool json_budget(const cJSON *node, size_t *count) {
    if (++*count > 250000)
        return false;
    for (const cJSON *c = node->child; c; c = c->next)
        if (!json_budget(c, count))
            return false;
    return true;
}
bool ce_citation_cache_encode(const CeCitationCache *c, CeCslOutputString *out, char *error,
                              size_t capacity) {
    if (error && capacity)
        *error = 0;
    if (!c || !out)
        return fail(error, capacity, "Missing citation cache/output");
    cJSON *root = cJSON_CreateObject(), *style = cJSON_CreateObject();
    char *printed = NULL, *owned = NULL;
    bool ok = false;
    if (!root || !style) {
        cJSON_Delete(style);
        goto done;
    }
    if (!put(root, "style", style))
        goto done;
    if (!put(root, "version", cJSON_CreateNumber(CE_CITATION_CACHE_VERSION)) ||
        !put_string(root, "kind", c->kind == CE_CITATION_CACHE_CITATION ? "citation" : "bibliography") ||
        !put_string(root, "processorProfile", c->header.processor_profile) ||
        !put_string(root, "sourceDigest", c->header.source_digest) ||
        !put_string(style, "profile", c->header.profile) || !put_string(style, "id", c->header.style_id) ||
        !put_string(style, "sha256", c->header.style_sha256) ||
        !put_string(style, "commit", c->header.style_commit) ||
        !put_string(style, "locale", c->header.locale) ||
        !put_string(style, "localeCommit", c->header.locale_commit))
        goto done;
    if (c->kind == CE_CITATION_CACHE_CITATION) {
        if (!put_string(root, "citationId", c->citation_id) || !put(root, "runs", output_json(c->citation)))
            goto done;
    } else {
        cJSON *options = cJSON_CreateObject(), *entries = cJSON_CreateArray();
        if (!options || !entries) {
            cJSON_Delete(options);
            cJSON_Delete(entries);
            goto done;
        }
        if (!put(root, "options", options)) {
            cJSON_Delete(entries);
            goto done;
        }
        if (!put(root, "entries", entries))
            goto done;
        if (!put_string(options, "alignment", alignment_name(c->options.alignment)) ||
            !put(options, "hangingIndent", cJSON_CreateBool(c->options.hanging_indent)) ||
            !put(options, "lineSpacing", cJSON_CreateNumber(c->options.line_spacing)) ||
            !put(options, "entrySpacing", cJSON_CreateNumber(c->options.entry_spacing)))
            goto done;
        for (size_t i = 0; i < c->entry_count; ++i) {
            const CeCitationCacheEntry *e = &c->entries[i];
            cJSON *entry = cJSON_CreateObject(), *rows = cJSON_CreateArray();
            if (!entry || !rows) {
                cJSON_Delete(entry);
                cJSON_Delete(rows);
                goto done;
            }
            if (!put(entry, "rows", rows)) {
                cJSON_Delete(entry);
                goto done;
            }
            if (!add(entries, entry))
                goto done;
            if (!put_string(entry, "sourceItemId", e->source_item_id) ||
                !put(entry, "number", cJSON_CreateNumber((double)e->number)) ||
                !put(entry, "visible", cJSON_CreateBool(e->visible)))
                goto done;
            for (size_t r = 0; r < e->row_count; ++r) {
                const CeCitationCacheRow *row = &e->rows[r];
                cJSON *record = cJSON_CreateObject();
                if (!record)
                    goto done;
                if (!add(rows, record))
                    goto done;
                if (!put_string(record, "kind", row_name(row->kind)) ||
                    !put(record, "body", output_json(row->body)) ||
                    (row->left && !put(record, "left", output_json(row->left))))
                    goto done;
            }
        }
    }
    size_t nodes = 0;
    if (!json_budget(root, &nodes)) {
        fail(error, capacity, "Encoded cache exceeds strict JSON node limit");
        goto done;
    }
    printed = ce_csl_json_print_exact(root);
    if (!printed)
        goto done;
    size_t size = strlen(printed);
    if (size > CE_CITATION_CACHE_MAX_BYTES) {
        fail(error, capacity, "Encoded citation cache exceeds byte limit");
        goto done;
    }
    owned = malloc(size + 1);
    if (!owned)
        goto done;
    memcpy(owned, printed, size + 1);
    ce_csl_output_string_free(out);
    *out = (CeCslOutputString){owned, size, normal_free, NULL};
    owned = NULL;
    ok = true;
done:
    cJSON_Delete(root);
    cJSON_free(printed);
    free(owned);
    if (!ok && (!error || !capacity || !*error))
        fail(error, capacity, "Unable to encode controlled citation cache");
    return ok;
}
