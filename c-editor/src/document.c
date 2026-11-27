#include "document.h"
#include "cJSON.h"
#include "csl_json.h"
#include <ctype.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CE_HISTORY 128
#define CE_DEPTH 128
#define ATOM "\xef\xbf\xbc"
typedef struct {
    char *p;
    size_t n, cap;
    int failed;
} Buffer;
typedef struct {
    cJSON *node, *block;
    size_t start, end;
} Span;
typedef struct {
    cJSON *node;
    CeBlockInfo info;
} Block;
struct CeDocument {
    cJSON *root;
    Buffer text;
    Span *spans;
    size_t nspans, cspans;
    Block *blocks;
    size_t nblocks, cblocks;
    char *undo[CE_HISTORY], *redo[CE_HISTORY];
    size_t nundo, nredo;
    uint64_t revision, last_edit_revision, prior_edit_revision;
    char *saved;
    char error[256];
};
static char *copy_n(const char *s, size_t n) {
    char *p = malloc(n + 1);
    if (p) {
        memcpy(p, s, n);
        p[n] = 0;
    }
    return p;
}
static char *copy(const char *s) { return copy_n(s ? s : "", strlen(s ? s : "")); }
static char *print_owned(const cJSON *value) {
    char *printed=ce_csl_json_print_exact_with_limit(value,128u*1024u*1024u);
    char *owned=printed?copy(printed):NULL;
    cJSON_free(printed);
    return owned;
}
static void addn(Buffer *b, const char *s, size_t n) {
    if (b->failed)
        return;
    if (n > SIZE_MAX - b->n - 1) {
        b->failed = 1;
        return;
    }
    size_t need = b->n + n + 1;
    if (need > b->cap) {
        size_t cap = b->cap ? b->cap : 128;
        while (cap < need) {
            if (cap > SIZE_MAX / 2) {
                cap = need;
                break;
            }
            cap *= 2;
        }
        char *p = realloc(b->p, cap);
        if (!p) {
            b->failed = 1;
            return;
        }
        b->p = p;
        b->cap = cap;
    }
    if (n)
        memcpy(b->p + b->n, s, n);
    b->n += n;
    b->p[b->n] = 0;
}
static void adds(Buffer *b, const char *s) { addn(b, s, strlen(s)); }
static const char *str(const cJSON *j, const char *key) {
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(j, key);
    return cJSON_IsString(v) ? v->valuestring : "";
}
static const char *type(const cJSON *j) { return str(j, "type"); }
static bool istype(const cJSON *j, const char *t) { return strcmp(type(j), t) == 0; }
static cJSON *field(cJSON *j, const char *k) { return cJSON_GetObjectItemCaseSensitive(j, k); }
static void set(cJSON *j, const char *k, cJSON *v) {
    cJSON_DeleteItemFromObjectCaseSensitive(j, k);
    cJSON_AddItemToObject(j, k, v);
}
static void setstr(cJSON *j, const char *k, const char *v) { set(j, k, cJSON_CreateString(v)); }
static cJSON *node(const char *t) {
    cJSON *j = cJSON_CreateObject();
    if (j && !cJSON_AddStringToObject(j, "type", t)) {
        cJSON_Delete(j);
        return NULL;
    }
    return j;
}
static cJSON *array(cJSON *j) {
    if (!j)
        return NULL;
    cJSON *a = field(j, "content");
    if (!a) {
        a = cJSON_CreateArray();
        if (!a || !cJSON_AddItemToObject(j, "content", a)) {
            cJSON_Delete(a);
            return NULL;
        }
    }
    return a;
}
static cJSON *paragraph(void) {
    cJSON *j = node("paragraph");
    if (j && !array(j)) {
        cJSON_Delete(j);
        return NULL;
    }
    return j;
}
static cJSON *empty_root(void) {
    cJSON *j = node("doc");
    if (!j)
        return NULL;
    cJSON *content = array(j);
    if (!content) {
        cJSON_Delete(j);
        return NULL;
    }
    cJSON *p = paragraph();
    if (!p || !cJSON_AddItemToArray(content, p)) {
        cJSON_Delete(p);
        cJSON_Delete(j);
        return NULL;
    }
    return j;
}
static bool fail(CeDocument *d, const char *s) {
    snprintf(d->error, sizeof d->error, "%s", s);
    return false;
}
static bool utf8(const char *s) {
    const unsigned char *p = (const unsigned char *)s;
    while (*p) {
        uint32_t c;
        unsigned n;
        if (*p < 0x80) {
            p++;
            continue;
        }
        if (*p >= 0xc2 && *p <= 0xdf) {
            c = *p & 31;
            n = 1;
        } else if (*p >= 0xe0 && *p <= 0xef) {
            c = *p & 15;
            n = 2;
        } else if (*p >= 0xf0 && *p <= 0xf4) {
            c = *p & 7;
            n = 3;
        } else
            return false;
        p++;
        for (unsigned i = 0; i < n; i++) {
            if ((*p & 0xc0) != 0x80)
                return false;
            c = (c << 6) | (*p++ & 63);
        }
        if ((n == 1 && c < 0x80) || (n == 2 && c < 0x800) || (n == 3 && c < 0x10000) ||
            c > 0x10ffff || (c >= 0xd800 && c <= 0xdfff))
            return false;
    }
    return true;
}
static bool boundary(const CeDocument *d, size_t p) {
    return p <= d->text.n && (p == d->text.n || (((unsigned char)d->text.p[p] & 0xc0) != 0x80));
}
static bool reserve(void **p, size_t *cap, size_t n, size_t size) {
    if (n <= *cap)
        return true;
    size_t next = *cap ? *cap * 2 : 16;
    if (next < n)
        next = n;
    if (next > SIZE_MAX / size)
        return false;
    void *q = realloc(*p, next * size);
    if (!q)
        return false;
    *p = q;
    *cap = next;
    return true;
}
static bool span_add(CeDocument *d, cJSON *n, cJSON *b, const char *text) {
    if (!reserve((void **)&d->spans, &d->cspans, d->nspans + 1, sizeof(Span)))
        return false;
    size_t start = d->text.n;
    adds(&d->text, text);
    d->spans[d->nspans++] = (Span){n, b, start, d->text.n};
    return !d->text.failed;
}
static bool inline_cache(CeDocument *d, cJSON *j, cJSON *block, unsigned depth) {
    if (depth > CE_DEPTH)
        return fail(d, "Document nesting is too deep");
    if (istype(j, "text"))
        return span_add(d, j, block, str(j, "text"));
    if (istype(j, "hardBreak"))
        return span_add(d, j, block, "\n");
    cJSON *child;
    cJSON_ArrayForEach (child, field(j, "content"))
        if (!inline_cache(d, child, block, depth + 1))
            return false;
    return true;
}
static bool cache_node(CeDocument *d, cJSON *j, unsigned depth, CeBlockKind listkind,
                       unsigned listdepth) {
    if (depth > CE_DEPTH)
        return fail(d, "Document nesting is too deep");
    CeBlockKind k = CE_BLOCK_UNKNOWN;
    if (istype(j, "paragraph"))
        k = listkind == CE_BLOCK_UNKNOWN ? CE_BLOCK_PARAGRAPH : listkind;
    else if (istype(j, "heading"))
        k = CE_BLOCK_HEADING;
    else if (istype(j, "zoteroCitationBlock"))
        k = CE_BLOCK_CITATION;
    else if (istype(j, "zoteroEndnoteSection"))
        k = CE_BLOCK_ENDNOTES;
    else if (istype(j, "bulletList")) {
        listkind = CE_BLOCK_BULLET;
        listdepth++;
    } else if (istype(j, "orderedList")) {
        listkind = CE_BLOCK_ORDERED;
        listdepth++;
    }
    bool leaf = k != CE_BLOCK_UNKNOWN || (!field(j, "content") && !istype(j, "doc"));
    if (leaf) {
        if (d->nblocks)
            adds(&d->text, "\n");
        if (!reserve((void **)&d->blocks, &d->cblocks, d->nblocks + 1, sizeof(Block)))
            return false;
        Block *b = &d->blocks[d->nblocks++];
        memset(b, 0, sizeof *b);
        b->node = j;
        b->info.start = d->text.n;
        b->info.kind = k;
        b->info.depth = listdepth;
        cJSON *attrs = field(j, "attrs"), *level = field(attrs, "level");
        b->info.level = cJSON_IsNumber(level) ? (unsigned)level->valuedouble : 0;
        b->info.alignment = str(attrs, "textAlign");
        if (k == CE_BLOCK_CITATION || k == CE_BLOCK_ENDNOTES || k == CE_BLOCK_UNKNOWN) {
            if (!span_add(d, j, j, ATOM))
                return false;
        } else if (!inline_cache(d, j, j, 0))
            return false;
        b->info.end = d->text.n;
    } else {
        cJSON *child;
        cJSON_ArrayForEach (child, field(j, "content"))
            if (!cache_node(d, child, depth + 1, listkind, listdepth))
                return false;
    }
    return !d->text.failed;
}
static bool rebuild(CeDocument *d) {
    d->text.n = 0;
    d->text.failed = 0;
    d->nblocks = d->nspans = 0;
    adds(&d->text, "");
    return cache_node(d, d->root, 0, CE_BLOCK_UNKNOWN, 0);
}
static bool validate_json(cJSON *j, unsigned depth) {
    if (depth > CE_DEPTH || !cJSON_IsObject(j) || !*type(j))
        return false;
    if (istype(j, "text") && (!cJSON_IsString(field(j, "text")) || !utf8(str(j, "text"))))
        return false;
    cJSON *attrs = field(j, "attrs"), *marks = field(j, "marks");
    if (attrs && !cJSON_IsObject(attrs))
        return false;
    if (marks && !cJSON_IsArray(marks))
        return false;
    cJSON *mark;
    cJSON_ArrayForEach (mark, marks) {
        if (!cJSON_IsObject(mark) || !*type(mark))
            return false;
        cJSON *ma = field(mark, "attrs");
        if (ma && !cJSON_IsObject(ma))
            return false;
    }
    cJSON *a = field(j, "content");
    if (a && !cJSON_IsArray(a))
        return false;
    cJSON *child;
    cJSON_ArrayForEach (child, a)
        if (!validate_json(child, depth + 1))
            return false;
    return true;
}
static void clear_stack(char **s, size_t *n) {
    for (size_t i = 0; i < *n; i++)
        free(s[i]);
    *n = 0;
}
static void push(char **s, size_t *n, char *v) {
    if (*n == CE_HISTORY) {
        free(s[0]);
        memmove(s, s + 1, (CE_HISTORY - 1) * sizeof *s);
        (*n)--;
    }
    s[(*n)++] = v;
}
CeDocument *ce_document_new(void) {
    CeDocument *d = calloc(1, sizeof *d);
    if (!d)
        return NULL;
    d->root = empty_root();
    if (!d->root || !rebuild(d)) {
        ce_document_free(d);
        return NULL;
    }
    ce_document_mark_saved(d);
    if (!d->saved) {
        ce_document_free(d);
        return NULL;
    }
    return d;
}
void ce_document_free(CeDocument *d) {
    if (!d)
        return;
    cJSON_Delete(d->root);
    free(d->text.p);
    free(d->spans);
    free(d->blocks);
    free(d->saved);
    clear_stack(d->undo, &d->nundo);
    clear_stack(d->redo, &d->nredo);
    free(d);
}
const char *ce_document_text(const CeDocument *d) { return d && d->text.p ? d->text.p : ""; }
size_t ce_document_length(const CeDocument *d) { return d ? d->text.n : 0; }
const char *ce_document_error(const CeDocument *d) { return d ? d->error : "Invalid document"; }
char *ce_document_export_json(const CeDocument *d) {
    return d ? print_owned(d->root) : NULL;
}
void ce_document_mark_saved(CeDocument *d) {
    if (d) {
        char *s = ce_document_export_json(d);
        if (s) {
            free(d->saved);
            d->saved = s;
            ++d->revision; d->last_edit_revision=0;
        }
    }
}
bool ce_document_is_modified(const CeDocument *d) {
    if (!d)
        return false;
    char *s = ce_document_export_json(d);
    bool dirty = !s || !d->saved || strcmp(s, d->saved) != 0;
    free(s);
    return dirty;
}
/* Contents and their indexes travel together; savepoints and history stacks
 * remain attached to their document. This operation cannot allocate or fail. */
