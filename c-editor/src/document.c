#include "document.h"
#include "cJSON.h"
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
    if (j)
        cJSON_AddStringToObject(j, "type", t);
    return j;
}
static cJSON *array(cJSON *j) {
    cJSON *a = field(j, "content");
    if (!a) {
        a = cJSON_CreateArray();
        cJSON_AddItemToObject(j, "content", a);
    }
    return a;
}
static cJSON *paragraph(void) {
    cJSON *j = node("paragraph");
    if (j)
        array(j);
    return j;
}
static cJSON *empty_root(void) {
    cJSON *j = node("doc");
    if (j)
        cJSON_AddItemToArray(array(j), paragraph());
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
    return d ? cJSON_PrintUnformatted(d->root) : NULL;
}
void ce_document_mark_saved(CeDocument *d) {
    if (d) {
        char *s = ce_document_export_json(d);
        if (s) {
            free(d->saved);
            d->saved = s;
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
static bool restore(CeDocument *d, const char *s) {
    cJSON *j = cJSON_Parse(s);
    if (!j)
        return false;
    cJSON_Delete(d->root);
    d->root = j;
    return rebuild(d);
}
static char *begin(CeDocument *d) {
    d->error[0] = 0;
    char *s = ce_document_export_json(d);
    if (!s)
        fail(d, "Out of memory");
    return s;
}
static bool finish(CeDocument *d, char *before, bool ok) {
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
        push(d->undo, &d->nundo, before);
        clear_stack(d->redo, &d->nredo);
    } else
        free(before);
    free(after);
    return true;
}
bool ce_document_apply_json(CeDocument *d, const char *json) {
    if (!d || !json) return false;
    CeDocument *candidate = ce_document_new();
    if (!candidate) return fail(d, "Out of memory");
    char error[256];
    if (!ce_document_import(candidate, json, CE_FORMAT_JSON, error, sizeof(error))) {
        ce_document_free(candidate);
        return fail(d, error);
    }
    char *before = begin(d);
    if (!before) { ce_document_free(candidate); return false; }
    cJSON *previous = d->root;
    d->root = candidate->root; candidate->root = previous;
    bool result = finish(d, before, true);
    ce_document_free(candidate);
    return result;
}
bool ce_document_can_undo(const CeDocument *d) { return d && d->nundo; }
bool ce_document_can_redo(const CeDocument *d) { return d && d->nredo; }
static bool history(CeDocument *d, bool redo) {
    char **a = redo ? d->redo : d->undo, **b = redo ? d->undo : d->redo;
    size_t *na = redo ? &d->nredo : &d->nundo, *nb = redo ? &d->nundo : &d->nredo;
    if (!*na)
        return false;
    char *now = ce_document_export_json(d);
    if (!now)
        return false;
    if (!restore(d, a[*na - 1])) {
        free(now);
        return false;
    }
    free(a[--*na]);
    push(b, nb, now);
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
    return d && i < d->nblocks ? cJSON_PrintUnformatted(d->blocks[i].node) : NULL;
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
            cJSON_AddStringToObject(n, "text", t);
            free(t);
            cJSON_AddItemToArray(array(stack[depth]), n);
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
        cJSON_AddItemToObject(n, "attrs", attrs);
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
            setstr(attrs, key, v);
            free(key);
            free(v);
        }
        cJSON_AddItemToArray(array(stack[depth]), n);
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
        if (istype(n, "zoteroCitationBlock")) {
            for (size_t i = 0; i < sizeof citation_keys / sizeof *citation_keys; i++) {
                char lowerkey[32];
                snprintf(lowerkey, sizeof lowerkey, "%s", citation_keys[i]);
                for (char *p = lowerkey; *p; p++)
                    *p = (char)lower((unsigned char)*p);
                const char *v = str(ha, lowerkey);
                if (!strcmp(citation_keys[i], "citationId") && !*v)
                    v = str(ha, "data-citation-id");
                if (!strcmp(citation_keys[i], "endnoteNumber")) {
                    if (!*v)
                        v = str(ha, "data-endnote-number");
                    cJSON_AddNumberToObject(attrs, citation_keys[i], strtol(v, NULL, 10));
                } else
                    cJSON_AddStringToObject(attrs, citation_keys[i], v);
            }
        } else {
            cJSON_AddStringToObject(attrs, "style", *str(ha, "style") ? str(ha, "style") : "apa");
            cJSON_AddStringToObject(attrs, "entriesJson",
                                    *str(ha, "entriesjson") ? str(ha, "entriesjson") : "[]");
        }
        cJSON_AddItemToObject(n, "attrs", attrs);
    } else
        return fail(d, "Unsupported block HTML tag; original document was retained");
    return cJSON_AddItemToArray(out, n);
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
        bool ok = html_blocks(d, html, array(r), 0);
        cJSON_Delete(html);
        if (!ok) {
            cJSON_Delete(r);
            r = NULL;
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
    if (!cJSON_GetArraySize(field(r, "content")))
        cJSON_AddItemToArray(array(r), paragraph());
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
    ce_document_mark_saved(d);
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
    const char *known = !strcmp(t, "paragraph")     ? "textAlign"
                        : !strcmp(t, "heading")     ? "level|textAlign"
                        : !strcmp(t, "orderedList") ? "start"
                                                    : "";
    if (atom)
        known = !strcmp(t, "zoteroCitationBlock") ? "citationId|endnoteNumber|itemKey|itemUri|"
                                                    "pdfUri|title|author|year|citeKey|page|note"
                                                  : "style|entriesJson";
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
            if (cJSON_IsString(a))
                attribute(b, a->string, a->valuestring);
            else if (cJSON_IsNumber(a)) {
                char num[32];
                snprintf(num, sizeof num, "%.17g", a->valuedouble);
                attribute(b, a->string, num);
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
