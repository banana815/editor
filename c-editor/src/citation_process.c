#include "citation_process.h"
#include "citation_digest.h"
#include "csl_document.h"
#include "csl_json.h"
#include <inttypes.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CITATION "zoteroCitationBlock"
#define BIBLIOGRAPHY "zoteroEndnoteSection"
#define SOURCE_LIMIT CE_CSL_INPUT_MAX_BYTES
#define LIBRARY_KEY_LIMIT ((size_t)64 * 1024 * 1024)
typedef struct Nodes {
    cJSON **data;
    size_t count, capacity;
} Nodes;
typedef struct Record {
    cJSON *node, *attrs, *item;
    const char *id, *snapshot, *page;
    char *identity;
    size_t ordinal;
    bool repair, numbers_exact;
} Record;
typedef struct LibraryKey {
    char *key;
    const CeZoteroItem *item;
} LibraryKey;
typedef struct Source {
    cJSON *root;
    Nodes citations, sections;
    Record *records;
    Record **by_identity;
    size_t count, repaired;
    CeCslInput *input;
    char digest[65];
    LibraryKey *library_keys;
    size_t library_count, library_capacity, library_bytes;
} Source;
struct CeCitationPrepared {
    char *json;
    char digest[65];
    CeCitationProcessStats stats;
    CeCitationCache **citations, **sorted;
    size_t count;
    CeCitationCache *bibliography;
    size_t generated_bytes;
};
static bool fail(char *error, size_t cap, const char *message) {
    if (error && cap)
        snprintf(error, cap, "%s", message);
    return false;
}
static const cJSON *field(const cJSON *o, const char *k) {
    return cJSON_GetObjectItemCaseSensitive(o, k);
}
static const char *string(const cJSON *o, const char *k) {
    const cJSON *v = field(o, k);
    return cJSON_IsString(v) ? v->valuestring : NULL;
}
static const char *text(const cJSON *o, const char *k) {
    const char *s = string(o, k);
    return s ? s : "";
}
static bool type_is(const cJSON *n, const char *t) {
    return !strcmp(text(n, "type"), t);
}
static char *copy(const char *s) {
    size_t n = strlen(s) + 1;
    char *p = malloc(n);
    if (p)
        memcpy(p, s, n);
    return p;
}
static bool put(cJSON *o, const char *key, cJSON *value) {
    if (!value)
        return false;
    bool ok = o && (field(o, key) ? cJSON_ReplaceItemInObjectCaseSensitive(o, key, value)
                                  : cJSON_AddItemToObject(o, key, value));
    if (!ok)
        cJSON_Delete(value);
    return ok;
}
static bool put_text(cJSON *o, const char *key, const char *s) {
    return put(o, key, cJSON_CreateString(s));
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
static char *serialize(const cJSON *o) {
    char *s = ce_csl_json_print_exact(o);
    char *result = s ? copy(s) : NULL;
    cJSON_free(s);
    return result;
}
static bool grow(void **data, size_t *capacity, size_t count, size_t bytes) {
    if (count <= *capacity)
        return true;
    size_t next = *capacity ? *capacity * 2 : 16;
    if (next < count)
        next = count;
    if (next > SIZE_MAX / bytes)
        return false;
    void *p = realloc(*data, next * bytes);
    if (!p)
        return false;
    *data = p;
    *capacity = next;
    return true;
}
static bool collect(cJSON *node, Source *s, unsigned depth) {
    if (depth > 128)
        return false;
    Nodes *target = type_is(node, CITATION)       ? &s->citations
                    : type_is(node, BIBLIOGRAPHY) ? &s->sections
                                                  : NULL;
    if (target) {
        if (target->count == CE_CSL_INPUT_MAX_CLUSTERS ||
            !grow((void **)&target->data, &target->capacity, target->count + 1, sizeof *target->data))
            return false;
        target->data[target->count++] = node;
        return true;
    }
    if (type_is(node, "paragraph") || type_is(node, "heading"))
        return true;
    cJSON *content = cJSON_GetObjectItemCaseSensitive(node, "content");
    for (cJSON *child = content ? content->child : NULL; child; child = child->next)
        if (!collect(child, s, depth + 1))
            return false;
    return true;
}
static cJSON *attributes(cJSON *node) {
    cJSON *a = cJSON_GetObjectItemCaseSensitive(node, "attrs");
    if (!a) {
        if (!put(node, "attrs", cJSON_CreateObject()))
            return NULL;
        a = cJSON_GetObjectItemCaseSensitive(node, "attrs");
    }
    return cJSON_IsObject(a) ? a : NULL;
}
static bool valid_id(const char *id) {
    if (!id || !*id || strlen(id) > 4096)
        return false;
    for (const unsigned char *p = (const unsigned char *)id; *p; ++p)
        if (*p < 32 || *p == 127)
            return false;
    return true;
}
static int compare_strings(const void *a, const void *b) {
    return strcmp(*(char *const *)a, *(char *const *)b);
}
static int compare_original_id(const void *a, const void *b) {
    const Record *const *x = a, *const *y = b;
    int order = strcmp((*x)->id, (*y)->id);
    return order ? order : (((*x)->ordinal > (*y)->ordinal) - ((*x)->ordinal < (*y)->ordinal));
}
static bool prepare_ids(Source *s, char *error, size_t capacity) {
    size_t n = s->count;
    Record **sorted = n ? malloc(n * sizeof *sorted) : NULL;
    char **reserved = n ? calloc(n, sizeof *reserved) : NULL;
    bool ok = false;
    size_t reserved_count = 0;
    if (n && (!sorted || !reserved))
        goto done;
    for (size_t i = 0; i < n; ++i) {
        Record *r = &s->records[i];
        r->node = s->citations.data[i];
        r->attrs = attributes(r->node);
        r->ordinal = i;
        if (!r->attrs) {
            fail(error, capacity, "Citation attributes must be an object");
            goto done;
        }
        const cJSON *id = field(r->attrs, "citationId");
        if (id && !cJSON_IsString(id)) {
            fail(error, capacity, "citationId must be a string");
            goto done;
        }
        r->id = text(r->attrs, "citationId");
        r->repair = !*r->id;
        sorted[i] = r;
        if (*r->id) {
            if (!valid_id(r->id)) {
                fail(error, capacity, "Invalid citationId");
                goto done;
            }
            reserved[reserved_count] = copy(r->id);
            if (!reserved[reserved_count++])
                goto done;
        }
    }
    if (n > 1)
        qsort(sorted, n, sizeof *sorted, compare_original_id);
    if (reserved_count > 1)
        qsort(reserved, reserved_count, sizeof *reserved, compare_strings);
    for (size_t i = 1; i < n; ++i)
        if (*sorted[i]->id && !strcmp(sorted[i - 1]->id, sorted[i]->id))
            sorted[i]->repair = true;
    size_t sequence = 0;
    for (size_t i = 0; i < n; ++i)
        if (s->records[i].repair) {
            char id[64];
            char *key = id;
            do {
                snprintf(id, sizeof id, "cite-c-%zu", ++sequence);
            } while (reserved_count &&
                     bsearch(&key, reserved, reserved_count, sizeof *reserved, compare_strings));
            if (!put_text(s->records[i].attrs, "citationId", id))
                goto done;
            s->records[i].id = text(s->records[i].attrs, "citationId");
            ++s->repaired;
        }
    ok = true;
done:
    for (size_t i = 0; i < reserved_count; ++i)
        free(reserved[i]);
    free(reserved);
    free(sorted);
    if (!ok && (!error || !capacity || !*error))
        fail(error, capacity, "Unable to prepare stable citation IDs");
    return ok;
}
static char *key(const char *tag, const char *scope, const char *value) {
    if (!value || !*value || strlen(value) > 4096 || (scope && strlen(scope) > 4096))
        return NULL;
    size_t n = strlen(tag) + strlen(value) + (scope ? strlen(scope) : 0) + 64;
    char *result = malloc(n);
    if (!result)
        return NULL;
    if (scope)
        snprintf(result, n, "%s:%zu:%s:%s", tag, strlen(scope), scope, value);
    else
        snprintf(result, n, "%s:%s", tag, value);
    if (!valid_id(result)) {
        free(result);
        return NULL;
    }
    return result;
}
static bool scalar(const cJSON *v, char **out) {
    *out = NULL;
    if (!v || cJSON_IsNull(v))
        return true;
    if (cJSON_IsString(v)) {
        if (*v->valuestring)
            *out = copy(v->valuestring);
        return !*v->valuestring || *out;
    }
    if (!cJSON_IsNumber(v) || !isfinite(v->valuedouble) || v->valuedouble < -9007199254740991.0 ||
        v->valuedouble > 9007199254740991.0)
        return false;
    int64_t integer = (int64_t)v->valuedouble;
    if ((double)integer != v->valuedouble)
        return false;
    char number[32];
    snprintf(number, sizeof number, "%" PRId64, integer);
    *out = copy(number);
    return *out != NULL;
}
static char *identity(const cJSON *attrs) {
    const char *keys[] = {"itemUri", "itemKey", "citeKey"};
    for (size_t i = 0; i < 3; ++i)
        if (field(attrs, keys[i]) && !cJSON_IsString(field(attrs, keys[i])) &&
            !cJSON_IsNull(field(attrs, keys[i])))
            return NULL;
    const char *uri = text(attrs, "itemUri");
    if (*uri)
        return key("uri", NULL, uri);
    char *source = NULL, *library = NULL, *result = NULL;
    if (!scalar(field(attrs, "sourceId"), &source) || !scalar(field(attrs, "libraryId"), &library))
        goto done;
    if (source)
        result = key(library ? "library-source" : "source", library, source);
    else if (*text(attrs, "itemKey"))
        result = key(library ? "library" : "key", library, text(attrs, "itemKey"));
    else if (*text(attrs, "citeKey"))
        result = key("cite", NULL, text(attrs, "citeKey"));
    else
        result = key("id", NULL, text(attrs, "citationId"));
done:
    free(source);
    free(library);
    return result;
}
static bool library_alias(Source *s, const CeZoteroItem *item, const char *tag, const char *scope,
                          const char *value) {
    if (!value || !*value)
        return true;
    char *id = key(tag, scope, value);
    if (!id)
        return false;
    size_t n = strlen(id) + 1;
    if (n > LIBRARY_KEY_LIMIT - s->library_bytes || s->library_count == CE_CSL_INPUT_MAX_ITEMS * 6 ||
        !grow((void **)&s->library_keys, &s->library_capacity, s->library_count + 1,
              sizeof *s->library_keys)) {
        free(id);
        return false;
    }
    s->library_keys[s->library_count++] = (LibraryKey){id, item};
    s->library_bytes += n;
    return true;
}
static int compare_library(const void *a, const void *b) {
    return strcmp(((const LibraryKey *)a)->key, ((const LibraryKey *)b)->key);
}
static bool index_library(Source *s, const CeZoteroLibrary *library) {
    if (!library)
        return true;
    if (library->item_count > CE_CSL_INPUT_MAX_ITEMS || (library->item_count && !library->items))
        return false;
    for (size_t i = 0; i < library->item_count; ++i) {
        const CeZoteroItem *item = &library->items[i];
        char scope[32];
        const char *qualified = NULL;
        if (item->library_id > 0) {
            snprintf(scope, sizeof scope, "%" PRId64, item->library_id);
            qualified = scope;
        }
        if (!library_alias(s, item, "uri", NULL, item->item_uri) ||
            !library_alias(s, item, "source", NULL, item->id) ||
            (qualified && !library_alias(s, item, "library-source", qualified, item->id)) ||
            !library_alias(s, item, "key", NULL, item->item_key) ||
            (qualified && !library_alias(s, item, "library", qualified, item->item_key)) ||
            !library_alias(s, item, "cite", NULL, item->cite_key))
            return false;
    }
    if (s->library_count > 1)
        qsort(s->library_keys, s->library_count, sizeof *s->library_keys, compare_library);
    return true;
}
static const CeZoteroItem *find_library(const Source *s, const char *identity_key, bool *ambiguous) {
    size_t low = 0, high = s->library_count;
    *ambiguous = false;
    while (low < high) {
        size_t mid = low + (high - low) / 2;
        if (strcmp(s->library_keys[mid].key, identity_key) < 0)
            low = mid + 1;
        else
            high = mid;
    }
    if (low == s->library_count || strcmp(s->library_keys[low].key, identity_key))
        return NULL;
    if (low + 1 < s->library_count && !strcmp(s->library_keys[low + 1].key, identity_key)) {
        *ambiguous = true;
        return NULL;
    }
    return s->library_keys[low].item;
}
static int compare_identity(const void *a, const void *b) {
    const Record *const *x = a, *const *y = b;
    int order = strcmp((*x)->identity, (*y)->identity);
    return order ? order : (((*x)->ordinal > (*y)->ordinal) - ((*x)->ordinal < (*y)->ordinal));
}
static int compare_member(const void *a, const void *b) {
    const cJSON *const *x = a, *const *y = b;
    return strcmp((*x)->string, (*y)->string);
}
/* cJSON_Compare uses relative numeric tolerance, which can equate distinct
 * large SAFE integers. Here lexeme validation has already established exact
 * integer representation; compare their doubles directly, with no epsilon.
 * Sorted object members also avoid quadratic lookups for wide metadata. */
static bool metadata_equal(const cJSON *a, const cJSON *b, bool *equal, char *error, size_t capacity) {
    if (cJSON_IsNumber(a) || cJSON_IsNumber(b)) {
        *equal = cJSON_IsNumber(a) && cJSON_IsNumber(b) && a->valuedouble == b->valuedouble;
        return true;
    }
    if (cJSON_IsString(a) || cJSON_IsString(b)) {
        *equal = cJSON_IsString(a) && cJSON_IsString(b) && !strcmp(a->valuestring, b->valuestring);
        return true;
    }
    if (cJSON_IsBool(a) || cJSON_IsBool(b)) {
        *equal = cJSON_IsBool(a) && cJSON_IsBool(b) && cJSON_IsTrue(a) == cJSON_IsTrue(b);
        return true;
    }
    if (cJSON_IsNull(a) || cJSON_IsNull(b)) {
        *equal = cJSON_IsNull(a) && cJSON_IsNull(b);
        return true;
    }
    bool object = cJSON_IsObject(a);
    if ((object && !cJSON_IsObject(b)) || (!object && (!cJSON_IsArray(a) || !cJSON_IsArray(b)))) {
        *equal = false;
        return true;
    }
    size_t count = 0, other = 0;
    for (const cJSON *v = a->child; v; v = v->next)
        ++count;
    for (const cJSON *v = b->child; v; v = v->next)
        ++other;
    if (count != other) {
        *equal = false;
        return true;
    }
    if (!count) {
        *equal = true;
        return true;
    }
    if (!object) {
        const cJSON *x = a->child, *y = b->child;
        for (; x; x = x->next, y = y->next) {
            bool same = false;
            if (!metadata_equal(x, y, &same, error, capacity))
                return false;
            if (!same) {
                *equal = false;
                return true;
            }
        }
        *equal = true;
        return true;
    }
    const cJSON **x = malloc(count * sizeof *x), **y = malloc(count * sizeof *y);
    if (!x || !y) {
        free(x);
        free(y);
        return fail(error, capacity, "Out of memory comparing exact CSL metadata");
    }
    size_t at = 0;
    for (const cJSON *v = a->child; v; v = v->next)
        x[at++] = v;
    at = 0;
    for (const cJSON *v = b->child; v; v = v->next)
        y[at++] = v;
    if (count > 1) {
        qsort(x, count, sizeof *x, compare_member);
        qsort(y, count, sizeof *y, compare_member);
    }
    bool ok = true, same = true;
    for (size_t i = 0; i < count; ++i) {
        if (strcmp(x[i]->string, y[i]->string)) {
            same = false;
            break;
        }
        if (!metadata_equal(x[i], y[i], &same, error, capacity)) {
            ok = false;
            break;
        }
        if (!same)
            break;
    }
    free(x);
    free(y);
    if (ok)
        *equal = same;
    return ok;
}
static bool source_items(Source *s, const CeZoteroLibrary *library, cJSON *items, char *error,
                         size_t capacity) {
    bool indexed = false;
    Record **sorted = s->count ? malloc(s->count * sizeof *sorted) : NULL;
    bool ok = false;
    s->by_identity = sorted;
    if (s->count && !sorted)
        return fail(error, capacity, "Out of memory preparing CSL snapshots");
    for (size_t i = 0; i < s->count; ++i) {
        Record *r = &s->records[i];
        sorted[i] = r;
        r->identity = identity(r->attrs);
        if (!r->identity) {
            fail(error, capacity, "Invalid/unsafe citation identity or allocation failure");
            goto done;
        }
        const cJSON *snapshot = field(r->attrs, "cslJson");
        if (snapshot && !cJSON_IsString(snapshot)) {
            fail(error, capacity, "cslJson must contain a complete CSL object string");
            goto done;
        }
        r->snapshot = text(r->attrs, "cslJson");
        if (!*r->snapshot) {
            if (!indexed) {
                if (!index_library(s, library)) {
                    fail(error, capacity, "Unable to index fallback library within limits");
                    goto done;
                }
                indexed = true;
            }
            bool ambiguous = false;
            const CeZoteroItem *item = find_library(s, r->identity, &ambiguous);
            if (!item || !item->csl_json || !*item->csl_json) {
                fail(error, capacity,
                     ambiguous ? "Citation source matches multiple library items"
                               : "Citation lacks a complete CSL snapshot and a unique library match");
                goto done;
            }
            if (!put_text(r->attrs, "cslJson", item->csl_json))
                goto done;
            r->snapshot = text(r->attrs, "cslJson");
        }
        if (!ce_csl_json_parse_strict_with_numbers(r->snapshot, strlen(r->snapshot), &r->item,
                                                   &r->numbers_exact, error, capacity))
            goto done;
        if (!cJSON_IsObject(r->item)) {
            fail(error, capacity, "Embedded CSL snapshot must be an object");
            goto done;
        }
        cJSON_DeleteItemFromObjectCaseSensitive(r->item, "id");
        const cJSON *page = field(r->attrs, "page");
        if (page && !cJSON_IsString(page) && !cJSON_IsNull(page)) {
            fail(error, capacity, "Citation page locator must be a string");
            goto done;
        }
        r->page = text(r->attrs, "page");
    }
    if (s->count > 1)
        qsort(sorted, s->count, sizeof *sorted, compare_identity);
    for (size_t begin = 0; begin < s->count;) {
        size_t end = begin + 1;
        while (end < s->count && !strcmp(sorted[begin]->identity, sorted[end]->identity))
            ++end;
        Record *first = sorted[begin];
        for (size_t i = begin + 1; i < end; ++i) {
            Record *other = sorted[i];
            if (strcmp(first->snapshot, other->snapshot)) {
                bool equal = false;
                if (first->numbers_exact && other->numbers_exact &&
                    !metadata_equal(first->item, other->item, &equal, error, capacity))
                    goto done;
                if (!first->numbers_exact || !other->numbers_exact || !equal) {
                    fail(error, capacity,
                         "Conflicting or not losslessly comparable CSL snapshots for one source identity");
                    goto done;
                }
            }
        }
        if (!put_text(first->item, "id", first->identity))
            goto done;
        cJSON *item = first->item;
        first->item = NULL;
        if (!add(items, item))
            goto done;
        for (size_t i = begin + 1; i < end; ++i) {
            cJSON_Delete(sorted[i]->item);
            sorted[i]->item = NULL;
        }
        begin = end;
    }
    ok = true;
done:
    if (!ok && (!error || !capacity || !*error))
        fail(error, capacity, "Unable to build complete CSL items");
    return ok;
}
static bool digest_source(Source *s, const CeCitationStyle *style, char *error, size_t capacity) {
    const CeCitationStyleInfo *info = ce_citation_style_loaded_info(style);
    const CeCslLocale *locale = ce_citation_style_locale(style);
    if (!info || !locale)
        return fail(error, capacity, "A loaded citation profile is required");
    CeCitationDigest digest;
    ce_citation_digest_init(&digest);
    const char *fields[] = {"notelab-citation-source/v1",
                            CE_CITATION_PROCESSOR_PROFILE,
                            info->profile_id,
                            info->style_id,
                            info->style_sha256,
                            info->style_commit,
                            ce_csl_locale_language(locale),
                            info->locale_commit};
    for (size_t i = 0; i < sizeof fields / sizeof *fields; ++i)
        if (!ce_citation_digest_field(&digest, fields[i], strlen(fields[i]), error, capacity))
            return false;
    char number[32];
    snprintf(number, sizeof number, "%zu", s->count);
    if (!ce_citation_digest_field(&digest, number, strlen(number), error, capacity))
        return false;
    for (size_t i = 0; i < s->count; ++i) {
        const Record *r = &s->records[i];
        const char *values[] = {r->id, r->identity, r->snapshot, r->page};
        for (size_t j = 0; j < 4; ++j)
            if (!ce_citation_digest_field(&digest, values[j], strlen(values[j]), error, capacity))
                return false;
    }
    return ce_citation_digest_final_hex(&digest, s->digest, error, capacity);
}
static void source_free(Source *s) {
    if (s->records)
        for (size_t i = 0; i < s->count; ++i) {
            free(s->records[i].identity);
            cJSON_Delete(s->records[i].item);
        }
    for (size_t i = 0; i < s->library_count; ++i)
        free(s->library_keys[i].key);
    free(s->library_keys);
    free(s->records);
    free(s->by_identity);
    free(s->citations.data);
    free(s->sections.data);
    ce_csl_input_free(s->input);
    cJSON_Delete(s->root);
    memset(s, 0, sizeof *s);
}
static bool has_atoms(const CeDocument *doc) {
    for (size_t i = 0; i < ce_document_block_count(doc); ++i) {
        CeBlockInfo block;
        if (ce_document_block_info(doc, i, &block) &&
            (block.kind == CE_BLOCK_CITATION || block.kind == CE_BLOCK_ENDNOTES))
            return true;
    }
    return false;
}
static bool source_build(const CeDocument *doc, const CeCitationStyle *style, const CeZoteroLibrary *library,
                         bool force_tree, Source *s, char *error, size_t capacity) {
    if (!doc || !ce_citation_style_loaded_info(style))
        return fail(error, capacity, "Document and loaded citation profile are required");
    if (!force_tree && !has_atoms(doc))
        return digest_source(s, style, error, capacity);
    char *json = ce_document_export_json(doc);
    if (!json)
        return fail(error, capacity, "Unable to copy citation document");
    bool parsed = ce_csl_json_parse_strict(json, strlen(json), &s->root, error, capacity);
    free(json);
    if (!parsed)
        return false;
    if (!type_is(s->root, "doc") || !collect(s->root, s, 0))
        return fail(error, capacity, "Unable to traverse citation document within limits");
    s->count = s->citations.count;
    if (s->count) {
        s->records = calloc(s->count, sizeof *s->records);
        if (!s->records)
            return fail(error, capacity, "Out of memory collecting citations");
    }
    if (!prepare_ids(s, error, capacity))
        return false;
    cJSON *envelope = cJSON_CreateObject(), *items = cJSON_CreateArray(), *clusters = cJSON_CreateArray();
    bool ok = false;
    if (!envelope || !items || !clusters) {
        cJSON_Delete(items);
        cJSON_Delete(clusters);
        goto done;
    }
    if (!put(envelope, "items", items)) {
        cJSON_Delete(clusters);
        goto done;
    }
    if (!put(envelope, "citations", clusters))
        goto done;
    if (!source_items(s, library, items, error, capacity))
        goto done;
    for (size_t i = 0; i < s->count; ++i) {
        const Record *r = &s->records[i];
        cJSON *cluster = cJSON_CreateObject(), *cites = cJSON_CreateArray(), *cite = cJSON_CreateObject();
        if (!cluster || !cites || !cite) {
            cJSON_Delete(cluster);
            cJSON_Delete(cites);
            cJSON_Delete(cite);
            goto done;
        }
        if (!put(cluster, "citationItems", cites)) {
            cJSON_Delete(cluster);
            cJSON_Delete(cite);
            goto done;
        }
        if (!add(clusters, cluster)) {
            cJSON_Delete(cite);
            goto done;
        }
        if (!add(cites, cite))
            goto done;
        if (!put_text(cluster, "citationID", r->id) || !put_text(cite, "id", r->identity) ||
            (*r->page && (!put_text(cite, "locator", r->page) || !put_text(cite, "label", "page"))))
            goto done;
    }
    json = serialize(envelope);
    if (!json)
        goto done;
    ok = ce_csl_input_load(&s->input, json, strlen(json), error, capacity);
    free(json);
    if (ok)
        ok = digest_source(s, style, error, capacity);
done:
    cJSON_Delete(envelope);
    if (!ok && (!error || !capacity || !*error))
        fail(error, capacity, "Unable to construct citation input envelope");
    return ok;
}
bool ce_citations_source_digest(const CeDocument *doc, const CeCitationStyle *style,
                                const CeZoteroLibrary *library, char digest[65], char *error,
                                size_t capacity) {
    if (error && capacity)
        *error = 0;
    if (!digest)
        return fail(error, capacity, "Missing citation source digest destination");
    Source source = {0};
    bool ok = source_build(doc, style, library, false, &source, error, capacity);
    if (ok)
        memcpy(digest, source.digest, 65);
    source_free(&source);
    return ok;
}
void ce_citation_prepared_free(CeCitationPrepared *p) {
    if (!p)
        return;
    if (p->citations)
        for (size_t i = 0; i < p->count; ++i)
            ce_citation_cache_free(p->citations[i]);
    free(p->citations);
    free(p->sorted);
    ce_citation_cache_free(p->bibliography);
    free(p->json);
    free(p);
}
const char *ce_citation_prepared_json(const CeCitationPrepared *p) {
    return p ? p->json : NULL;
}
const char *ce_citation_prepared_digest(const CeCitationPrepared *p) {
    return p ? p->digest : NULL;
}
const CeCitationProcessStats *ce_citation_prepared_stats(const CeCitationPrepared *p) {
    return p ? &p->stats : NULL;
}
const CeCitationCache *ce_citation_prepared_bibliography(const CeCitationPrepared *p) {
    return p ? p->bibliography : NULL;
}
static int compare_cache(const void *a, const void *b) {
    const CeCitationCache *const *x = a, *const *y = b;
    return strcmp(ce_citation_cache_citation_id(*x), ce_citation_cache_citation_id(*y));
}
const CeCitationCache *ce_citation_prepared_citation(const CeCitationPrepared *p, const char *id) {
    if (!p || !id)
        return NULL;
    size_t low = 0, high = p->count;
    while (low < high) {
        size_t mid = low + (high - low) / 2;
        int c = strcmp(ce_citation_cache_citation_id(p->sorted[mid]), id);
        if (c < 0)
            low = mid + 1;
        else
            high = mid;
    }
    return low < p->count && !strcmp(ce_citation_cache_citation_id(p->sorted[low]), id) ? p->sorted[low]
                                                                                        : NULL;
}
static bool supported_keys(const cJSON *o, const char *allowed) {
    if (!o)
        return true;
    if (!cJSON_IsObject(o))
        return false;
    for (const cJSON *v = o->child; v; v = v->next) {
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
static bool ordinary_entry(const cJSON *entry) {
    return supported_keys(entry, "citationId|number|entryHtml|key|itemUri|pdfUri|formatting|sourceItemId");
}
static bool read_entries(const cJSON *section, cJSON **out, char *error, size_t capacity) {
    const cJSON *attrs = field(section, "attrs"), *v = field(attrs, "entriesJson");
    if (v && !cJSON_IsString(v))
        return fail(error, capacity, "Existing bibliography entriesJson must be a string");
    const char *serialized = text(attrs, "entriesJson");
    if (*serialized) {
        bool exact = false;
        if (!ce_csl_json_parse_strict_with_numbers(serialized, strlen(serialized), out, &exact, error,
                                                   capacity))
            return false;
        if (!exact)
            return fail(error, capacity,
                        "Legacy bibliography numbers cannot be rewritten losslessly; original retained");
    } else
        *out = cJSON_CreateArray();
    if (!*out || !cJSON_IsArray(*out))
        return fail(error, capacity, "Existing bibliography entriesJson must be an array");
    for (const cJSON *e = (*out)->child; e; e = e->next)
        if (!cJSON_IsObject(e))
            return fail(error, capacity, "Existing bibliography entry must be an object");
    return true;
}
static bool ordinary_section_fields(const cJSON *section) {
    return supported_keys(section, "type|attrs") &&
           supported_keys(field(section, "attrs"), "style|entriesJson|cslRenderJson|bibliographyMode");
}
static void remove_sections(cJSON *node) {
    if (type_is(node, CITATION) || type_is(node, "paragraph") || type_is(node, "heading"))
        return;
    cJSON *content = cJSON_GetObjectItemCaseSensitive(node, "content");
    for (int i = cJSON_GetArraySize(content) - 1; i >= 0; --i) {
        cJSON *child = cJSON_GetArrayItem(content, i);
        if (type_is(child, BIBLIOGRAPHY))
            cJSON_DeleteItemFromArray(content, i);
        else
            remove_sections(child);
    }
}
static bool json_string_budget(size_t *used, const char *text, size_t size, char *error, size_t capacity) {
    /* Exact quoted-string expansion when embedded into the document JSON.
     * Count a small field/container allowance as well, before copying it. */
    size_t bytes = 32;
    for (size_t i = 0; i < size; ++i) {
        unsigned char ch = (unsigned char)text[i];
        size_t add = ch == '"' || ch == '\\' ? 2
                     : ch < 32 ? (ch == '\b' || ch == '\f' || ch == '\n' || ch == '\r' || ch == '\t' ? 2 : 6)
                               : 1;
        if (bytes > SOURCE_LIMIT - add)
            return fail(error, capacity, "Generated citation data exceeds the document budget");
        bytes += add;
    }
    if (bytes > SOURCE_LIMIT - *used)
        return fail(error, capacity, "Combined citation caches exceed the document budget");
    *used += bytes;
    return true;
}
static bool attach_cache(cJSON *attrs, const CeCitationCache *cache, CeCitationPrepared *prepared,
                         char *error, size_t capacity) {
    CeCslOutputString json = {0};
    if (!ce_citation_cache_encode(cache, &json, error, capacity))
        return false;
    bool ok = json_string_budget(&prepared->generated_bytes, json.data, json.length, error, capacity) &&
              put_text(attrs, "cslRenderJson", json.data);
    ce_csl_output_string_free(&json);
    if (!ok && (!error || !capacity || !*error))
        fail(error, capacity, "Unable to store controlled citation cache");
    return ok;
}
static Record *first_record(const Source *s, const char *identity_key) {
    size_t low = 0, high = s->count;
    while (low < high) {
        size_t mid = low + (high - low) / 2;
        if (strcmp(s->by_identity[mid]->identity, identity_key) < 0)
            low = mid + 1;
        else
            high = mid;
    }
    return low < s->count && !strcmp(s->by_identity[low]->identity, identity_key) ? s->by_identity[low]
                                                                                  : NULL;
}
typedef struct OldKey {
    char *key;
    cJSON *entry;
    size_t index;
} OldKey;
typedef struct OldEntries {
    OldKey *keys;
    size_t count;
    bool *used;
    size_t entries;
    bool *derived;
    size_t derived_capacity;
} OldEntries;
static int old_key_compare(const void *a, const void *b) {
    return strcmp(((const OldKey *)a)->key, ((const OldKey *)b)->key);
}
static void old_free(OldEntries *old) {
    for (size_t i = 0; i < old->count; ++i)
        free(old->keys[i].key);
    free(old->keys);
    free(old->used);
    free(old->derived);
    memset(old, 0, sizeof *old);
}
static bool collect_old_entries(const Source *source, cJSON **entries, OldEntries *old, char *error,
                                size_t capacity) {
    *entries = cJSON_CreateArray();
    if (!*entries)
        return fail(error, capacity, "Out of memory collecting bibliography metadata");
    size_t total = 0;
    for (size_t i = 0; i < source->sections.count; ++i) {
        const cJSON *section = source->sections.data[i], *attrs = field(section, "attrs");
        const cJSON *mode = field(attrs, "bibliographyMode");
        if (mode && (!cJSON_IsString(mode) || strcmp(mode->valuestring, "citations")))
            return fail(error, capacity, "Unsupported bibliographyMode; original bibliography retained");
        if (i && !ordinary_section_fields(section))
            return fail(error, capacity, "Extra bibliography section has custom metadata; original retained");
        cJSON *parsed = NULL;
        bool ok = read_entries(section, &parsed, error, capacity);
        size_t count = ok ? (size_t)cJSON_GetArraySize(parsed) : 0;
        if (ok && count > CE_CSL_INPUT_MAX_ITEMS - total)
            ok = fail(error, capacity, "Combined existing bibliographies exceed the entry limit");
        if (ok && !grow((void **)&old->derived, &old->derived_capacity, total + count, sizeof *old->derived))
            ok = fail(error, capacity, "Out of memory recording bibliography source modes");
        while (ok && parsed->child) {
            if (i && !ordinary_entry(parsed->child)) {
                ok = fail(error, capacity, "Extra bibliography entry has custom metadata; original retained");
                break;
            }
            cJSON *entry = cJSON_DetachItemFromArray(parsed, 0);
            ok = add(*entries, entry);
            if (ok)
                old->derived[total++] = mode != NULL;
        }
        cJSON_Delete(parsed);
        if (!ok)
            return false;
    }
    return true;
}
static bool old_index(cJSON *entries, OldEntries *old, char *error, size_t capacity) {
    old->entries = (size_t)cJSON_GetArraySize(entries);
    if (old->entries > CE_CSL_INPUT_MAX_ITEMS)
        return fail(error, capacity, "Existing bibliography exceeds entry limit");
    if (!old->entries)
        return true;
    old->keys = calloc(old->entries * 3, sizeof *old->keys);
    old->used = calloc(old->entries, sizeof *old->used);
    if (!old->keys || !old->used)
        return fail(error, capacity, "Out of memory indexing old bibliography metadata");
    size_t i = 0;
    for (cJSON *e = entries->child; e; e = e->next, ++i) {
        const char *fields[] = {"sourceItemId", "citationId", "itemUri"};
        const char *tags[] = {"identity", "citation", "uri"};
        for (size_t j = 0; j < 3; ++j)
            if (*text(e, fields[j])) {
                char *id = key(tags[j], NULL, text(e, fields[j]));
                if (!id)
                    return fail(error, capacity, "Invalid old bibliography identity or allocation failure");
                old->keys[old->count++] = (OldKey){id, e, i};
            }
    }
    if (old->count > 1)
        qsort(old->keys, old->count, sizeof *old->keys, old_key_compare);
    for (i = 1; i < old->count; ++i)
        if (!strcmp(old->keys[i - 1].key, old->keys[i].key))
            return fail(error, capacity, "Ambiguous legacy bibliography metadata identity");
    return true;
}
static OldKey *old_lookup(OldEntries *old, const char *id) {
    size_t low = 0, high = old->count;
    while (low < high) {
        size_t mid = low + (high - low) / 2;
        if (strcmp(old->keys[mid].key, id) < 0)
            low = mid + 1;
        else
            high = mid;
    }
    return low < old->count && !strcmp(old->keys[low].key, id) ? &old->keys[low] : NULL;
}
static cJSON *previous_entry(OldEntries *old, const Record *r, bool *oom) {
    const char *values[] = {r->identity, r->id, text(r->attrs, "itemUri")};
    const char *tags[] = {"identity", "citation", "uri"};
    *oom = false;
    for (size_t i = 0; i < 3; ++i)
        if (*values[i]) {
            char *id = key(tags[i], NULL, values[i]);
            if (!id) {
                *oom = true;
                return NULL;
            }
            OldKey *match = old_lookup(old, id);
            free(id);
            if (match) {
                const char *uri = text(match->entry, "itemUri");
                if (i == 1 && *uri && strcmp(uri, text(r->attrs, "itemUri")))
                    continue;
                old->used[match->index] = true;
                return match->entry;
            }
        }
    return NULL;
}
static bool fallback_bib_options(const CeCitationStyle *style, CeCslBibliographyOptions *options) {
    *options = (CeCslBibliographyOptions){CE_CSL_BIBLIOGRAPHY_ALIGN_NONE, false, 1, 1};
    const CeCslXmlNode *node = ce_csl_style_bibliography(ce_citation_style_style(style));
    if (!node)
        return false;
    const char *align = ce_csl_xml_attribute(node, "second-field-align"),
               *hanging = ce_csl_xml_attribute(node, "hanging-indent");
    if (align)
        options->alignment =
            !strcmp(align, "flush") ? CE_CSL_BIBLIOGRAPHY_ALIGN_FLUSH : CE_CSL_BIBLIOGRAPHY_ALIGN_MARGIN;
    options->hanging_indent = hanging && !strcmp(hanging, "true");
    const char *line = ce_csl_xml_attribute(node, "line-spacing"),
               *entry = ce_csl_xml_attribute(node, "entry-spacing");
    if (line)
        options->line_spacing = (uint32_t)strtoul(line, NULL, 10);
    if (entry)
        options->entry_spacing = (uint32_t)strtoul(entry, NULL, 10);
    return options->line_spacing > 0;
}
static bool build_bibliography(Source *s, const CeCitationStyle *style, const CeCslDocument *document,
                               const CeCitationCacheHeader *header, CeCitationPrepared *p, char *error,
                               size_t capacity) {
    cJSON *section = NULL, *old_entries = NULL, *entries = NULL;
    OldEntries previous = {0};
    CeCitationCacheBibliographySource *rows = NULL;
    bool ok = false;
    size_t legacy_bytes = 0;
    if (s->sections.count) {
        section = cJSON_Duplicate(s->sections.data[0], true);
    } else {
        section = cJSON_CreateObject();
        if (!put_text(section, "type", BIBLIOGRAPHY))
            goto done;
    }
    if (!section || !collect_old_entries(s, &old_entries, &previous, error, capacity) ||
        !old_index(old_entries, &previous, error, capacity))
        goto done;
    size_t count = ce_csl_document_bibliography_count(document);
    p->stats.reference_count = count;
    rows = count ? calloc(count, sizeof *rows) : NULL;
    entries = cJSON_CreateArray();
    if ((count && !rows) || !entries)
        goto done;
    CeCslBibliographyOptions options;
    if (!fallback_bib_options(style, &options)) {
        fail(error, capacity, "Citation profile lacks a supported bibliography");
        goto done;
    }
    for (size_t i = 0; i < count; ++i) {
        const CeCslDocumentBibliography *result = ce_csl_document_bibliography(document, i);
        const CeCslInputItem *item = ce_csl_input_item(s->input, result->item_index);
        Record *r = item ? first_record(s, item->id) : NULL;
        if (!r) {
            fail(error, capacity, "Processor bibliography item has no source citation");
            goto done;
        }
        if (!i)
            options = *ce_csl_bibliography_entry_options(result->entry);
        rows[i] = (CeCitationCacheBibliographySource){item->id, result->citation_number, result->visible,
                                                      result->entry};
        p->stats.missing_term |= result->missing_term;
        p->stats.literal_markup |= result->literal_markup;
        p->stats.malformed_markup |= result->malformed_markup;
        bool oom = false;
        cJSON *old = previous_entry(&previous, r, &oom);
        if (oom)
            goto done;
        cJSON *entry = old ? cJSON_Duplicate(old, true) : cJSON_CreateObject();
        if (!entry)
            goto done;
        if (!add(entries, entry))
            goto done;
        const CeCslBibliographyEntry *one[] = {result->entry};
        CeCslOutputString html = {0};
        if (!ce_csl_bibliography_html(one, 1, NULL, NULL, &html, error, capacity))
            goto done;
        bool stored =
            json_string_budget(&legacy_bytes, html.data, html.length, error, capacity) &&
            put_text(entry, "sourceItemId", item->id) && put_text(entry, "citationId", r->id) &&
            put(entry, "number", cJSON_CreateNumber((double)result->citation_number)) &&
            put_text(entry, "entryHtml", html.data) && put_text(entry, "formatting", "csl") &&
            put_text(entry, "key",
                     *text(r->attrs, "citeKey") ? text(r->attrs, "citeKey") : text(r->attrs, "itemKey")) &&
            put_text(entry, "itemUri", text(r->attrs, "itemUri")) &&
            put_text(entry, "pdfUri", text(r->attrs, "pdfUri"));
        ce_csl_output_string_free(&html);
        if (!stored)
            goto done;
    }
    size_t index = 0;
    for (const cJSON *old = old_entries->child; old; old = old->next, ++index)
        if (!previous.used[index]) {
            if (!previous.derived[index]) {
                fail(error, capacity,
                     "Legacy bibliography contains uncited entries; original bibliography retained");
                goto done;
            }
            if (!ordinary_entry(old)) {
                fail(error, capacity, "Unmatched bibliography entry has custom metadata; original retained");
                goto done;
            }
        }
    /* All entries were already validated above. Preserve first-section custom
     * fields without reparsing entries and mistaking an allocation failure for
     * custom metadata. */
    bool keep = s->count != 0 || (s->sections.count && !ordinary_section_fields(s->sections.data[0]));
    if (keep) {
        if (!ce_citation_cache_from_bibliography(header, &options, rows, count, NULL, &p->bibliography, error,
                                                 capacity))
            goto done;
        cJSON *attrs = attributes(section);
        char *json = serialize(entries);
        if (!attrs || !json) {
            free(json);
            goto done;
        }
        bool stored = json_string_budget(&p->generated_bytes, json, strlen(json), error, capacity) &&
                      put_text(attrs, "entriesJson", json) && put_text(attrs, "style", header->profile) &&
                      put_text(attrs, "bibliographyMode", "citations");
        free(json);
        if (!stored || !attach_cache(attrs, p->bibliography, p, error, capacity))
            goto done;
    }
    remove_sections(s->root);
    p->stats.removed_sections = s->sections.count - (keep && s->sections.count ? 1 : 0);
    if (keep) {
        cJSON *content = cJSON_GetObjectItemCaseSensitive(s->root, "content");
        if (!content) {
            if (!put(s->root, "content", cJSON_CreateArray()))
                goto done;
            content = cJSON_GetObjectItemCaseSensitive(s->root, "content");
        }
        cJSON *owned = section;
        section = NULL;
        if (!add(content, owned))
            goto done;
    }
    ok = true;
done:
    cJSON_Delete(section);
    cJSON_Delete(old_entries);
    cJSON_Delete(entries);
    old_free(&previous);
    free(rows);
    if (!ok && (!error || !capacity || !*error))
        fail(error, capacity, "Unable to construct exact bibliography cache");
    return ok;
}
bool ce_citations_prepare(const CeDocument *doc, const CeCitationStyle *style, const CeZoteroLibrary *library,
                          const CeCitationProcessOptions *options, CeCitationPrepared **out, char *error,
                          size_t capacity) {
    if (error && capacity)
        *error = 0;
    if (!out)
        return fail(error, capacity, "Missing prepared citation destination");
    Source source = {0};
    CeCslDocument *processed = NULL;
    CeCitationPrepared *p = calloc(1, sizeof *p);
    bool ok = false;
    if (!p)
        return fail(error, capacity, "Out of memory preparing citations");
    bool persist = options && options->persist_profile;
    if (!source_build(doc, style, library, persist, &source, error, capacity))
        goto done;
    memcpy(p->digest, source.digest, 65);
    p->stats.citation_count = source.count;
    p->stats.repaired_ids = source.repaired;
    if (!source.root) {
        p->json = ce_document_export_json(doc);
        if (!p->json)
            goto done;
    } else {
        if (source.count || persist) {
            cJSON *attrs = attributes(source.root);
            if (!attrs ||
                !put_text(attrs, "citationProfile", ce_citation_style_loaded_info(style)->profile_id))
                goto done;
            /* Explicit citationLocale remains untouched; do not turn a style's
             * implicit default into a persistent user override. */
        }
        if (!ce_csl_document_build(ce_citation_style_style(style), ce_citation_style_locale(style),
                                   source.input, NULL, NULL, &processed, error, capacity))
            goto done;
        const CeCitationStyleInfo *info = ce_citation_style_loaded_info(style);
        CeCitationCacheHeader header = {CE_CITATION_PROCESSOR_PROFILE,
                                        info->profile_id,
                                        info->style_id,
                                        info->style_sha256,
                                        info->style_commit,
                                        ce_csl_locale_language(ce_citation_style_locale(style)),
                                        info->locale_commit,
                                        source.digest};
        p->count = source.count;
        if (p->count) {
            p->citations = calloc(p->count, sizeof *p->citations);
            p->sorted = calloc(p->count, sizeof *p->sorted);
            if (!p->citations || !p->sorted)
                goto done;
        }
        for (size_t i = 0; i < p->count; ++i) {
            Record *r = &source.records[i];
            const CeCslDocumentCitation *result = ce_csl_document_citation(processed, i);
            if (!result || !ce_citation_cache_from_citation(&header, r->id, result->output, NULL,
                                                            &p->citations[i], error, capacity))
                goto done;
            p->sorted[i] = p->citations[i];
            size_t item = ce_csl_input_find(source.input, r->identity);
            uint64_t number = ce_csl_registry_citation_number(ce_csl_document_registry(processed), item);
            if (!number || !put(r->attrs, "endnoteNumber", cJSON_CreateNumber((double)number)) ||
                !attach_cache(r->attrs, p->citations[i], p, error, capacity))
                goto done;
            p->stats.missing_term |= result->missing_term;
            p->stats.literal_markup |= result->literal_markup;
            p->stats.malformed_markup |= result->malformed_markup;
        }
        if (p->count > 1)
            qsort(p->sorted, p->count, sizeof *p->sorted, compare_cache);
        if (source.count || source.sections.count) {
            if (!build_bibliography(&source, style, processed, &header, p, error, capacity))
                goto done;
        }
        p->json = serialize(source.root);
        if (!p->json)
            goto done;
        /* Every prepared document can be fed back into the same strict source
         * reader: cache expansion may not make the next refresh unparseable. */
        cJSON *validated = NULL;
        if (!ce_csl_json_parse_strict(p->json, strlen(p->json), &validated, error, capacity))
            goto done;
        cJSON_Delete(validated);
    }
    ce_citation_prepared_free(*out);
    *out = p;
    p = NULL;
    ok = true;
done:
    ce_csl_document_free(processed);
    source_free(&source);
    ce_citation_prepared_free(p);
    if (!ok && (!error || !capacity || !*error))
        fail(error, capacity, "Unable to prepare citation document within limits");
    return ok;
}