static void swap_content(CeDocument *d, CeDocument *candidate) {
#define SWAP_CONTENT(type, member) do { type value = d->member; d->member = candidate->member; candidate->member = value; } while (0)
    SWAP_CONTENT(cJSON *, root);
    SWAP_CONTENT(Buffer, text);
    SWAP_CONTENT(Span *, spans);
    SWAP_CONTENT(size_t, nspans);
    SWAP_CONTENT(size_t, cspans);
    SWAP_CONTENT(Block *, blocks);
    SWAP_CONTENT(size_t, nblocks);
    SWAP_CONTENT(size_t, cblocks);
#undef SWAP_CONTENT
}
static bool restore(CeDocument *d, const char *s) {
    CeDocument *candidate = calloc(1, sizeof *candidate);
    if (!candidate) return fail(d, "Out of memory restoring document");
    candidate->root = cJSON_Parse(s);
    if (!candidate->root || !rebuild(candidate)) {
        ce_document_free(candidate);
        return fail(d, "Unable to construct restored document");
    }
    swap_content(d, candidate);
    ce_document_free(candidate);
    return true;
}
static char *begin(CeDocument *d) {
    d->error[0] = 0;
    char *s = ce_document_export_json(d);
    if (!s)
        fail(d, "Out of memory");
    return s;
}
static bool finish(CeDocument *d, char *before, bool ok) {
    /* Legacy in-place edits still need allocations to reconstruct `before`.
     * Persistent OOM can prevent that rollback. The stronger atomic guarantee
     * belongs to apply_json and history, which prepare a private candidate
     * before touching live content; it does not yet cover all editing APIs. */
    if (ok)
        ok = rebuild(d);
    if (!ok) {
        if (!*d->error)
            fail(d, "Out of memory");
        restore(d, before);
        free(before);
        return false;
    }
    char *after = ce_document_export_json(d);
    if (!after) {
        restore(d, before);
        free(before);
        return fail(d, "Out of memory");
    }
    if (strcmp(after, before)) {
        d->prior_edit_revision=d->last_edit_revision;
        d->last_edit_revision=++d->revision;
        push(d->undo, &d->nundo, before);
        clear_stack(d->redo, &d->nredo);
    } else
        free(before);
    free(after);
    return true;
}
bool ce_document_apply_json(CeDocument *d, const char *json) {
    if (!d || !json) return false;
    /* Import can build into an empty private object. Avoid constructing an
     * unrelated initial document/savepoint before replacing it immediately. */
    CeDocument *candidate = calloc(1, sizeof *candidate);
    if (!candidate) return fail(d, "Out of memory");
    char error[256];
    if (!ce_document_import(candidate, json, CE_FORMAT_JSON, error, sizeof(error))) {
        ce_document_free(candidate);
        return fail(d, error);
    }
    char *before = ce_document_export_json(d);
    char *after = ce_document_export_json(candidate);
    if (!before || !after) {
        free(before); free(after); ce_document_free(candidate);
        return fail(d, "Out of memory");
    }
    if (strcmp(before, after)) {
        /* All fallible work is complete, including the new root's indexes and
         * the history snapshot. Commit without allocating or reparsing the
         * old state. Candidate takes ownership of the previous content. */
        swap_content(d, candidate);
        d->prior_edit_revision=d->last_edit_revision;
        d->last_edit_revision=++d->revision;
        push(d->undo, &d->nundo, before);
        before = NULL;
        clear_stack(d->redo, &d->nredo);
    }
    free(before); free(after);
    ce_document_free(candidate);
    d->error[0] = 0;
    return true;
}
bool ce_document_can_undo(const CeDocument *d) { return d && d->nundo; }
bool ce_document_can_redo(const CeDocument *d) { return d && d->nredo; }
uint64_t ce_document_revision(const CeDocument *d) { return d ? d->revision : 0; }
bool ce_document_join_undo(CeDocument *d,uint64_t previous) {
    if(!d || !previous || d->nundo<2 || d->nredo || d->revision!=previous+1 ||
       d->last_edit_revision!=d->revision || d->prior_edit_revision!=previous) return false;
    free(d->undo[--d->nundo]); d->undo[d->nundo]=NULL;
    d->prior_edit_revision=0; /* A particular boundary can be joined only once. */
    return true;
}
static bool history(CeDocument *d, bool redo) {
    char **a = redo ? d->redo : d->undo, **b = redo ? d->undo : d->redo;
    size_t *na = redo ? &d->nredo : &d->nundo, *nb = redo ? &d->nundo : &d->nredo;
    if (!*na)
        return false;
    char *now = ce_document_export_json(d);
    if (!now)
        return fail(d, "Out of memory saving document history");
    if (!restore(d, a[*na - 1])) {
        free(now);
        return false;
    }
    free(a[--*na]);
    push(b, nb, now);
    ++d->revision; d->last_edit_revision=0;
    d->error[0] = 0;
    return true;
}
bool ce_document_undo(CeDocument *d) { return d && history(d, false); }
bool ce_document_redo(CeDocument *d) { return d && history(d, true); }
size_t ce_document_block_count(const CeDocument *d) { return d ? d->nblocks : 0; }
bool ce_document_block_info(const CeDocument *d, size_t i, CeBlockInfo *o) {
    if (!d || !o || i >= d->nblocks)
        return false;
    *o = d->blocks[i].info;
    return true;
}
static size_t block_index(const CeDocument *d, size_t p) {
    for (size_t i = 0; i < d->nblocks; i++)
        if (p <= d->blocks[i].info.end)
            return i;
    return d->nblocks ? d->nblocks - 1 : 0;
}
bool ce_document_block_at(const CeDocument *d, size_t p, CeBlockInfo *o) {
    return d && p <= d->text.n && ce_document_block_info(d, block_index(d, p), o);
}
static CeStyle style_of(const cJSON *j) {
    CeStyle s = {0, "", "", ""};
    const cJSON *m;
    cJSON_ArrayForEach (m, cJSON_GetObjectItemCaseSensitive(j, "marks")) {
        const char *t = type(m);
        const cJSON *a = cJSON_GetObjectItemCaseSensitive(m, "attrs");
        if (!strcmp(t, "bold"))
            s.marks |= CE_MARK_BOLD;
        else if (!strcmp(t, "italic"))
            s.marks |= CE_MARK_ITALIC;
        else if (!strcmp(t, "underline"))
            s.marks |= CE_MARK_UNDERLINE;
        else if (!strcmp(t, "highlight")) {
            s.marks |= CE_MARK_HIGHLIGHT;
            s.highlight = str(a, "color");
        } else if (!strcmp(t, "textStyle")) {
            s.color = str(a, "color");
            if (*s.color)
                s.marks |= CE_MARK_COLOR;
        } else if (!strcmp(t, "link")) {
            s.marks |= CE_MARK_LINK;
            s.link = str(a, "href");
        }
    }
    return s;
}
size_t ce_document_span_count(const CeDocument *d) { return d ? d->nspans : 0; }
bool ce_document_span(const CeDocument *d, size_t i, CeStyleSpan *o) {
    if (!d || !o || i >= d->nspans)
        return false;
    Span *s = &d->spans[i];
    *o = (CeStyleSpan){s->start, s->end, style_of(s->node)};
    return true;
}
CeStyle ce_document_style_at(const CeDocument *d, size_t p) {
    if (d)
        for (size_t i = 0; i < d->nspans; i++)
            if (p >= d->spans[i].start && p < d->spans[i].end)
                return style_of(d->spans[i].node);
    return (CeStyle){0, "", "", ""};
}
char *ce_document_block_json(const CeDocument *d, size_t i) {
    return d && i < d->nblocks ? print_owned(d->blocks[i].node) : NULL;
}
static cJSON *parent_of(cJSON *j, cJSON *wanted, unsigned depth) {
    if (depth > CE_DEPTH)
        return NULL;
    cJSON *a = field(j, "content"), *n;
    cJSON_ArrayForEach (n, a) {
        if (n == wanted)
            return a;
        cJSON *p = parent_of(n, wanted, depth + 1);
        if (p)
            return p;
    }
    return NULL;
}
static int index_of(cJSON *a, cJSON *n) {
    int i = 0;
    cJSON *x;
    cJSON_ArrayForEach (x, a) {
        if (x == n)
            return i;
        i++;
    }
    return -1;
}
static cJSON *container_of(cJSON *j, cJSON *wanted, unsigned depth) {
    if (depth > CE_DEPTH)
        return NULL;
    cJSON *n;
    cJSON_ArrayForEach(n, field(j, "content")) {
        if (n == wanted)
            return j;
        cJSON *p = container_of(n, wanted, depth + 1);
        if (p)
            return p;
    }
    return NULL;
}
static bool is_list(cJSON *n) {
    return istype(n, "bulletList") || istype(n, "orderedList");
}
static cJSON *list_item(CeDocument *d, cJSON *n) {
    cJSON *p = container_of(d->root, n, 0);
    return istype(p, "listItem") ? p : NULL;
}
static bool editable(cJSON *n) {
    if (!istype(n, "paragraph") && !istype(n, "heading"))
        return false;
    cJSON *x;
    cJSON_ArrayForEach (x, field(n, "content"))
        if (!istype(x, "text") && !istype(x, "hardBreak"))
            return false;
    return true;
}
static size_t inline_size(cJSON *n) {
    return istype(n, "text") ? strlen(str(n, "text")) : istype(n, "hardBreak") ? 1 : 0;
}
static cJSON *slice(cJSON *b, size_t from, size_t to) {
    cJSON *a = cJSON_CreateArray();
    size_t pos = 0;
    cJSON *n;
    cJSON_ArrayForEach (n, field(b, "content")) {
        size_t len = inline_size(n);
        if (pos < to && pos + len > from) {
            size_t lo = from > pos ? from - pos : 0, hi = to < pos + len ? to - pos : len;
            cJSON *part = cJSON_Duplicate(n, true);
            if (!part) {
                cJSON_Delete(a);
                return NULL;
            }
            if (istype(part, "text")) {
                char *s = copy_n(str(n, "text") + lo, hi - lo);
                if (!s) {
                    cJSON_Delete(part);
                    cJSON_Delete(a);
                    return NULL;
                }
                setstr(part, "text", s);
                free(s);
            }
            cJSON_AddItemToArray(a, part);
        }
        pos += len;
    }
    return a;
}
static void append_array(cJSON *a, cJSON *b) {
    while (cJSON_GetArraySize(b))
        cJSON_AddItemToArray(a, cJSON_DetachItemFromArray(b, 0));
    cJSON_Delete(b);
}
/* Split off following list items, retaining list attributes and numbering. */
static cJSON *list_suffix(cJSON *list, int first) {
    cJSON *suffix = cJSON_Duplicate(list, true), *content = cJSON_CreateArray();
    if (!suffix || !content) {
        cJSON_Delete(suffix);
        cJSON_Delete(content);
        return NULL;
    }
    set(suffix, "content", content);
    cJSON *a = array(list);
    while (cJSON_GetArraySize(a) > first)
        cJSON_AddItemToArray(content, cJSON_DetachItemFromArray(a, first));
    if (istype(list, "orderedList") && first) {
        cJSON *attrs = field(suffix, "attrs");
        if (!attrs) {
            attrs = cJSON_CreateObject();
            set(suffix, "attrs", attrs);
        }
        cJSON *start = field(attrs, "start");
        double number = cJSON_IsNumber(start) ? start->valuedouble : 1;
        set(attrs, "start", cJSON_CreateNumber(number + first));
    }
    return suffix;
}
/* Lift a nested item one level, or unwrap a top-level item. Its descendants
 * travel with it and all following content retains its document order. */
static bool lift_item(CeDocument *d, cJSON *item) {
    cJSON *list = container_of(d->root, item, 0);
    if (!is_list(list))
        return fail(d, "List item has no list container");
    cJSON *parent = container_of(d->root, list, 0);
    cJSON *a = array(list), *p = field(parent, "content");
    int at = index_of(a, item), slot = index_of(p, list);
    cJSON *suffix = list_suffix(list, at + 1);
    if (!parent || at < 0 || slot < 0 || !suffix) {
        cJSON_Delete(suffix);
        return false;
    }
    cJSON_DetachItemFromArray(a, at);
    if (istype(parent, "listItem")) {
        cJSON *outer = container_of(d->root, parent, 0);
        if (!is_list(outer)) {
            cJSON_Delete(item);
            cJSON_Delete(suffix);
            return fail(d, "Nested list item has no outer list");
        }
        if (cJSON_GetArraySize(field(suffix, "content")))
            cJSON_AddItemToArray(array(item), suffix);
        else
            cJSON_Delete(suffix);
        while (cJSON_GetArraySize(p) > slot + 1)
            cJSON_AddItemToArray(array(item), cJSON_DetachItemFromArray(p, slot + 1));
        cJSON_InsertItemInArray(array(outer), index_of(array(outer), parent) + 1, item);
    } else {
        int insert = slot + 1;
        while (cJSON_GetArraySize(field(item, "content")))
            cJSON_InsertItemInArray(p, insert++, cJSON_DetachItemFromArray(array(item), 0));
        cJSON_Delete(item);
        if (cJSON_GetArraySize(field(suffix, "content")))
            cJSON_InsertItemInArray(p, insert, suffix);
        else
            cJSON_Delete(suffix);
    }
    if (!cJSON_GetArraySize(a))
        cJSON_DeleteItemFromArray(p, slot);
    return true;
}
static bool append_text(cJSON *a, const char *s, size_t n, cJSON *marks) {
    if (!n)
        return true;
    char *txt = copy_n(s, n);
    cJSON *j = node("text");
    if (!txt || !j) {
        free(txt);
        cJSON_Delete(j);
        return false;
    }
    cJSON_AddStringToObject(j, "text", txt);
    free(txt);
    if (marks)
        cJSON_AddItemToObject(j, "marks", cJSON_Duplicate(marks, true));
    return cJSON_AddItemToArray(a, j);
}
static void prune(cJSON *j) {
    cJSON *a = field(j, "content");
    for (int i = cJSON_GetArraySize(a) - 1; i >= 0; i--) {
        cJSON *n = cJSON_GetArrayItem(a, i);
        prune(n);
        if ((istype(n, "bulletList") || istype(n, "orderedList") || istype(n, "listItem")) &&
            cJSON_GetArraySize(field(n, "content")) == 0)
            cJSON_DeleteItemFromArray(a, i);
    }
}
static bool replace_inner(CeDocument *d, size_t from, size_t to, const char *s) {
    if (!d->nblocks)
        cJSON_AddItemToArray(array(d->root), paragraph());
    if (!rebuild(d))
        return false;
    size_t first = block_index(d, from), last = block_index(d, to);
    Block *a = &d->blocks[first], *b = &d->blocks[last];
    /* A position immediately after a separator belongs to the following block. */
    if (from > a->info.end && first + 1 < d->nblocks) {
        first++;
        a = &d->blocks[first];
    }
    if (from == to && !editable(a->node)) {
        if (from != a->info.start && from != a->info.end)
            return fail(d, "Cannot edit inside an atomic block");
        cJSON *parent = parent_of(d->root, a->node, 0), *p = paragraph();
        int index = index_of(parent, a->node);
        if (!parent || index < 0 || !p) {
            cJSON_Delete(p);
            return false;
        }
        cJSON_InsertItemInArray(parent, index + (from == a->info.end), p);
        if (!strcmp(s, "\n"))
            return true; /* The new block separator itself represents Enter. */
        if (!rebuild(d))
            return false;
        for (size_t i = 0; i < d->nblocks; i++)
            if (d->blocks[i].node == p)
                return replace_inner(d, d->blocks[i].info.start, d->blocks[i].info.start, s);
        return false;
    }
    if (from == to && !strcmp(s, "\n") && a->info.start == a->info.end) {
        cJSON *item = list_item(d, a->node);
        if (item && cJSON_GetArraySize(field(item, "content")) == 1)
            return lift_item(d, item);
    }
    cJSON *head = cJSON_CreateArray(), *tail = cJSON_CreateArray(), *marks = NULL;
    if (editable(a->node)) {
        cJSON_Delete(head);
        head = slice(a->node, 0, from - a->info.start);
    } else if (from != a->info.start) {
        cJSON_Delete(head);
        cJSON_Delete(tail);
        return fail(d, "Cannot edit inside an atomic block");
    }
    if (editable(b->node)) {
        cJSON_Delete(tail);
        tail = slice(b->node, to - b->info.start, b->info.end - b->info.start);
    } else if (to != b->info.end) {
        cJSON_Delete(head);
        cJSON_Delete(tail);
        return fail(d, "Select an entire atomic block to replace it");
    }
    if (!head || !tail) {
        cJSON_Delete(head);
        cJSON_Delete(tail);
        return false;
    }
    for (size_t i = 0; i < d->nspans; i++) {
        Span *sp = &d->spans[i];
        if (sp->block == a->node && from >= sp->start && from <= sp->end &&
            istype(sp->node, "text")) {
            marks = cJSON_Duplicate(field(sp->node, "marks"), true);
            break;
        }
    }
    cJSON *parent = parent_of(d->root, a->node, 0);
    int idx = index_of(parent, a->node);
    if (!parent || idx < 0) {
        cJSON_Delete(head);
        cJSON_Delete(tail);
        cJSON_Delete(marks);
        return false;
    }
    cJSON *base = editable(a->node) ? cJSON_Duplicate(a->node, true) : paragraph();
    if (!base) {
        cJSON_Delete(head);
        cJSON_Delete(tail);
        cJSON_Delete(marks);
        return false;
    }
    for (size_t i = last + 1; i > first; i--) {
        cJSON *n = d->blocks[i - 1].node, *p = parent_of(d->root, n, 0);
        if (p)
            cJSON_DeleteItemFromArray(p, index_of(p, n));
    }
    set(base, "content", head);
    cJSON_InsertItemInArray(parent, idx, base);
    const char *at = s, *nl;
    while ((nl = strchr(at, '\n'))) {
        if (!append_text(array(base), at, (size_t)(nl - at), marks))
            goto bad;
        cJSON *next = paragraph();
        if (!next)
            goto bad;
        cJSON *item = list_item(d, base);
        cJSON *list = item ? container_of(d->root, item, 0) : NULL;
        if (item && is_list(list)) {
            cJSON *next_item = cJSON_Duplicate(item, true), *content = cJSON_CreateArray();
            if (!next_item || !content) {
                cJSON_Delete(next_item);
                cJSON_Delete(content);
                cJSON_Delete(next);
                goto bad;
            }
            set(next_item, "content", content);
            cJSON_AddItemToArray(content, next);
            while (cJSON_GetArraySize(parent) > idx + 1)
                cJSON_AddItemToArray(content, cJSON_DetachItemFromArray(parent, idx + 1));
            cJSON_InsertItemInArray(array(list), index_of(array(list), item) + 1, next_item);
            parent = content;
            idx = 0;
        } else
            cJSON_InsertItemInArray(parent, ++idx, next);
        base = next;
        at = nl + 1;
    }
    if (!append_text(array(base), at, strlen(at), marks))
        goto bad;
    append_array(array(base), tail);
    cJSON_Delete(marks);
    prune(d->root);
    return true;
bad:
    cJSON_Delete(tail);
    cJSON_Delete(marks);
    return false;
}
bool ce_document_replace(CeDocument *d, size_t a, size_t b, const char *s) {
    if (!d || !s)
        return false;
    if (a > b || !boundary(d, a) || !boundary(d, b) || !utf8(s))
        return fail(d, "Invalid UTF-8 or byte boundary");
    if (a == b && !*s)
        return true;
    char *old = begin(d);
    return old && finish(d, old, replace_inner(d, a, b, s));
}
bool ce_document_insert(CeDocument *d, size_t p, const char *s) {
    return ce_document_replace(d, p, p, s);
}
bool ce_document_delete(CeDocument *d, size_t a, size_t b) {
    return ce_document_replace(d, a, b, "");
}
static const char *mark_type(CeMark m) {
    switch (m) {
    case CE_MARK_BOLD:
        return "bold";
    case CE_MARK_ITALIC:
        return "italic";
    case CE_MARK_UNDERLINE:
        return "underline";
    case CE_MARK_HIGHLIGHT:
        return "highlight";
    case CE_MARK_COLOR:
        return "textStyle";
    case CE_MARK_LINK:
        return "link";
    default:
        return NULL;
    }
}
static void change_mark(cJSON *n, CeMark m, const char *v, bool on) {
    const char *t = mark_type(m);
    cJSON *a = field(n, "marks");
    if (!a) {
        a = cJSON_CreateArray();
        cJSON_AddItemToObject(n, "marks", a);
    }
    for (int i = cJSON_GetArraySize(a) - 1; i >= 0; i--)
        if (istype(cJSON_GetArrayItem(a, i), t))
            cJSON_DeleteItemFromArray(a, i);
    if (on) {
        cJSON *j = node(t);
        if (m == CE_MARK_COLOR || m == CE_MARK_HIGHLIGHT || m == CE_MARK_LINK) {
            cJSON *attrs = cJSON_CreateObject();
            cJSON_AddStringToObject(attrs, m == CE_MARK_LINK ? "href" : "color",
                                    v                        ? v
                                    : m == CE_MARK_HIGHLIGHT ? "#ffff00"
                                                             : "");
            cJSON_AddItemToObject(j, "attrs", attrs);
        }
        cJSON_AddItemToArray(a, j);
    }
    if (!cJSON_GetArraySize(a))
        cJSON_DeleteItemFromObjectCaseSensitive(n, "marks");
}
static bool apply_style(CeDocument *d, size_t a, size_t b, CeMark m, const char *v, bool on,
                        bool clear) {
    if (!d)
        return false;
    if (a > b || !boundary(d, a) || !boundary(d, b) || (!clear && !mark_type(m)))
        return fail(d, "Invalid style range");
    if (a == b)
        return true;
    char *old = begin(d);
    if (!old)
        return false;
    for (size_t i = d->nspans; i > 0; i--) {
        Span *s = &d->spans[i - 1];
        if (clear && istype(s->node, "hardBreak") && s->start >= a && s->end <= b) {
            cJSON_DeleteItemFromObjectCaseSensitive(s->node, "marks");
            continue;
        }
        if (!istype(s->node, "text") || s->end <= a || s->start >= b)
            continue;
        cJSON *p = parent_of(d->root, s->node, 0);
        int index = index_of(p, s->node);
        size_t lo = a > s->start ? a - s->start : 0,
               hi = b < s->end ? b - s->start : s->end - s->start;
        const char *text = str(s->node, "text");
        cJSON *parts = cJSON_CreateArray();
        size_t cuts[] = {0, lo, hi, s->end - s->start};
        for (int k = 0; k < 3; k++) {
            if (cuts[k] == cuts[k + 1])
                continue;
            cJSON *part = cJSON_Duplicate(s->node, true);
            char *frag = copy_n(text + cuts[k], cuts[k + 1] - cuts[k]);
            if (!part || !frag) {
                cJSON_Delete(part);
                free(frag);
                cJSON_Delete(parts);
                return finish(d, old, false);
            }
            setstr(part, "text", frag);
            free(frag);
            if (k == 1) {
                if (clear)
                    cJSON_DeleteItemFromObjectCaseSensitive(part, "marks");
                else
                    change_mark(part, m, v, on);
            }
            cJSON_AddItemToArray(parts, part);
        }
        cJSON_DeleteItemFromArray(p, index);
        while (cJSON_GetArraySize(parts))
            cJSON_InsertItemInArray(p, index++, cJSON_DetachItemFromArray(parts, 0));
        cJSON_Delete(parts);
    }
    return finish(d, old, true);
}
bool ce_document_style(CeDocument *d, size_t a, size_t b, CeMark m, const char *v, bool on) {
    return apply_style(d, a, b, m, v, on, false);
}
bool ce_document_clear_formatting(CeDocument *d, size_t a, size_t b) {
    return apply_style(d, a, b, CE_MARK_BOLD, NULL, false, true);
}
/* Nonempty ranges are half-open; selecting up to the next block's start must
 * not also format that next block. A caret still selects its own block. */
static size_t last_selected_block(CeDocument *d, size_t a, size_t b) {
    size_t last = block_index(d, b);
    if (b > a && last && d->blocks[last].info.start == b)
        last--;
    return last;
}
/* Structural list edits are built privately: no live document/history mutation
 * occurs before the single atomic apply_json commit. */
#define LE_MAX_BYTES (16u * 1024u * 1024u)
#define LE_MAX_VALUES 250000u
#define LE_MAX_WORK 16000000u
#define LE_NONE SIZE_MAX
typedef struct {
    cJSON *node;
    size_t parent, first, last;
    unsigned depth;
} ListEditRef;
typedef struct {
    CeDocument *doc;
    ListEditRef *refs;
    size_t count, capacity, next_block, first_block, last_block, work;
} ListEdit;
static bool le_set(cJSON *obj, const char *key, cJSON *value) {
    if (!value) return false;
    cJSON_DeleteItemFromObjectCaseSensitive(obj,key);
    if (cJSON_AddItemToObject(obj,key,value)) return true;
    cJSON_Delete(value); return false;
}
static bool le_index(ListEdit *e, cJSON *n, size_t parent, unsigned depth) {
    if (++e->work > LE_MAX_WORK || depth > 128 || e->count >= LE_MAX_VALUES)
        return fail(e->doc,"List edit traversal limit exceeded");
    if (!reserve((void **)&e->refs,&e->capacity,e->count+1,sizeof *e->refs)) return false;
    size_t at=e->count++;
    e->refs[at]=(ListEditRef){n,parent,LE_NONE,LE_NONE,depth};
    if (e->next_block<e->doc->nblocks && e->doc->blocks[e->next_block].node==n) {
        size_t b=e->next_block++;
        if (b>=e->first_block && b<=e->last_block) e->refs[at].first=e->refs[at].last=at;
    }
    cJSON *child;
    cJSON_ArrayForEach(child,field(n,"content")) {
        size_t sub=e->count;
        if (!le_index(e,child,at,depth+1)) return false;
        if (e->refs[sub].first!=LE_NONE) {
            if (e->refs[at].first==LE_NONE) e->refs[at].first=e->refs[sub].first;
            e->refs[at].last=e->refs[sub].last;
        }
    }
    return true;
}
static bool le_reindex(ListEdit *e,size_t a,size_t b) {
    e->count=e->next_block=0;
    e->first_block=block_index(e->doc,a); e->last_block=last_selected_block(e->doc,a,b);
    return le_index(e,e->doc->root,LE_NONE,0) && e->next_block==e->doc->nblocks;
}
/* cJSON's first child prev points at the last child. Splice at a known pointer
 * instead of repeatedly scanning linked arrays by ordinal (quadratic on lists). */
static void le_before(cJSON *a,cJSON *before,cJSON *n) {
    if (!before) { cJSON_AddItemToArray(a,n); return; }
    n->next=before; n->prev=before->prev;
    if (a->child==before) a->child=n; else before->prev->next=n;
    before->prev=n;
}
static void le_move(cJSON *from,cJSON *to,cJSON *n,cJSON *before) {
    cJSON_DetachItemViaPointer(from,n); le_before(to,before,n);
}
static bool le_discardable(cJSON *n) {
    cJSON *f;
    cJSON_ArrayForEach(f,n) {
        if (!strcmp(f->string,"type") || !strcmp(f->string,"content")) continue;
        if (strcmp(f->string,"attrs") || !cJSON_IsObject(f)) return false;
        cJSON *attr;
        cJSON_ArrayForEach(attr,f)
            if (!istype(n,"orderedList") || strcmp(attr->string,"start")) return false;
    }
    return true;
}
static cJSON *le_shell(cJSON *source) {
    cJSON *out=cJSON_CreateObject(); if (!out) return NULL;
    cJSON *f;
    cJSON_ArrayForEach(f,source)
        if (strcmp(f->string,"content") && !le_set(out,f->string,cJSON_Duplicate(f,true))) { cJSON_Delete(out); return NULL; }
    if (!le_set(out,"content",cJSON_CreateArray())) { cJSON_Delete(out); return NULL; }
    return out;
}
static bool le_list_shape(CeDocument *d,cJSON *list) {
    cJSON *a=field(list,"content"),*item;
    if (!cJSON_IsArray(a)) return fail(d,"List has no content array");
    cJSON_ArrayForEach(item,a)
        if (!istype(item,"listItem") || !cJSON_IsArray(field(item,"content")) || !field(item,"content")->child)
            return fail(d,"Malformed list item cannot be structurally edited");
    return true;
}
static bool le_change(ListEdit *e,size_t list_at,size_t first_at,size_t last_at,int direction,bool *changed) {
    CeDocument *d=e->doc;
    cJSON *list=e->refs[list_at].node,*first=e->refs[first_at].node,*last=e->refs[last_at].node;
    cJSON *a=field(list,"content");
    if (!le_list_shape(d,list)) return false;
    if (direction>0) {
        if (a->child==first) return true;
        cJSON *previous=first->prev,*children=field(previous,"content"),*nested=children->child?children->child->prev:NULL;
        if (!nested || strcmp(type(nested),type(list))) {
            nested=node(type(list));
            if (!nested || !le_set(nested,"content",cJSON_CreateArray())) { cJSON_Delete(nested); return false; }
            if (istype(list,"orderedList")) {
                cJSON *attrs=cJSON_CreateObject();
                if (!attrs || !le_set(attrs,"start",cJSON_CreateNumber(1))) { cJSON_Delete(attrs); cJSON_Delete(nested); return false; }
                if (!le_set(nested,"attrs",attrs)) { cJSON_Delete(nested); return false; }
            }
            cJSON_AddItemToArray(children,nested);
        } else if (!le_list_shape(d,nested)) return false;
        cJSON *after=last->next,*item=first;
        while (item!=after) { cJSON *next=item->next; le_move(a,field(nested,"content"),item,NULL); item=next; }
        *changed=true; return true;
    }
    size_t parent_at=e->refs[list_at].parent;
    if (parent_at==LE_NONE) return fail(d,"List has no parent");
    cJSON *parent=e->refs[parent_at].node,*p=field(parent,"content");
    bool nested=istype(parent,"listItem");
    size_t outer_at=e->refs[parent_at].parent;
    cJSON *outer=nested && outer_at!=LE_NONE?e->refs[outer_at].node:NULL;
    if (nested && (!is_list(outer) || !le_list_shape(d,outer))) return fail(d,"Nested item has no outer list");
    if (first==a->child && !last->next && !le_discardable(list))
        return fail(d,"Removing this list would discard unknown metadata");
    if (!nested) for (cJSON *item=first;;item=item->next) {
        if (!le_discardable(item)) return fail(d,"Unwrapping this item would discard unknown metadata");
        if (item==last) break;
    }
    cJSON *suffix=last->next?le_shell(list):NULL;
    if (last->next && !suffix) return false;
    while (last->next) le_move(a,field(suffix,"content"),last->next,NULL);
    cJSON *after=last->next; /* NULL after moving the suffix */
    if (nested) {
        cJSON *tail=field(last,"content");
        cJSON *existing=tail->child?tail->child->prev:NULL;
        if (suffix && existing && !strcmp(type(existing),type(suffix))) {
            /* ProseMirror joins following items to the lifted item's existing
             * final list, retaining that list's start. Custom suffix metadata
             * has no safe merge rule, so refuse rather than dropping it. */
            if (!le_discardable(suffix) || !le_list_shape(d,existing)) {
                cJSON_Delete(suffix);
                return fail(d,"Joining the trailing list would discard unsupported metadata");
            }
            cJSON *children=field(suffix,"content");
            while (children->child) le_move(children,field(existing,"content"),children->child,NULL);
            cJSON_Delete(suffix);
        } else if (suffix) cJSON_AddItemToArray(tail,suffix);
        while (list->next) le_move(p,tail,list->next,NULL);
        cJSON *insert=parent->next,*item=first;
        while (item!=after) { cJSON *next=item->next; le_move(a,field(outer,"content"),item,insert); item=next; }
    } else {
        cJSON *insert=list->next,*item=first;
        while (item!=after) {
            cJSON *next=item->next,*children=field(item,"content");
            while (children->child) le_move(children,p,children->child,insert);
            cJSON_Delete(cJSON_DetachItemViaPointer(a,item)); item=next;
        }
        if (suffix) le_before(p,insert,suffix);
    }
    if (!a->child) cJSON_Delete(cJSON_DetachItemViaPointer(p,list));
    *changed=true; return true;
}
static bool le_lists(ListEdit *e,int direction,bool *changed) {
    for (size_t i=0;i<e->count;++i) {
        if (!is_list(e->refs[i].node) || e->refs[i].first==LE_NONE) continue;
        bool ancestor=false;
        for (size_t p=e->refs[i].parent;p!=LE_NONE;p=e->refs[p].parent) {
            if (++e->work>LE_MAX_WORK) return fail(e->doc,"List edit work limit exceeded");
            if (is_list(e->refs[p].node)) { ancestor=true; break; }
        }
        if (ancestor) continue;
        size_t first=e->refs[i].first,last=e->refs[i].last,x=first,y=last;
        while (x!=y) {
            if (e->refs[x].depth>=e->refs[y].depth) x=e->refs[x].parent;
            else y=e->refs[y].parent;
        }
        while (x!=LE_NONE && !is_list(e->refs[x].node)) x=e->refs[x].parent;
        if (x==LE_NONE) return fail(e->doc,"Cannot locate selected list");
        while (e->refs[first].parent!=x) first=e->refs[first].parent;
        while (e->refs[last].parent!=x) last=e->refs[last].parent;
        /* Skip this whole outer subtree after mutating it; its old refs may
         * point to wrappers removed by the operation. Other roots are disjoint. */
        size_t next=i+1;
        while (next<e->count && e->refs[next].depth>e->refs[i].depth) ++next;
        if (!le_change(e,x,first,last,direction,changed)) return false;
        i=next-1;
    }
    return true;
}
static bool le_clear_marks(ListEdit *e,size_t a,size_t b) {
    if (a==b) return true;
    size_t span=0;
    for (size_t i=0;i<e->count && span<e->doc->nspans;++i) {
        cJSON *n=e->refs[i].node;
        if (n!=e->doc->spans[span].node) continue;
        Span *s=&e->doc->spans[span++];
        if (s->end<=a || s->start>=b || (!istype(n,"text") && !istype(n,"hardBreak")) || !field(n,"marks")) continue;
        if (istype(n,"hardBreak")) { cJSON_DeleteItemFromObjectCaseSensitive(n,"marks"); continue; }
        size_t lo=a>s->start?a-s->start:0,hi=b<s->end?b-s->start:s->end-s->start;
        if (!lo && hi==s->end-s->start) { cJSON_DeleteItemFromObjectCaseSensitive(n,"marks"); continue; }
        cJSON *parent=field(e->refs[e->refs[i].parent].node,"content");
        size_t cuts[]={0,lo,hi,s->end-s->start};
        for (size_t k=0;k<3;++k) {
            if (cuts[k]==cuts[k+1]) continue;
            cJSON *part=cJSON_Duplicate(n,true); char *text=copy_n(str(n,"text")+cuts[k],cuts[k+1]-cuts[k]);
            if (!part || !text) { cJSON_Delete(part); free(text); return false; }
            bool ok=le_set(part,"text",cJSON_CreateString(text)); free(text);
            if (!ok) { cJSON_Delete(part); return false; }
            if (k==1) cJSON_DeleteItemFromObjectCaseSensitive(part,"marks");
            le_before(parent,n,part);
        }
        cJSON_Delete(cJSON_DetachItemViaPointer(parent,n));
    }
    return true;
}
static bool le_edit(CeDocument *d,size_t a,size_t b,int direction,bool clear,bool *changed) {
    if (!d) return false;
    if (a>b || !boundary(d,a) || !boundary(d,b) || (!clear && direction!=1 && direction!=-1))
        return fail(d,"Invalid list edit range or direction");
    CeDocument *candidate=calloc(1,sizeof *candidate);
    if (!candidate) return fail(d,"Out of memory preparing list edit");
    char *json=ce_csl_json_print_exact_with_limit(d->root,LE_MAX_BYTES);
    bool ok=json && ce_csl_json_parse_strict(json,strlen(json),&candidate->root,candidate->error,sizeof candidate->error);
    cJSON_free(json); json=NULL;
    if (ok) ok=rebuild(candidate);
    ListEdit e={0}; e.doc=candidate; bool any=false;
    if (ok) ok=le_reindex(&e,a,b);
    if (ok && clear) {
        for (size_t i=e.first_block;i<=e.last_block && i<candidate->nblocks;++i) {
            cJSON *n=candidate->blocks[i].node;
            if (!istype(n,"heading") && !istype(n,"paragraph")) continue;
            if (istype(n,"heading") && !le_set(n,"type",cJSON_CreateString("paragraph"))) { ok=false; break; }
            cJSON *attrs=field(n,"attrs");
            cJSON_DeleteItemFromObjectCaseSensitive(attrs,"level");
            cJSON_DeleteItemFromObjectCaseSensitive(attrs,"textAlign");
        }
        if (ok) ok=le_clear_marks(&e,a,b);
        if (ok) ok=rebuild(candidate) && le_reindex(&e,a,b);
    }
    for (unsigned pass=0;ok;++pass) {
        bool moved=false; ok=le_lists(&e,clear?-1:direction,&moved); any|=moved;
        if (!ok || !clear || !moved) break;
        if (pass>=128) { ok=fail(candidate,"List clear depth limit exceeded"); break; }
        ok=rebuild(candidate) && le_reindex(&e,a,b);
    }
    if (ok) ok=rebuild(candidate);
    if (ok && (candidate->text.n!=d->text.n || strcmp(candidate->text.p,d->text.p)))
        ok=fail(candidate,"List transformation would change text order");
    if (ok) { json=ce_csl_json_print_exact_with_limit(candidate->root,LE_MAX_BYTES); ok=json!=NULL; }
    if (ok) {
        cJSON *checked=NULL;
        ok=ce_csl_json_parse_strict(json,strlen(json),&checked,candidate->error,sizeof candidate->error);
        cJSON_Delete(checked);
    }
    if (!ok) fail(d,*candidate->error?candidate->error:"List edit exceeds limits or memory is unavailable");
    if (ok) ok=ce_document_apply_json(d,json);
    cJSON_free(json); free(e.refs); ce_document_free(candidate);
    if (ok && changed) *changed=any;
    return ok;
}
bool ce_document_adjust_list_depth(CeDocument *d,size_t a,size_t b,int direction,bool *changed) {
    return le_edit(d,a,b,direction,false,changed);
}
bool ce_document_clear_block_formatting(CeDocument *d,size_t a,size_t b) {
    return le_edit(d,a,b,-1,true,NULL);
}

bool ce_document_set_alignment(CeDocument *d, size_t a, size_t b, const char *alignment) {
    if (!d || !alignment || a > b || !boundary(d, a) || !boundary(d, b))
        return false;
    if (strcmp(alignment, "left") && strcmp(alignment, "center") &&
        strcmp(alignment, "right") && strcmp(alignment, "justify"))
        return fail(d, "Unknown text alignment");
    char *old = begin(d);
    if (!old)
        return false;
    size_t first = block_index(d, a), last = last_selected_block(d, a, b);
    for (size_t i = first; i <= last && i < d->nblocks; i++) {
        cJSON *n = d->blocks[i].node;
        if (!istype(n, "paragraph") && !istype(n, "heading"))
            continue;
        cJSON *attrs = field(n, "attrs");
        if (!attrs) {
            attrs = cJSON_CreateObject();
            set(n, "attrs", attrs);
        }
        if (!attrs)
            return finish(d, old, false);
        setstr(attrs, "textAlign", alignment);
    }
    return finish(d, old, true);
}
bool ce_document_set_block(CeDocument *d, size_t a, size_t b, CeBlockKind k, unsigned level) {
    if (!d || a > b || !boundary(d, a) || !boundary(d, b))
        return false;
    if (k != CE_BLOCK_PARAGRAPH && k != CE_BLOCK_HEADING && k != CE_BLOCK_BULLET &&
        k != CE_BLOCK_ORDERED)
        return fail(d, "Cannot convert text to an atomic block");
    if (k == CE_BLOCK_HEADING && (level < 1 || level > 6))
        return fail(d, "Heading level must be 1–6");
    char *old = begin(d);
    if (!old)
        return false;
    if (!d->nblocks) {
        cJSON *p = paragraph();
        if (!p || !cJSON_AddItemToArray(array(d->root), p) || !rebuild(d))
            return finish(d, old, false);
    }
    size_t first = block_index(d, a), last = last_selected_block(d, a, b);
    for (size_t i = first; i <= last; i++)
        if (!editable(d->blocks[i].node))
            return finish(d, old, fail(d, "Selection contains an atomic or unsupported block"));
    if (k == CE_BLOCK_BULLET || k == CE_BLOCK_ORDERED) {
        for (size_t i = first; i <= last;) {
            cJSON *n = d->blocks[i].node, *item = list_item(d, n);
            if (item) {
                cJSON *list = container_of(d->root, item, 0);
                if (!is_list(list))
                    return finish(d, old, fail(d, "List item has no list container"));
                setstr(list, "type", k == CE_BLOCK_BULLET ? "bulletList" : "orderedList");
                if (k == CE_BLOCK_BULLET)
                    cJSON_DeleteItemFromObjectCaseSensitive(field(list, "attrs"), "start");
                i++;
                continue;
            }
            cJSON *parent = parent_of(d->root, n, 0);
            int insert = index_of(parent, n);
            cJSON *list = node(k == CE_BLOCK_BULLET ? "bulletList" : "orderedList");
            if (!list || insert < 0) {
                cJSON_Delete(list);
                return finish(d, old, false);
            }
            while (i <= last && parent_of(d->root, d->blocks[i].node, 0) == parent &&
                   index_of(parent, d->blocks[i].node) == insert) {
                cJSON *li = node("listItem");
                n = d->blocks[i++].node;
                if (!li) {
                    cJSON_Delete(list);
                    return finish(d, old, false);
                }
                setstr(n, "type", "paragraph");
                cJSON_DeleteItemFromObjectCaseSensitive(field(n, "attrs"), "level");
                cJSON_AddItemToArray(array(li), cJSON_DetachItemFromArray(parent, insert));
                cJSON_AddItemToArray(array(list), li);
            }
            cJSON_InsertItemInArray(parent, insert, list);
        }
        return finish(d, old, true);
    }
    for (size_t i = first; i <= last; i++) {
        cJSON *n = d->blocks[i].node;
        if (!editable(n))
            return finish(d, old, fail(d, "Selection contains an atomic or unsupported block"));
        setstr(n, "type", k == CE_BLOCK_HEADING ? "heading" : "paragraph");
        cJSON *attrs = field(n, "attrs");
        if (!attrs) {
            attrs = cJSON_CreateObject();
            cJSON_AddItemToObject(n, "attrs", attrs);
        }
        if (k == CE_BLOCK_HEADING)
            set(attrs, "level", cJSON_CreateNumber(level));
        else
            cJSON_DeleteItemFromObjectCaseSensitive(attrs, "level");
        cJSON *item;
        while ((item = list_item(d, n)))
            if (!lift_item(d, item))
                return finish(d, old, false);
    }
    return finish(d, old, true);
}
bool ce_document_insert_node(CeDocument *d, size_t offset, const char *json) {
    if (!d || !json || !boundary(d, offset))
        return false;
    for (const char *p = json; *p; p++) {
        if (*p == '\\' && p[1]) {
            if (!strncmp(p + 1, "u0000", 5))
                return fail(d, "Embedded NUL characters are not supported");
            p++;
        }
    }
    cJSON *n = cJSON_ParseWithOpts(json, NULL, true);
    if (!validate_json(n, 0) || istype(n, "doc") || istype(n, "text") ||
        istype(n, "hardBreak") || istype(n, "listItem")) {
        cJSON_Delete(n);
        return fail(d, "Expected a block node JSON object");
    }
    char *old = begin(d);
    if (!old) {
        cJSON_Delete(n);
        return false;
    }
    size_t bi = block_index(d, offset);
    if (!d->nblocks) {
        cJSON_AddItemToArray(array(d->root), n);
        return finish(d, old, true);
    }
    cJSON *ref = d->blocks[bi].node, *p = parent_of(d->root, ref, 0);
    int i = index_of(p, ref);
    CeBlockInfo info = d->blocks[bi].info;
    if (offset > info.start && offset < info.end && !editable(ref)) {
        cJSON_Delete(n);
        return finish(d, old, fail(d, "Cannot insert inside an atomic or unsupported block"));
    }
    bool first_in_item = i == 0 && list_item(d, ref);
    if (editable(ref) && offset < info.end && (offset > info.start || first_in_item)) {
        cJSON *tail = cJSON_Duplicate(ref, true);
        cJSON *left = slice(ref, 0, offset - info.start);
        cJSON *right = slice(ref, offset - info.start, info.end - info.start);
        if (!tail || !left || !right) {
            cJSON_Delete(n);
            cJSON_Delete(tail);
            cJSON_Delete(left);
            cJSON_Delete(right);
            return finish(d, old, false);
        }
        set(ref, "content", left);
        set(tail, "content", right);
        cJSON_InsertItemInArray(p, ++i, n);
        cJSON_InsertItemInArray(p, ++i, tail);
        return finish(d, old, true);
    }
    if (first_in_item && offset == info.start && !istype(n, "paragraph"))
        i++;
    if (offset != d->blocks[bi].info.start)
        i++;
    cJSON_InsertItemInArray(p, i, n);
    return finish(d, old, true);
}
bool ce_document_move_block_ex(CeDocument *d, size_t from, size_t to, size_t *new_offset) {
    if (!d || from >= d->nblocks || to >= d->nblocks)
        return false;
    if (from == to) {
        if (new_offset) *new_offset=d->blocks[from].info.start;
        return true;
    }
    cJSON *moved_block=d->blocks[from].node;
    cJSON *a = d->blocks[from].node, *b = d->blocks[to].node, *p = parent_of(d->root, a, 0),
          *q = parent_of(d->root, b, 0);
    if (p != q) {
        cJSON *left_item = list_item(d, a), *right_item = list_item(d, b);
        if (left_item && right_item && cJSON_GetArrayItem(p, 0) == a &&
            cJSON_GetArrayItem(q, 0) == b) {
            cJSON *list = parent_of(d->root, left_item, 0);
            if (list == parent_of(d->root, right_item, 0)) {
                a = left_item;
                b = right_item;
                p = q = list;
            }
        }
    }
    if (p != q)
        return fail(d, "Moving blocks between nested containers requires an explicit list action");
    char *old = begin(d);
    if (!old)
        return false;
    int i = index_of(p, a), j = index_of(p, b);
    cJSON *n = cJSON_DetachItemFromArray(p, i);
    cJSON_InsertItemInArray(p, j, n);
    if (!finish(d, old, true)) return false;
    if (new_offset)
        for (size_t index=0;index<d->nblocks;index++)
            if (d->blocks[index].node==moved_block) { *new_offset=d->blocks[index].info.start; break; }
    return true;
}
bool ce_document_move_block(CeDocument *d, size_t from, size_t to) {
    return ce_document_move_block_ex(d,from,to,NULL);
}
static unsigned char lower(unsigned char c) { return c >= 'A' && c <= 'Z' ? c + 32 : c; }
bool ce_document_find(const CeDocument *d, const char *term, size_t from, bool back, size_t *a,
                      size_t *b) {
    if (!d || !term || !*term || !utf8(term))
        return false;
    size_t n = strlen(term), chosen = SIZE_MAX, wrap = SIZE_MAX;
    if (n > d->text.n)
        return false;
    for (size_t i = 0; i <= d->text.n - n; i++) {
        size_t j = 0;
        while (j < n && lower((unsigned char)d->text.p[i + j]) == lower((unsigned char)term[j]))
            j++;
        if (j != n)
            continue;
        if (!back) {
            if (wrap == SIZE_MAX)
                wrap = i;
            if (i >= from && chosen == SIZE_MAX)
                chosen = i;
        } else {
            wrap = i;
            if (i < from)
                chosen = i;
        }
    }
    if (chosen == SIZE_MAX)
        chosen = wrap;
    if (chosen == SIZE_MAX)
        return false;
    if (a)
        *a = chosen;
    if (b)
        *b = chosen + n;
    return true;
}

/* HTML migration parser: a deliberately strict subset matching the old TipTap schema.
 * Unsupported tags are rejected rather than flattened and silently overwritten. */
static void codepoint(Buffer *b, uint32_t c) {
    char out[4];
    size_t n;
    if (c < 128) {
        out[0] = (char)c;
        n = 1;
    } else if (c < 2048) {
        out[0] = (char)(0xc0 | (c >> 6));
        out[1] = (char)(0x80 | (c & 63));
        n = 2;
    } else if (c < 65536) {
        out[0] = (char)(0xe0 | (c >> 12));
        out[1] = (char)(0x80 | ((c >> 6) & 63));
        out[2] = (char)(0x80 | (c & 63));
        n = 3;
    } else {
        out[0] = (char)(0xf0 | (c >> 18));
        out[1] = (char)(0x80 | ((c >> 12) & 63));
        out[2] = (char)(0x80 | ((c >> 6) & 63));
        out[3] = (char)(0x80 | (c & 63));
        n = 4;
    }
    addn(b, out, n);
}
static char *decode(const char *s, size_t n) {
    Buffer b = {0};
    for (size_t i = 0; i < n; i++) {
        if (s[i] != '&') {
            addn(&b, s + i, 1);
            continue;
        }
        size_t end = i + 1;
        while (end < n && end - i < 32 && s[end] != ';')
            end++;
        if (end == n || s[end] != ';') {
            addn(&b, s + i, 1);
            continue;
        }
        char *entity = copy_n(s + i + 1, end - i - 1);
        uint32_t cp = 0;
        if (!entity) {
            b.failed = 1;
            break;
        }
        if (!strcmp(entity, "amp"))
            cp = '&';
        else if (!strcmp(entity, "lt"))
            cp = '<';
        else if (!strcmp(entity, "gt"))
            cp = '>';
        else if (!strcmp(entity, "quot"))
            cp = '"';
        else if (!strcmp(entity, "apos") || !strcmp(entity, "#039"))
            cp = '\'';
        else if (!strcmp(entity, "nbsp"))
            cp = 160;
        else if (entity[0] == '#') {
            char *tail;
            unsigned long x = strtoul(entity + (entity[1] == 'x' || entity[1] == 'X' ? 2 : 1),
                                      &tail, entity[1] == 'x' || entity[1] == 'X' ? 16 : 10);
            if (!*tail && x > 0 && x <= 0x10ffff && !(x >= 0xd800 && x <= 0xdfff))
                cp = (uint32_t)x;
        }
        free(entity);
        if (!cp) {
            addn(&b, s + i, end - i + 1);
        } else
            codepoint(&b, cp);
        i = end;
    }
    if (!b.p)
        adds(&b, "");
    if (b.failed) {
        free(b.p);
        return NULL;
    }
    return b.p;
}
static char *name_token(const char **p) {
    const char *start = *p;
    while (isalnum((unsigned char)**p) || **p == '-' || **p == ':' || **p == '_')
        (*p)++;
    if (start == *p)
        return NULL;
    char *s = copy_n(start, (size_t)(*p - start));
    if (s)
        for (char *t = s; *t; t++)
            *t = (char)lower((unsigned char)*t);
    return s;
}
static void space(const char **p) {
    while (isspace((unsigned char)**p))
        (*p)++;
}
static bool blank(const char *s) {
    while (*s)
        if (!isspace((unsigned char)*s++))
            return false;
    return true;
}
static cJSON *html_tree(CeDocument *d, const char *s) {
    cJSON *root = node("html-root"), *stack[CE_DEPTH + 1];
    unsigned depth = 0;
    if (!root)
        goto memory;
    stack[0] = root;
    const char *p = s;
    while (*p) {
        if (*p != '<') {
            const char *start = p;
            while (*p && *p != '<')
                p++;
            char *t = decode(start, (size_t)(p - start));
            if (!t)
                goto memory;
            cJSON *n = node("#text");
            bool ok = n && cJSON_AddStringToObject(n, "text", t) &&
                cJSON_AddItemToArray(array(stack[depth]), n);
            free(t);
            if (!ok) {
                cJSON_Delete(n);
                goto memory;
            }
            continue;
        }
        if (!strncmp(p, "<!--", 4)) {
            const char *end = strstr(p + 4, "-->");
            if (!end)
                goto malformed;
            p = end + 3;
            continue;
        }
        p++;
        bool closing = false;
        if (*p == '/') {
            closing = true;
            p++;
        }
        space(&p);
        char *tag = name_token(&p);
        if (!tag)
            goto malformed;
        if (closing) {
            space(&p);
            if (*p != '>' || !depth || strcmp(type(stack[depth]), tag)) {
                free(tag);
                goto malformed;
            }
            depth--;
            p++;
            free(tag);
            continue;
        }
        cJSON *n = node(tag);
        free(tag);
        cJSON *attrs = cJSON_CreateObject();
        if (!n || !attrs || !cJSON_AddItemToObject(n, "attrs", attrs)) {
            cJSON_Delete(n);
            cJSON_Delete(attrs);
            goto memory;
        }
        bool self = false;
        for (;;) {
            space(&p);
            if (*p == '>') {
                p++;
                break;
            }
            if (*p == '/' && p[1] == '>') {
                self = true;
                p += 2;
                break;
            }
            char *key = name_token(&p);
            if (!key) {
                cJSON_Delete(n);
                goto malformed;
            }
            if (field(attrs, key)) {
                free(key);
                cJSON_Delete(n);
                goto malformed;
            }
            space(&p);
            char *v = NULL;
            if (*p == '=') {
                p++;
                space(&p);
                char quote = 0;
                if (*p == '\'' || *p == '"')
                    quote = *p++;
                const char *start = p;
                if (quote) {
                    while (*p && *p != quote)
                        p++;
                    if (!*p) {
                        free(key);
                        cJSON_Delete(n);
                        goto malformed;
                    }
                } else
                    while (*p && !isspace((unsigned char)*p) && *p != '>')
                        p++;
                v = decode(start, (size_t)(p - start));
                if (quote)
                    p++;
            } else
                v = copy("");
            if (!v) {
                free(key);
                cJSON_Delete(n);
                goto memory;
            }
            bool added = cJSON_AddStringToObject(attrs, key, v) != NULL;
            free(key);
            free(v);
            if (!added) {
                cJSON_Delete(n);
                goto memory;
            }
        }
        if (!cJSON_AddItemToArray(array(stack[depth]), n)) {
            cJSON_Delete(n);
            goto memory;
        }
        if (!strcmp(type(n), "br"))
            self = true;
        if (!self) {
            if (depth == CE_DEPTH)
                goto malformed;
            stack[++depth] = n;
        }
    }
    if (depth)
        goto malformed;
    return root;
malformed:
    fail(d, "Malformed or excessively nested HTML; original document was retained");
    cJSON_Delete(root);
    return NULL;
memory:
    fail(d, "Out of memory");
    cJSON_Delete(root);
    return NULL;
}
static char *css_property(const char *style, const char *key) {
    size_t kn = strlen(key);
    const char *p = style;
    while (*p) {
        space(&p);
        const char *name = p;
        while (*p && *p != ':' && *p != ';')
            p++;
        size_t nn = (size_t)(p - name);
        while (nn && isspace((unsigned char)name[nn - 1]))
            nn--;
        if (*p != ':') {
            if (*p)
                p++;
            continue;
        }
        p++;
        space(&p);
        const char *value = p;
        while (*p && *p != ';')
            p++;
        size_t vn = (size_t)(p - value);
        while (vn && isspace((unsigned char)value[vn - 1]))
            vn--;
        if (nn == kn) {
            bool same = true;
            for (size_t i = 0; i < kn; i++)
                if (lower((unsigned char)name[i]) != (unsigned char)key[i])
                    same = false;
            if (same)
                return copy_n(value, vn);
        }
        if (*p)
            p++;
    }
    return NULL;
}
static bool css_supported(const char *style, const char *allowed1, const char *allowed2) {
    const char *p = style;
    while (*p) {
        space(&p);
        if (!*p)
            break;
        const char *start = p;
        while (isalpha((unsigned char)*p) || *p == '-')
            p++;
        char *name = copy_n(start, (size_t)(p - start));
        if (!name)
            return false;
        for (char *c = name; *c; c++)
            *c = (char)lower((unsigned char)*c);
        space(&p);
        bool ok = *p == ':' && (!strcmp(name, allowed1) || !strcmp(name, allowed2));
        free(name);
        if (!ok)
            return false;
        while (*p && *p != ';')
            p++;
        if (*p)
            p++;
    }
    return true;
}
static bool html_inlines(CeDocument *d, cJSON *h, cJSON *out, cJSON *marks, unsigned depth) {
    if (depth > CE_DEPTH)
        return fail(d, "HTML nesting is too deep");
    const char *tag = type(h);
    if (!strcmp(tag, "#text"))
        return append_text(out, str(h, "text"), strlen(str(h, "text")), marks);
    if (!strcmp(tag, "br"))
        return cJSON_AddItemToArray(out, node("hardBreak"));
    cJSON *next = marks ? cJSON_Duplicate(marks, true) : cJSON_CreateArray(), *m = NULL,
          *ha = field(h, "attrs");
    const char *css1 = !strcmp(tag, "span")   ? "color"
                       : !strcmp(tag, "mark") ? "background-color"
                                              : "";
    const char *css2 = !strcmp(tag, "mark") ? "color" : "";
    if (!css_supported(str(ha, "style"), css1, css2)) {
        cJSON_Delete(next);
        return fail(d, "Unsupported inline CSS; import JSON to preserve the original formatting");
    }
    if (!strcmp(tag, "strong") || !strcmp(tag, "b"))
        m = node("bold");
    else if (!strcmp(tag, "em") || !strcmp(tag, "i"))
        m = node("italic");
    else if (!strcmp(tag, "u"))
        m = node("underline");
    else if (!strcmp(tag, "a")) {
        m = node("link");
        cJSON *attrs = cJSON_CreateObject();
        const char *keys[] = {"href", "target", "rel", "class"};
        for (size_t i = 0; i < 4; i++)
            if (field(ha, keys[i]))
                cJSON_AddStringToObject(attrs, keys[i], str(ha, keys[i]));
        cJSON_AddItemToObject(m, "attrs", attrs);
    } else if (!strcmp(tag, "mark")) {
        m = node("highlight");
        char *color = css_property(str(ha, "style"), "background-color");
        const char *c = *str(ha, "data-color") ? str(ha, "data-color") : color;
        if (c) {
            cJSON *attrs = cJSON_CreateObject();
            cJSON_AddStringToObject(attrs, "color", c);
            cJSON_AddItemToObject(m, "attrs", attrs);
        }
        free(color);
    } else if (!strcmp(tag, "span")) {
        char *color = css_property(str(ha, "style"), "color");
        if (color) {
            m = node("textStyle");
            cJSON *attrs = cJSON_CreateObject();
            cJSON_AddStringToObject(attrs, "color", color);
            cJSON_AddItemToObject(m, "attrs", attrs);
            free(color);
        } else if (*str(ha, "style")) {
            cJSON_Delete(next);
            return fail(
                d, "Unsupported span styling; import JSON to preserve the original formatting");
        }
    } else {
        cJSON_Delete(next);
        return fail(d, "Unsupported inline HTML tag; original document was retained");
    }
    if (m)
        cJSON_AddItemToArray(next, m);
    cJSON *child;
    cJSON_ArrayForEach (child, field(h, "content"))
        if (!html_inlines(d, child, out, next, depth + 1)) {
            cJSON_Delete(next);
            return false;
        }
    cJSON_Delete(next);
    return true;
}
static const char *citation_keys[] = {"citationId", "endnoteNumber", "itemKey", "itemUri",
                                      "pdfUri",     "title",         "author",  "year",
                                      "citeKey",    "page",          "note"};
static bool keys_supported(cJSON *obj, const char *allowed);
/* A document wrapper may only occupy the sole meaningful top-level position.
 * html/body shells and surrounding whitespace are transparent. The full walk
 * also rejects hidden nested wrappers inside atomic nodes whose content is
 * otherwise intentionally ignored by the editable exchange importer. */
static bool html_document_wrapper(CeDocument *d, cJSON *h, bool root_position, cJSON **wrapper) {
    cJSON *attrs = field(h, "attrs"), *children = field(h, "content"), *child;
    const char *tag = type(h), *kind = str(attrs, "data-type");
    bool is_wrapper = !strcmp(kind, "notelabDocument");
    bool atom = !strcmp(tag, "div") && (!strcmp(kind, "zoteroCitationBlock") || !strcmp(kind, "zoteroEndnoteSection"));
    if (is_wrapper) {
        if (!root_position || *wrapper || strcmp(tag, "div") ||
            !keys_supported(attrs, "data-type|data-citation-profile|data-citation-locale"))
            return fail(d, "Ambiguous, nested or unsupported HTML document metadata wrapper");
        *wrapper = h;
    } else if (field(attrs, "data-citation-profile") || field(attrs, "data-citation-locale"))
        return fail(d, "Citation profile and locale belong only to the document wrapper");
    if ((field(attrs, "data-csl-render-json") || field(attrs, "cslrenderjson")) && !atom)
        return fail(d, "Citation render metadata belongs only to citation or endnote blocks");
    if (field(attrs, "data-bibliography-mode") &&
        (strcmp(tag, "div") || strcmp(kind, "zoteroEndnoteSection") ||
         (strcmp(str(attrs, "data-bibliography-mode"), "citations") &&
          strcmp(str(attrs, "data-bibliography-mode"), "selected"))))
        return fail(d, "Unsupported bibliography mode or placement; original document was retained");
    if (field(attrs, "data-selected-items-json") &&
        (strcmp(tag, "div") || strcmp(kind, "zoteroEndnoteSection")))
        return fail(d, "Selected bibliography metadata belongs only to an endnote section");
    bool transparent = root_position && (!strcmp(tag, "html-root") || !strcmp(tag, "html") || !strcmp(tag, "body"));
    size_t meaningful = 0;
    cJSON_ArrayForEach(child, children)
        if (strcmp(type(child), "#text") || !blank(str(child, "text")))
            ++meaningful;
    cJSON_ArrayForEach(child, children)
        if (!html_document_wrapper(d, child, transparent && meaningful == 1, wrapper))
            return false;
    return true;
}
static bool optional_html_metadata(CeDocument *d, cJSON *source, cJSON *attrs,
    const char *key, const char *html_key, const char *legacy_key) {
    cJSON *value = field(source, html_key), *legacy = field(source, legacy_key);
    if (value && legacy && !cJSON_Compare(value, legacy, true))
        return fail(d, "Ambiguous duplicate citation metadata aliases");
    if (!value)
        value = legacy;
    if (value && (!cJSON_IsString(value) || !cJSON_AddStringToObject(attrs, key, value->valuestring)))
        return fail(d, "Out of memory while importing citation metadata");
    return true;
}
static bool html_blocks(CeDocument *d, cJSON *h, cJSON *out, unsigned depth) {
    if (depth > CE_DEPTH)
        return fail(d, "HTML nesting is too deep");
    const char *t = type(h);
    cJSON *ha = field(h, "attrs"), *n = NULL;
    if (!strcmp(t, "html-root") || !strcmp(t, "html") || !strcmp(t, "body")) {
        cJSON *child;
        cJSON_ArrayForEach (child, field(h, "content"))
            if (!html_blocks(d, child, out, depth + 1))
                return false;
        return true;
    }
    if (!strcmp(t, "#text")) {
        if (blank(str(h, "text")))
            return true;
        n = paragraph();
        if (!append_text(array(n), str(h, "text"), strlen(str(h, "text")), NULL)) {
            cJSON_Delete(n);
            return false;
        }
    } else if (!strcmp(t, "p") || (t[0] == 'h' && t[1] >= '1' && t[1] <= '6' && !t[2])) {
        if (!css_supported(str(ha, "style"), "text-align", ""))
            return fail(d, "Unsupported block CSS; original document was retained");
        n = node(t[0] == 'h' ? "heading" : "paragraph");
        cJSON *attrs = cJSON_CreateObject();
        if (t[0] == 'h')
            cJSON_AddNumberToObject(attrs, "level", t[1] - '0');
        char *align = css_property(str(ha, "style"), "text-align");
        if (align) {
            cJSON_AddStringToObject(attrs, "textAlign", align);
            free(align);
        }
        if (attrs->child)
            cJSON_AddItemToObject(n, "attrs", attrs);
        else
            cJSON_Delete(attrs);
        cJSON *child;
        cJSON_ArrayForEach (child, field(h, "content"))
            if (!html_inlines(d, child, array(n), NULL, 0)) {
                cJSON_Delete(n);
                return false;
            }
    } else if (!strcmp(t, "ul") || !strcmp(t, "ol") || !strcmp(t, "li")) {
        n = node(!strcmp(t, "ul") ? "bulletList" : !strcmp(t, "ol") ? "orderedList" : "listItem");
        if (!strcmp(t, "ol") && *str(ha, "start")) {
            cJSON *attrs = cJSON_CreateObject();
            cJSON_AddNumberToObject(attrs, "start", strtol(str(ha, "start"), NULL, 10));
            cJSON_AddItemToObject(n, "attrs", attrs);
        }
        cJSON *child;
        cJSON_ArrayForEach (child, field(h, "content"))
            if (!html_blocks(d, child, array(n), depth + 1)) {
                cJSON_Delete(n);
                return false;
            }
    } else if (!strcmp(t, "div") && (!strcmp(str(ha, "data-type"), "zoteroCitationBlock") ||
                                     !strcmp(str(ha, "data-type"), "zoteroEndnoteSection"))) {
        n = node(str(ha, "data-type"));
        cJSON *attrs = cJSON_CreateObject();
        if (!n || !attrs) {
            cJSON_Delete(n); cJSON_Delete(attrs);
            return fail(d, "Out of memory while importing citation metadata");
        }
        const char *allowed = istype(n, "zoteroCitationBlock") ?
            "data-type|citationid|endnotenumber|itemkey|itemuri|pdfuri|title|author|year|citekey|page|note|"
            "data-citation-id|data-endnote-number|sourceid|libraryid|csljson|data-source-id|data-library-id|data-csl-json|"
            "data-csl-render-json|cslrenderjson" : "data-type|style|entriesjson|data-csl-render-json|cslrenderjson|data-bibliography-mode|data-selected-items-json";
        if (!keys_supported(ha, allowed)) {
            cJSON_Delete(attrs); cJSON_Delete(n);
            return fail(d, "Unknown citation HTML metadata requires JSON to avoid data loss");
        }
        bool metadata_ok = true;
        if (istype(n, "zoteroCitationBlock")) {
            for (size_t i = 0; i < sizeof citation_keys / sizeof *citation_keys; i++) {
                char lowerkey[32];
                snprintf(lowerkey, sizeof lowerkey, "%s", citation_keys[i]);
                for (char *p = lowerkey; *p; p++)
                    *p = (char)lower((unsigned char)*p);
                const char *v = str(ha, lowerkey);
                if (!strcmp(citation_keys[i], "citationId")) {
                    if (field(ha, lowerkey) && field(ha, "data-citation-id") &&
                        strcmp(str(ha, lowerkey), str(ha, "data-citation-id")))
                        metadata_ok = false;
                    if (!*v)
                        v = str(ha, "data-citation-id");
                }
                if (!strcmp(citation_keys[i], "endnoteNumber")) {
                    if (field(ha, lowerkey) && field(ha, "data-endnote-number") &&
                        strcmp(str(ha, lowerkey), str(ha, "data-endnote-number")))
                        metadata_ok = false;
                    if (!*v)
                        v = str(ha, "data-endnote-number");
                    metadata_ok = cJSON_AddNumberToObject(attrs, citation_keys[i], strtol(v, NULL, 10)) && metadata_ok;
                } else
                    metadata_ok = cJSON_AddStringToObject(attrs, citation_keys[i], v) && metadata_ok;
            }
            /* Optional metadata stays absent when unknown. String IDs retain
             * all digits; cslJson retains the original bibliographic snapshot. */
            const char *identity_keys[] = {"sourceId", "libraryId", "cslJson"};
            const char *html_keys[] = {"data-source-id", "data-library-id", "data-csl-json"};
            const char *legacy_keys[] = {"sourceid", "libraryid", "csljson"};
            for (size_t i = 0; i < sizeof identity_keys / sizeof *identity_keys; i++) {
                if (!optional_html_metadata(d, ha, attrs, identity_keys[i], html_keys[i], legacy_keys[i]))
                    metadata_ok = false;
            }
        } else {
            metadata_ok = cJSON_AddStringToObject(attrs, "style", *str(ha, "style") ? str(ha, "style") : "apa") &&
                cJSON_AddStringToObject(attrs, "entriesJson", *str(ha, "entriesjson") ? str(ha, "entriesjson") : "[]");
            if (field(ha, "data-bibliography-mode"))
                metadata_ok = cJSON_AddStringToObject(attrs, "bibliographyMode", str(ha, "data-bibliography-mode")) && metadata_ok;
        }
        if (istype(n, "zoteroEndnoteSection") && field(ha, "data-selected-items-json"))
            metadata_ok = cJSON_AddStringToObject(attrs, "selectedItemsJson", str(ha, "data-selected-items-json")) && metadata_ok;
        metadata_ok = optional_html_metadata(d, ha, attrs, "cslRenderJson", "data-csl-render-json", "cslrenderjson") && metadata_ok;
        if (!metadata_ok || !cJSON_AddItemToObject(n, "attrs", attrs)) {
            cJSON_Delete(attrs); cJSON_Delete(n);
            if (!*d->error)
                fail(d, "Invalid, ambiguous or unallocatable citation HTML metadata");
            return false;
        }
    } else
        return fail(d, "Unsupported block HTML tag; original document was retained");
    if (!cJSON_AddItemToArray(out, n)) {
        cJSON_Delete(n);
        return fail(d, "Out of memory importing HTML block");
    }
    return true;
}
static cJSON *plain_doc(const char *s) {
    cJSON *r = node("doc");
    const char *p = s, *nl;
    do {
        nl = strchr(p, '\n');
        size_t n = nl ? (size_t)(nl - p) : strlen(p);
        if (n && p[n - 1] == '\r')
            n--;
        cJSON *b = paragraph();
        if (!append_text(array(b), p, n, NULL)) {
            cJSON_Delete(r);
            cJSON_Delete(b);
            return NULL;
        }
        cJSON_AddItemToArray(array(r), b);
        if (nl)
            p = nl + 1;
    } while (nl);
    return r;
}
bool ce_document_import(CeDocument *d, const char *data, CeFormat f, char *error, size_t cap) {
    if (!d || !data)
        return false;
    d->error[0] = 0;
    cJSON *r = NULL;
    if (!utf8(data)) {
        fail(d, "Input is not valid UTF-8");
        goto bad;
    }
    const char *p = data;
    space(&p);
    if (f == CE_FORMAT_AUTO) {
        if (*p == '{' || *p == '[')
            f = CE_FORMAT_JSON;
        else if (*p == '<' && (isalpha((unsigned char)p[1]) || p[1] == '!'))
            f = CE_FORMAT_HTML;
        else
            f = CE_FORMAT_TEXT;
    }
    if (f == CE_FORMAT_TEXT)
        r = plain_doc(data);
    else if (f == CE_FORMAT_JSON) {
        /* cJSON strings are NUL terminated; reject JSON escapes that would truncate text. */
        for (const char *q = data; *q; q++)
            if (*q == '\\' && q[1]) {
                if (!strncmp(q + 1, "u0000", 5)) {
                    fail(d, "Embedded NUL characters are not supported");
                    goto bad;
                }
                q++;
            }
        const char *end = NULL;
        cJSON *parsed = cJSON_ParseWithOpts(data, &end, 1);
        if (!parsed) {
            fail(d, "Invalid JSON; original document was retained");
            goto bad;
        }
        cJSON *candidate = istype(parsed, "doc") ? parsed : field(parsed, "doc");
        if (!candidate || !istype(candidate, "doc") || !validate_json(candidate, 0)) {
            cJSON_Delete(parsed);
            fail(d, "Expected a valid TipTap document object");
            goto bad;
        }
        r = candidate == parsed ? parsed : cJSON_Duplicate(candidate, true);
        if (candidate != parsed)
            cJSON_Delete(parsed);
    } else if (f == CE_FORMAT_HTML) {
        cJSON *html = html_tree(d, data);
        if (!html)
            goto bad;
        r = node("doc");
        cJSON *wrapper = NULL;
        bool ok = r && html_document_wrapper(d, html, true, &wrapper);
        if (ok && wrapper) {
            cJSON *attrs = cJSON_CreateObject(), *source = field(wrapper, "attrs");
            const char *keys[] = {"citationProfile", "citationLocale"};
            const char *html_keys[] = {"data-citation-profile", "data-citation-locale"};
            ok = attrs != NULL;
            for (size_t i = 0; i < 2 && ok; ++i)
                if (field(source, html_keys[i]))
                    ok = cJSON_AddStringToObject(attrs, keys[i], str(source, html_keys[i])) != NULL;
            if (ok && attrs->child) {
                ok = cJSON_AddItemToObject(r, "attrs", attrs);
                if (ok) attrs = NULL;
            }
            cJSON_Delete(attrs);
        }
        cJSON *out = ok ? array(r) : NULL;
        ok = ok && out;
        if (ok && wrapper) {
            cJSON *child;
            cJSON_ArrayForEach(child, field(wrapper, "content"))
                if (ok) ok = html_blocks(d, child, out, 0);
        } else if (ok)
            ok = html_blocks(d, html, out, 0);
        cJSON_Delete(html);
        if (!ok) {
            cJSON_Delete(r);
            r = NULL;
            if (!*d->error)
                fail(d, "Out of memory importing document HTML metadata");
            goto bad;
        }
    } else {
        fail(d, "Unknown input format");
        goto bad;
    }
    if (!r) {
        fail(d, "Out of memory");
        goto bad;
    }
    if (!cJSON_GetArraySize(field(r, "content"))) {
        /* Empty document normalization is also part of private construction.
         * Check every attachment so OOM cannot leak a detached node or accept
         * a partially constructed paragraph. */
        cJSON *content = field(r, "content");
        if (!content) {
            content = cJSON_CreateArray();
            if (!content || !cJSON_AddItemToObject(r, "content", content)) {
                cJSON_Delete(content); cJSON_Delete(r); fail(d, "Out of memory"); goto bad;
            }
        }
        cJSON *blank = cJSON_CreateObject(), *children = cJSON_CreateArray();
        if (!blank || !children || !cJSON_AddStringToObject(blank, "type", "paragraph") ||
            !cJSON_AddItemToObject(blank, "content", children)) {
            cJSON_Delete(blank); cJSON_Delete(children); cJSON_Delete(r);
            fail(d, "Out of memory"); goto bad;
        }
        if (!cJSON_AddItemToArray(content, blank)) {
            cJSON_Delete(blank); cJSON_Delete(r); fail(d, "Out of memory"); goto bad;
        }
    }
    CeDocument temp = {0};
    temp.root = r;
    if (!rebuild(&temp)) {
        cJSON_Delete(r);
        free(temp.text.p);
        free(temp.spans);
        free(temp.blocks);
        fail(d, "Unable to build document index");
        goto bad;
    }
    char *saved = print_owned(r);
    if (!saved) {
        cJSON_Delete(r);
        free(temp.text.p); free(temp.spans); free(temp.blocks);
        fail(d, "Unable to prepare imported document savepoint");
        goto bad;
    }
    cJSON_Delete(d->root);
    free(d->text.p);
    free(d->spans);
    free(d->blocks);
    d->root = r;
    d->text = temp.text;
    d->spans = temp.spans;
    d->nspans = temp.nspans;
    d->cspans = temp.cspans;
    d->blocks = temp.blocks;
    d->nblocks = temp.nblocks;
    d->cblocks = temp.cblocks;
    clear_stack(d->undo, &d->nundo);
    clear_stack(d->redo, &d->nredo);
    free(d->saved);
    d->saved = saved;
    ++d->revision; d->last_edit_revision=0;
    if (error && cap)
        *error = 0;
    return true;
bad:
    if (error && cap)
        snprintf(error, cap, "%s", d->error);
    return false;
}
static void escaped(Buffer *b, const char *s) {
    for (; *s; s++) {
        switch (*s) {
        case '&':
            adds(b, "&amp;");
            break;
        case '<':
            adds(b, "&lt;");
            break;
        case '>':
            adds(b, "&gt;");
            break;
        case '"':
            adds(b, "&quot;");
            break;
        case '\'':
            adds(b, "&#39;");
            break;
        default:
            addn(b, s, 1);
        }
    }
}
static void attribute(Buffer *b, const char *k, const char *v) {
    adds(b, " ");
    adds(b, k);
    adds(b, "=\"");
    escaped(b, v);
    adds(b, "\"");
}
static bool keys_supported(cJSON *obj, const char *allowed) {
    cJSON *x;
    cJSON_ArrayForEach (x, obj) {
        if (!x->string)
            return false;
        for (cJSON *previous = obj->child; previous != x; previous = previous->next)
            if (!strcmp(previous->string, x->string))
                return false;
        size_t n = strlen(x->string);
        const char *p = allowed;
        bool found = false;
        while (*p) {
            const char *end = strchr(p, '|');
            size_t len = end ? (size_t)(end - p) : strlen(p);
            if (len == n && !memcmp(p, x->string, n)) {
                found = true;
                break;
            }
            if (!end)
                break;
            p = end + 1;
        }
        if (!found)
            return false;
    }
    return true;
}
static bool html_export(CeDocument *d, cJSON *n, Buffer *b, unsigned depth) {
    if (depth > CE_DEPTH)
        return fail(d, "Document nesting is too deep");
    const char *t = type(n);
    cJSON *attrs = field(n, "attrs");
    if (!keys_supported(n, "type|content|text|marks|attrs"))
        return fail(d, "Additional JSON fields require JSON export to avoid data loss");
    bool atom = !strcmp(t, "zoteroCitationBlock") || !strcmp(t, "zoteroEndnoteSection");
    const char *known = !strcmp(t, "doc")           ? "citationProfile|citationLocale"
                        : !strcmp(t, "paragraph")     ? "textAlign"
                        : !strcmp(t, "heading")     ? "level|textAlign"
                        : !strcmp(t, "orderedList") ? "start"
                                                    : "";
    if (atom)
        known = !strcmp(t, "zoteroCitationBlock") ? "citationId|endnoteNumber|itemKey|itemUri|"
                                                    "pdfUri|title|author|year|citeKey|page|note|"
                                                    "sourceId|libraryId|cslJson|cslRenderJson"
                                                  : "style|entriesJson|cslRenderJson|bibliographyMode|selectedItemsJson";
    if (!keys_supported(attrs, known))
        return fail(d, "Additional node attributes require JSON export to avoid data loss");
    if (!strcmp(t, "text")) {
        cJSON *marks = field(n, "marks"), *m;
        cJSON_ArrayForEach (m, marks) {
            const char *mt = type(m);
            cJSON *a = field(m, "attrs");
            const char *known_mark = !strcmp(mt, "link") ? "href|target|rel|class"
                                     : (!strcmp(mt, "highlight") || !strcmp(mt, "textStyle"))
                                         ? "color"
                                         : "";
            if (!keys_supported(m, "type|attrs") || !keys_supported(a, known_mark))
                return fail(d, "Additional mark attributes require JSON export to avoid data loss");
            if (!strcmp(mt, "bold"))
                adds(b, "<strong>");
            else if (!strcmp(mt, "italic"))
                adds(b, "<em>");
            else if (!strcmp(mt, "underline"))
                adds(b, "<u>");
            else if (!strcmp(mt, "highlight")) {
                adds(b, "<mark");
                if (*str(a, "color"))
                    attribute(b, "data-color", str(a, "color"));
                adds(b, ">");
            } else if (!strcmp(mt, "textStyle")) {
                adds(b, "<span style=\"color: ");
                escaped(b, str(a, "color"));
                adds(b, "\">");
            } else if (!strcmp(mt, "link")) {
                adds(b, "<a");
                const char *keys[] = {"href", "target", "rel", "class"};
                for (size_t i = 0; i < 4; i++)
                    if (field(a, keys[i]))
                        attribute(b, keys[i], str(a, keys[i]));
                adds(b, ">");
            } else
                return fail(d, "Cannot export an unsupported mark to HTML; export JSON instead");
        }
        escaped(b, str(n, "text"));
        for (int i = cJSON_GetArraySize(marks) - 1; i >= 0; i--) {
            m = cJSON_GetArrayItem(marks, i);
            const char *mt = type(m);
            adds(b, !strcmp(mt, "bold")        ? "</strong>"
                    : !strcmp(mt, "italic")    ? "</em>"
                    : !strcmp(mt, "underline") ? "</u>"
                    : !strcmp(mt, "highlight") ? "</mark>"
                    : !strcmp(mt, "link")      ? "</a>"
                                               : "</span>");
        }
        return !b->failed;
    }
    if (!strcmp(t, "hardBreak")) {
        adds(b, "<br>");
        return !b->failed;
    }
    char tag[16] = "";
    if (!strcmp(t, "doc")) {
        if (depth)
            return fail(d, "Nested document roots require JSON export to preserve their structure");
        if (field(attrs, "citationProfile") || field(attrs, "citationLocale")) {
            const char *keys[] = {"citationProfile", "citationLocale"};
            const char *html_keys[] = {"data-citation-profile", "data-citation-locale"};
            adds(b, "<div"); attribute(b, "data-type", "notelabDocument");
            for (size_t i = 0; i < 2; ++i) {
                cJSON *value = field(attrs, keys[i]);
                if (value && !cJSON_IsString(value))
                    return fail(d, "Non-string document citation settings require JSON export");
                if (value)
                    attribute(b, html_keys[i], value->valuestring);
            }
            adds(b, ">");
        }
    } else if (!strcmp(t, "paragraph"))
        strcpy(tag, "p");
    else if (!strcmp(t, "heading")) {
        int level = 1;
        cJSON *l = field(attrs, "level");
        if (cJSON_IsNumber(l) && l->valueint >= 1 && l->valueint <= 6)
            level = l->valueint;
        snprintf(tag, sizeof tag, "h%d", level);
    } else if (!strcmp(t, "bulletList"))
        strcpy(tag, "ul");
    else if (!strcmp(t, "orderedList"))
        strcpy(tag, "ol");
    else if (!strcmp(t, "listItem"))
        strcpy(tag, "li");
    else if (!strcmp(t, "zoteroCitationBlock") || !strcmp(t, "zoteroEndnoteSection")) {
        adds(b, "<div");
        attribute(b, "data-type", t);
        cJSON *a;
        cJSON_ArrayForEach (a, attrs) {
            const char *key = a->string;
            if (!strcmp(t, "zoteroEndnoteSection") && !strcmp(key, "bibliographyMode")) {
                if (!cJSON_IsString(a) || (strcmp(a->valuestring, "citations") && strcmp(a->valuestring, "selected")))
                    return fail(d, "Unsupported bibliography mode requires JSON export to preserve its value");
                key = "data-bibliography-mode";
            }
            if (!strcmp(t, "zoteroEndnoteSection") && !strcmp(key, "selectedItemsJson")) {
                if (!cJSON_IsString(a))
                    return fail(d, "Selected bibliography sources require string metadata for HTML export");
                key = "data-selected-items-json";
            }
            if (!strcmp(key, "cslRenderJson") || (!strcmp(t, "zoteroCitationBlock") &&
                (!strcmp(key, "sourceId") || !strcmp(key, "libraryId") || !strcmp(key,"cslJson")))) {
                if (!cJSON_IsString(a))
                    return fail(d, "Non-string citation metadata requires JSON export to preserve its type");
                key = !strcmp(key, "cslRenderJson") ? "data-csl-render-json" : !strcmp(key, "sourceId") ? "data-source-id"
                    : !strcmp(key,"libraryId") ? "data-library-id" : "data-csl-json";
            }
            if (cJSON_IsString(a))
                attribute(b, key, a->valuestring);
            else if (cJSON_IsNumber(a)) {
                char num[32];
                snprintf(num, sizeof num, "%.17g", a->valuedouble);
                attribute(b, key, num);
            } else
                return fail(d,
                            "Cannot export complex atomic attributes to HTML; export JSON instead");
        }
        adds(b, "></div>");
        return !b->failed;
    } else
        return fail(d, "Cannot export an unsupported node to HTML; export JSON instead");
    if (*tag) {
        adds(b, "<");
        adds(b, tag);
        if (*str(attrs, "textAlign")) {
            adds(b, " style=\"text-align: ");
            escaped(b, str(attrs, "textAlign"));
            adds(b, "\"");
        }
        if (!strcmp(tag, "ol") && cJSON_IsNumber(field(attrs, "start"))) {
            char num[32];
            snprintf(num, sizeof num, "%d", field(attrs, "start")->valueint);
            attribute(b, "start", num);
        }
        adds(b, ">");
    }
    cJSON *child;
    cJSON_ArrayForEach (child, field(n, "content"))
        if (!html_export(d, child, b, depth + 1))
            return false;
    if (*tag) {
        adds(b, "</");
        adds(b, tag);
        adds(b, ">");
    }
    if (!strcmp(t, "doc") && (field(attrs, "citationProfile") || field(attrs, "citationLocale")))
        adds(b, "</div>");
    return !b->failed;
}
char *ce_document_export_html(CeDocument *d) {
    if (!d)
        return NULL;
    Buffer b = {0};
    d->error[0] = 0;
    if (!html_export(d, d->root, &b, 0)) {
        free(b.p);
        return NULL;
    }
    if (!b.p)
        adds(&b, "");
    return b.p;
}
