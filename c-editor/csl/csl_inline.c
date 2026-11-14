#include "csl_inline.h"
#include "csl_format.h"
#include "csl_unicode.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Recognized field tags follow the primary citeproc CSL-JSON markup docs;
 * sc is a compatibility alias. This is independently implemented C, not HTML. */
typedef struct Memory Memory;
typedef union Block {
    struct {
        CeCslOutputAllocator allocator;
        Memory *owner;
        size_t bytes;
    } header;
    long double alignment;
    void *pointer;
} Block;
struct Memory {
    CeCslOutputAllocator allocator;
    size_t used;
    bool limit;
};
struct CeCslInline {
    CeCslOutputAllocator allocator;
    char *text;
    size_t length, text_capacity;
    CeCslInlineRun *runs;
    size_t count, run_capacity, payload;
};
static bool fail(char *error, size_t cap, const char *s) {
    if (error && cap)
        snprintf(error, cap, "CSL inline: %s", s);
    return false;
}
static void clear(char *error, size_t cap) {
    if (error && cap)
        *error = 0;
}
static void *default_allocate(void *ctx, size_t n) {
    (void)ctx;
    return malloc(n);
}
static void default_release(void *ctx, void *p) {
    (void)ctx;
    free(p);
}
static bool memory_init(Memory *m, const CeCslOutputAllocator *a, char *error, size_t cap) {
    if (a && (!a->allocate || !a->deallocate))
        return fail(error, cap, "invalid allocator");
    *m = (Memory){.allocator = a ? *a : (CeCslOutputAllocator){NULL, default_allocate, default_release}};
    return true;
}
static void *allocate(void *ctx, size_t n) {
    Memory *m = ctx;
    if (n > CE_CSL_INLINE_MAX_WORKSPACE - sizeof(Block) ||
        n + sizeof(Block) > CE_CSL_INLINE_MAX_WORKSPACE - m->used) {
        m->limit = true;
        return NULL;
    }
    Block *p = m->allocator.allocate(m->allocator.context, n + sizeof(Block));
    if (!p)
        return NULL;
    p->header.allocator = m->allocator;
    p->header.owner = m;
    p->header.bytes = n + sizeof(Block);
    m->used += p->header.bytes;
    return p + 1;
}
static void release(void *ctx, void *value) {
    (void)ctx;
    if (!value)
        return;
    Block *p = (Block *)value - 1;
    if (p->header.owner)
        p->header.owner->used -= p->header.bytes;
    CeCslOutputAllocator a = p->header.allocator;
    a.deallocate(a.context, p);
}
static void detach(void *value) {
    if (!value)
        return;
    Block *p = (Block *)value - 1;
    if (p->header.owner) {
        p->header.owner->used -= p->header.bytes;
        p->header.owner = NULL;
    }
}
static bool allocation_fail(Memory *m, char *e, size_t c) {
    return fail(e, c, m->limit ? "workspace limit exceeded" : "out of memory");
}
static bool valid(const char *s, size_t n, char *e, size_t c) {
    if ((!s && n) || n > CE_CSL_INLINE_MAX_BYTES)
        return fail(e, c, "invalid/oversized text");
    if (ce_csl_unicode_validate(s, n, NULL) != CE_CSL_UNICODE_OK)
        return fail(e, c, "invalid UTF-8");
    for (size_t i = 0; i < n; ++i)
        if ((unsigned char)s[i] < 32 && s[i] != '\t' && s[i] != '\n' && s[i] != '\r')
            return fail(e, c, "NUL/unsupported control");
    return true;
}
static bool same_href(const char *a, const char *b) { return (!a && !b) || (a && b && !strcmp(a, b)); }
static bool same_style(const CeCslInlineRun *a, const CeCslInlineRun *b) {
    return a->format == b->format && a->nocase == b->nocase && same_href(a->href, b->href);
}
static size_t grow(size_t old, size_t need, size_t limit) {
    size_t n = old ? old : 8;
    while (n < need) {
        if (n > limit / 2)
            return limit;
        n *= 2;
    }
    return n;
}
static CeCslInline *create(Memory *m) {
    CeCslInline *t = allocate(m, sizeof(*t));
    if (t)
        *t = (CeCslInline){.allocator = m->allocator};
    return t;
}
void ce_csl_inline_free(CeCslInline *t) {
    if (!t)
        return;
    for (size_t i = 0; i < t->count; ++i)
        release(NULL, (void *)t->runs[i].href);
    release(NULL, t->runs);
    release(NULL, t->text);
    release(NULL, t);
}
static void detach_text(CeCslInline *t) {
    detach(t);
    detach(t->text);
    detach(t->runs);
    for (size_t i = 0; i < t->count; ++i)
        detach((void *)t->runs[i].href);
}
const char *ce_csl_inline_text(const CeCslInline *t, size_t *n) {
    if (n)
        *n = t ? t->length : 0;
    return t ? (t->text ? t->text : "") : NULL;
}
size_t ce_csl_inline_run_count(const CeCslInline *t) { return t ? t->count : 0; }
const CeCslInlineRun *ce_csl_inline_run(const CeCslInline *t, size_t i) {
    return t && i < t->count ? &t->runs[i] : NULL;
}

/* All allocations happen before writing the destination's visible state.
 * Existing payload is copied only on geometric growth. Used for self-concat. */
static bool append_runs(Memory *m, CeCslInline *d, const char *text, size_t size, const CeCslInlineRun *runs,
                        size_t count, char *error, size_t cap) {
    if (!size)
        return true;
    bool merge = d->count && count && same_style(&d->runs[d->count - 1], &runs[0]);
    if (size > CE_CSL_INLINE_MAX_BYTES - d->length ||
        count - (merge ? 1 : 0) > CE_CSL_INLINE_MAX_RUNS - d->count)
        return fail(error, cap, "text/run limit exceeded");
    size_t payload = size;
    for (size_t i = merge ? 1 : 0; i < count; ++i) {
        size_t n = runs[i].href ? strlen(runs[i].href) : 0;
        if (n > CE_CSL_INLINE_MAX_BYTES - payload)
            return fail(error, cap, "link payload limit exceeded");
        payload += n;
    }
    if (payload > CE_CSL_INLINE_MAX_BYTES - d->payload)
        return fail(error, cap, "payload limit exceeded");
    size_t new_count = d->count + count - (merge ? 1 : 0), new_length = d->length + size;
    size_t text_capacity = d->text_capacity, run_capacity = d->run_capacity;
    char *new_text = d->text;
    CeCslInlineRun *new_runs = d->runs;
    if (new_length + 1 > text_capacity) {
        text_capacity = grow(text_capacity, new_length + 1, CE_CSL_INLINE_MAX_BYTES + 1);
        new_text = allocate(m, text_capacity);
        if (!new_text)
            return allocation_fail(m, error, cap);
        if (d->length)
            memcpy(new_text, d->text, d->length);
    }
    if (new_count > run_capacity) {
        run_capacity = grow(run_capacity, new_count, CE_CSL_INLINE_MAX_RUNS);
        new_runs = allocate(m, run_capacity * sizeof(*new_runs));
        if (!new_runs) {
            if (new_text != d->text)
                release(NULL, new_text);
            return allocation_fail(m, error, cap);
        }
        if (d->count)
            memcpy(new_runs, d->runs, d->count * sizeof(*new_runs));
    }
    size_t created = 0, merge_length = merge ? runs[0].length : 0;
    for (size_t i = merge ? 1 : 0; i < count; ++i) {
        CeCslInlineRun run = runs[i];
        run.offset += d->length;
        run.href = NULL;
        if (runs[i].href) {
            size_t n = strlen(runs[i].href) + 1;
            char *link = allocate(m, n);
            if (!link)
                goto failed;
            memcpy(link, runs[i].href, n);
            run.href = link;
        }
        new_runs[d->count + created++] = run;
    }
    /* source text may point into d->text during self-concat. */
    memmove(new_text + d->length, text, size);
    new_text[new_length] = 0;
    if (merge)
        new_runs[d->count - 1].length += merge_length;
    if (new_text != d->text)
        release(NULL, d->text);
    if (new_runs != d->runs)
        release(NULL, d->runs);
    d->text = new_text;
    d->runs = new_runs;
    d->text_capacity = text_capacity;
    d->run_capacity = run_capacity;
    d->length = new_length;
    d->count = new_count;
    d->payload += payload;
    return true;
failed:
    for (size_t i = 0; i < created; ++i)
        release(NULL, (void *)new_runs[d->count + i].href);
    if (new_text != d->text)
        release(NULL, new_text);
    if (new_runs != d->runs)
        release(NULL, new_runs);
    return allocation_fail(m, error, cap);
}
static bool append_piece(Memory *m, CeCslInline *d, const char *s, size_t n, unsigned format,
                         const char *href, bool nocase, char *e, size_t c) {
    CeCslInlineRun run = {0, n, format, href, nocase};
    return append_runs(m, d, s, n, &run, n ? 1 : 0, e, c);
}
bool ce_csl_inline_concat(CeCslInline *d, const CeCslInline *s, char *e, size_t c) {
    clear(e, c);
    if (!d || !s)
        return fail(e, c, "missing concat object");
    Memory m;
    memory_init(&m, &d->allocator, e, c);
    size_t old_count = d->count;
    char *old_text = d->text;
    CeCslInlineRun *old_runs = d->runs;
    if (!append_runs(&m, d, s->text, s->length, s->runs, s->count, e, c))
        return false;
    if (d->text != old_text)
        detach(d->text);
    if (d->runs != old_runs)
        detach(d->runs);
    for (size_t i = old_count; i < d->count; ++i)
        detach((void *)d->runs[i].href);
    return true;
}
bool ce_csl_inline_clone(const CeCslInline *s, const CeCslOutputAllocator *a, CeCslInline **out, char *e,
                         size_t c) {
    clear(e, c);
    Memory m;
    if (!s || !out)
        return fail(e, c, "missing clone argument");
    if (!memory_init(&m, a, e, c))
        return false;
    CeCslInline *t = create(&m);
    if (!t)
        return allocation_fail(&m, e, c);
    if (!append_runs(&m, t, s->text, s->length, s->runs, s->count, e, c)) {
        ce_csl_inline_free(t);
        return false;
    }
    detach_text(t);
    ce_csl_inline_free(*out);
    *out = t;
    return true;
}
static bool scalar_boundary(const CeCslInline *source, size_t offset) {
    return offset == source->length ||
           (offset < source->length && ((unsigned char)source->text[offset] & 0xc0) != 0x80);
}
bool ce_csl_inline_slice(const CeCslInline *source, size_t offset, size_t length,
                         const CeCslOutputAllocator *a, CeCslInline **out, char *e, size_t c) {
    clear(e, c);
    Memory m;
    if (!source || !out)
        return fail(e, c, "missing slice argument");
    if (offset > source->length || length > source->length - offset || !scalar_boundary(source, offset) ||
        !scalar_boundary(source, offset + length))
        return fail(e, c, "slice range must align with UTF-8 scalars");
    if (!memory_init(&m, a, e, c))
        return false;
    CeCslInline *t = create(&m);
    if (!t)
        return allocation_fail(&m, e, c);
    size_t lo = 0, hi = source->count, end = offset + length;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        const CeCslInlineRun *run = &source->runs[mid];
        if (run->offset + run->length <= offset)
            lo = mid + 1;
        else
            hi = mid;
    }
    for (size_t i = lo; i < source->count && offset < end; ++i) {
        const CeCslInlineRun *run = &source->runs[i];
        size_t stop = run->offset + run->length;
        if (stop > end)
            stop = end;
        if (!append_piece(&m, t, source->text + offset, stop - offset, run->format, run->href, run->nocase, e,
                          c)) {
            ce_csl_inline_free(t);
            return false;
        }
        offset = stop;
    }
    detach_text(t);
    ce_csl_inline_free(*out);
    *out = t;
    return true;
}
bool ce_csl_inline_from_output(const CeCslOutput *s, bool nocase, const CeCslOutputAllocator *a,
                               CeCslInline **out, char *e, size_t c) {
    clear(e, c);
    Memory m;
    if (!s || !out)
        return fail(e, c, "missing output conversion argument");
    if (!memory_init(&m, a, e, c))
        return false;
    CeCslInline *t = create(&m);
    if (!t)
        return allocation_fail(&m, e, c);
    for (size_t i = 0; i < ce_csl_output_run_count(s); ++i) {
        const CeCslOutputRun *r = ce_csl_output_run(s, i);
        if (!append_piece(&m, t, r->text, r->byte_length, r->format, r->href, nocase, e, c)) {
            ce_csl_inline_free(t);
            return false;
        }
    }
    detach_text(t);
    ce_csl_inline_free(*out);
    *out = t;
    return true;
}

/* A bounded lexer recognizes only the whitelist, never evaluates attributes. */
typedef enum Tag { NONE, ITALIC, BOLD, CAPS, SUP, SUB, SPAN_CAPS, SPAN_NOCASE, SPAN_UNKNOWN } Tag;
typedef struct Token {
    size_t start, end;
    Tag tag;
    bool close, self_closing, literal;
} Token;
typedef struct Frame {
    Tag tag;
    unsigned format;
    bool nocase;
} Frame;
static bool space(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }
static bool word_equal(const char *s, size_t n, const char *wanted) {
    return strlen(wanted) == n && !memcmp(s, wanted, n);
}
static bool small_caps_style(const char *s, size_t n) {
    size_t at = 0;
    while (at < n && space(s[at]))
        ++at;
    const char *name = "font-variant";
    size_t k = strlen(name);
    if (n - at < k || memcmp(s + at, name, k))
        return false;
    at += k;
    while (at < n && space(s[at]))
        ++at;
    if (at == n || s[at++] != ':')
        return false;
    while (at < n && space(s[at]))
        ++at;
    name = "small-caps";
    k = strlen(name);
    if (n - at < k || memcmp(s + at, name, k))
        return false;
    at += k;
    while (at < n && space(s[at]))
        ++at;
    if (at < n && s[at] == ';')
        ++at;
    while (at < n && space(s[at]))
        ++at;
    return at == n;
}
static Tag span_attribute(const char *s, size_t n) {
    size_t at = 0;
    while (at < n && space(s[at]))
        ++at;
    size_t key = at;
    while (at < n && s[at] >= 'a' && s[at] <= 'z')
        ++at;
    size_t key_end = at;
    while (at < n && space(s[at]))
        ++at;
    if (at == n || s[at++] != '=')
        return SPAN_UNKNOWN;
    while (at < n && space(s[at]))
        ++at;
    if (at == n || (s[at] != '\'' && s[at] != '"'))
        return SPAN_UNKNOWN;
    char quote = s[at++];
    size_t value = at;
    while (at < n && s[at] != quote)
        ++at;
    if (at == n)
        return SPAN_UNKNOWN;
    size_t value_end = at++;
    while (at < n && space(s[at]))
        ++at;
    if (at != n)
        return SPAN_UNKNOWN;
    if (word_equal(s + key, key_end - key, "class") && word_equal(s + value, value_end - value, "nocase"))
        return SPAN_NOCASE;
    if (word_equal(s + key, key_end - key, "style") && small_caps_style(s + value, value_end - value))
        return SPAN_CAPS;
    return SPAN_UNKNOWN;
}
static Token token(const char *s, size_t size, size_t start) {
    Token t = {.start = start, .end = start + 1, .literal = true};
    size_t end = start + 1;
    char quote = 0;
    while (end < size) {
        char c = s[end++];
        if (quote) {
            if (c == quote)
                quote = 0;
        } else if (c == '\'' || c == '"')
            quote = c;
        else if (c == '>')
            break;
    }
    t.end = end;
    if (end == size && s[end - 1] != '>')
        return t;
    size_t at = start + 1, stop = end - 1;
    if (at < stop && s[at] == '/') {
        t.close = true;
        ++at;
    }
    size_t name = at;
    while (at < stop && s[at] >= 'a' && s[at] <= 'z')
        ++at;
    size_t name_end = at;
    while (at < stop && space(s[at]))
        ++at;
    if (stop > at && s[stop - 1] == '/') {
        t.self_closing = true;
        --stop;
        while (stop > at && space(s[stop - 1]))
            --stop;
    }
    if (word_equal(s + name, name_end - name, "span"))
        t.tag = t.close ? SPAN_NOCASE : span_attribute(s + at, stop - at);
    else if (word_equal(s + name, name_end - name, "i"))
        t.tag = ITALIC;
    else if (word_equal(s + name, name_end - name, "b"))
        t.tag = BOLD;
    else if (word_equal(s + name, name_end - name, "sc"))
        t.tag = CAPS;
    else if (word_equal(s + name, name_end - name, "sup"))
        t.tag = SUP;
    else if (word_equal(s + name, name_end - name, "sub"))
        t.tag = SUB;
    if (t.self_closing || (at != stop && (t.close || t.tag < SPAN_CAPS)))
        t.tag = NONE;
    t.literal = t.tag == NONE || t.tag == SPAN_UNKNOWN;
    return t;
}
static bool same_tag(Tag a, Tag b) { return a == b || (a >= SPAN_CAPS && b >= SPAN_CAPS); }
static unsigned flip(unsigned f, Tag tag) {
    if (tag == ITALIC)
        return (f & ~(CE_CSL_ITALIC | CE_CSL_OBLIQUE)) | ((f & CE_CSL_ITALIC) ? 0 : CE_CSL_ITALIC);
    if (tag == BOLD)
        return (f & ~(CE_CSL_BOLD | CE_CSL_LIGHT)) | ((f & CE_CSL_BOLD) ? 0 : CE_CSL_BOLD);
    if (tag == CAPS || tag == SPAN_CAPS)
        return f ^ CE_CSL_SMALL_CAPS;
    if (tag == SUP)
        return (f & ~(CE_CSL_SUPERSCRIPT | CE_CSL_SUBSCRIPT)) | CE_CSL_SUPERSCRIPT;
    if (tag == SUB)
        return (f & ~(CE_CSL_SUPERSCRIPT | CE_CSL_SUBSCRIPT)) | CE_CSL_SUBSCRIPT;
    return f;
}
static bool scan(Memory *m, const char *s, size_t n, unsigned base, CeCslInline *out,
                 CeCslInlineDiagnostic *diagnostic, char *e, size_t c) {
    Frame stack[CE_CSL_INLINE_MAX_DEPTH];
    size_t depth = 0, at = 0, fragments = 0;
    unsigned format = base;
    bool nocase = false;
    while (at < n) {
        size_t start = at;
        while (at < n && s[at] != '<')
            ++at;
        if (at > start) {
            if (++fragments > CE_CSL_INLINE_MAX_FRAGMENTS)
                return fail(e, c, "fragment limit exceeded");
            if (out && !append_piece(m, out, s + start, at - start, format, NULL, nocase, e, c))
                return false;
        }
        if (at == n)
            break;
        Token t = token(s, n, at);
        at = t.end;
        if (++fragments > CE_CSL_INLINE_MAX_FRAGMENTS)
            return fail(e, c, "fragment limit exceeded");
        bool literal = t.literal;
        if (t.tag != NONE) {
            if (t.close) {
                if (!depth || !same_tag(stack[depth - 1].tag, t.tag))
                    diagnostic->malformed_markup = true;
                else {
                    Frame old = stack[--depth];
                    literal = old.tag == SPAN_UNKNOWN;
                    format = old.format;
                    nocase = old.nocase;
                }
            } else {
                if (depth == CE_CSL_INLINE_MAX_DEPTH)
                    return fail(e, c, "nesting depth limit exceeded");
                stack[depth++] = (Frame){t.tag, format, nocase};
                format = flip(format, t.tag);
                nocase |= t.tag == SPAN_NOCASE;
            }
        }
        if (literal) {
            diagnostic->literal_markup = true;
            if (out && !append_piece(m, out, s + t.start, t.end - t.start, format, NULL, nocase, e, c))
                return false;
        }
    }
    for (size_t i = 0; i < depth; ++i)
        if (stack[i].tag != SPAN_UNKNOWN)
            diagnostic->malformed_markup = true;
    return true;
}
bool ce_csl_inline_parse(const char *raw, size_t size, unsigned format, const CeCslOutputAllocator *a,
                         CeCslInline **out, CeCslInlineDiagnostic *diagnostic, char *e, size_t c) {
    clear(e, c);
    Memory m;
    unsigned checked;
    CeCslInlineDiagnostic next = {0};
    if (!out)
        return fail(e, c, "missing parse output");
    if (!valid(raw, size, e, c) || !ce_csl_format_resolve(NULL, format, &checked, e, c))
        return false;
    if (!memory_init(&m, a, e, c))
        return false;
    if (!scan(&m, raw, size, format, NULL, &next, e, c))
        return false;
    CeCslInline *t = create(&m);
    if (!t)
        return allocation_fail(&m, e, c);
    bool ok = next.malformed_markup ? append_piece(&m, t, raw, size, format, NULL, false, e, c)
                                    : scan(&m, raw, size, format, t, &next, e, c);
    if (!ok) {
        ce_csl_inline_free(t);
        return false;
    }
    detach_text(t);
    ce_csl_inline_free(*out);
    *out = t;
    if (diagnostic)
        *diagnostic = next;
    return true;
}

void ce_csl_inline_map_free(CeCslInlineMap *map) {
    if (!map)
        return;
    if (map->spans) {
        if (map->_deallocate)
            map->_deallocate(map->_context, map->spans);
        else
            free(map->spans);
    }
    memset(map, 0, sizeof(*map));
}
static bool map_boundary(const CeCslInlineMap *map, size_t offset, size_t *mapped) {
    if (offset == map->source_size) {
        *mapped = map->output_size;
        return true;
    }
    size_t lo = 0, hi = map->count;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (map->spans[mid].source_offset < offset)
            lo = mid + 1;
        else
            hi = mid;
    }
    if (lo == map->count || map->spans[lo].source_offset != offset)
        return false;
    *mapped = map->spans[lo].output_offset;
    return true;
}
bool ce_csl_inline_map_range(const CeCslInlineMap *map, size_t offset, size_t length, size_t *new_offset,
                             size_t *new_length) {
    size_t start, end;
    if (!map || !new_offset || !new_length || (map->count && !map->spans) || offset > map->source_size ||
        length > map->source_size - offset)
        return false;
    if (!map_boundary(map, offset, &start) || !map_boundary(map, offset + length, &end))
        return false;
    *new_offset = start;
    *new_length = end - start;
    return true;
}
bool ce_csl_inline_case(const CeCslInline *source, CeCslTextCase mode, const char *language,
                        const char *default_locale, const CeCslOutputAllocator *a, CeCslInline **out,
                        CeCslInlineMap *map, char *e, size_t c) {
    clear(e, c);
    Memory m;
    if (!source || !out)
        return fail(e, c, "missing case argument");
    if (!memory_init(&m, a, e, c))
        return false;
    CeCslOutputAllocator shared = {&m, allocate, release};
    CeCslCaseMappedText converted = {0};
    CeCslInlineMap next = {0};
    CeCslInline *t = NULL;
    bool ok = false;
    if (!ce_csl_case_transform_mapped(source->text, source->length, mode, language, default_locale, &shared,
                                      &converted, e, c))
        goto done;
    size_t bytes = 0, run = 0;
    for (size_t i = 0; i < converted.span_count; ++i) {
        const CeCslCaseSpan *s = &converted.spans[i];
        while (run < source->count && s->source_offset >= source->runs[run].offset + source->runs[run].length)
            ++run;
        if (run == source->count) {
            fail(e, c, "invalid source run coverage");
            goto done;
        }
        size_t n = source->runs[run].nocase ? s->source_length : s->output_length;
        if (n > CE_CSL_INLINE_MAX_BYTES - bytes) {
            fail(e, c, "case output limit exceeded");
            goto done;
        }
        bytes += n;
    }
    t = create(&m);
    if (!t)
        goto oom;
    t->text = allocate(&m, bytes + 1);
    t->text_capacity = bytes + 1;
    if (source->count) {
        t->runs = allocate(&m, source->count * sizeof(*t->runs));
        t->run_capacity = source->count;
    }
    if (!t->text || (source->count && !t->runs))
        goto oom;
    if (map && converted.span_count) {
        next.spans = allocate(&m, converted.span_count * sizeof(*next.spans));
        if (!next.spans)
            goto oom;
    }
    next.count = converted.span_count;
    next.source_size = source->length;
    next.output_size = bytes;
    next._deallocate = release;
    size_t span = 0, written = 0;
    for (size_t i = 0; i < source->count; ++i) {
        const CeCslInlineRun *original = &source->runs[i];
        size_t start = written;
        while (span < converted.span_count &&
               converted.spans[span].source_offset < original->offset + original->length) {
            const CeCslCaseSpan *s = &converted.spans[span];
            size_t n = original->nocase ? s->source_length : s->output_length;
            const char *text =
                original->nocase ? source->text + s->source_offset : converted.text.data + s->output_offset;
            if (n)
                memcpy(t->text + written, text, n);
            if (next.spans)
                next.spans[span] = (CeCslCaseSpan){s->source_offset, s->source_length, written, n};
            written += n;
            ++span;
        }
        if (written == start)
            continue;
        CeCslInlineRun item = {start, written - start, original->format, NULL, original->nocase};
        if (t->count && t->runs[t->count - 1].format == item.format &&
            t->runs[t->count - 1].nocase == item.nocase &&
            same_href(t->runs[t->count - 1].href, original->href)) {
            t->runs[t->count - 1].length += item.length;
            continue;
        }
        if (original->href) {
            size_t n = strlen(original->href);
            if (n > CE_CSL_INLINE_MAX_BYTES - bytes - t->payload) {
                fail(e, c, "case link payload limit exceeded");
                goto done;
            }
            char *href = allocate(&m, n + 1);
            if (!href)
                goto oom;
            memcpy(href, original->href, n + 1);
            item.href = href;
            t->payload += n;
        }
        t->runs[t->count++] = item;
    }
    t->text[written] = 0;
    t->length = written;
    t->payload += written;
    /* Free nested case text/maps before detaching the rebuilt owned result.
     * Both sets were charged to the SAME tracker while simultaneously live. */
    ce_csl_case_mapped_free(&converted);
    detach_text(t);
    if (next.spans)
        detach(next.spans);
    ce_csl_inline_free(*out);
    *out = t;
    t = NULL;
    if (map) {
        ce_csl_inline_map_free(map);
        *map = next;
        memset(&next, 0, sizeof(next));
    }
    ok = true;
    goto done;
oom:
    allocation_fail(&m, e, c);
done:
    if (!ok && m.limit)
        allocation_fail(&m, e, c);
    ce_csl_case_mapped_free(&converted);
    ce_csl_inline_free(t);
    ce_csl_inline_map_free(&next);
    return ok;
}
bool ce_csl_inline_strip_periods(const CeCslInline *source, const CeCslOutputAllocator *a, CeCslInline **out,
                                 CeCslInlineMap *map, char *e, size_t c) {
    clear(e, c);
    Memory m;
    if (!source || !out)
        return fail(e, c, "missing strip-periods argument");
    if (!memory_init(&m, a, e, c))
        return false;
    size_t scalars = 0, bytes = source->length;
    /* Source is an immutable validated object. Count scalar starts and ASCII
     * dots without per-scalar allocation or a second full-size text buffer. */
    for (size_t i = 0; i < source->length; ++i) {
        unsigned char ch = (unsigned char)source->text[i];
        if ((ch & 0xc0) != 0x80)
            ++scalars;
        if (ch == '.')
            --bytes;
    }
    CeCslInlineMap next = {
        .count = scalars, .source_size = source->length, .output_size = bytes, ._deallocate = release};
    CeCslInline *t = create(&m);
    bool ok = false;
    if (!t)
        goto oom;
    t->text = allocate(&m, bytes + 1);
    t->text_capacity = bytes + 1;
    t->length = bytes;
    t->payload = bytes;
    if (source->count) {
        t->runs = allocate(&m, source->count * sizeof(*t->runs));
        t->run_capacity = source->count;
    }
    if (!t->text || (source->count && !t->runs))
        goto oom;
    if (map && scalars) {
        next.spans = allocate(&m, scalars * sizeof(*next.spans));
        if (!next.spans)
            goto oom;
    }
    size_t written = 0, span = 0;
    for (size_t i = 0; i < source->count; ++i) {
        const CeCslInlineRun *original = &source->runs[i];
        size_t start = written, at = original->offset, end = at + original->length;
        while (at < end) {
            size_t from = at;
            uint32_t cp;
            if (ce_csl_unicode_next(source->text, end, &at, &cp) != CE_CSL_UNICODE_OK) {
                fail(e, c, "invalid source run coverage");
                goto done;
            }
            size_t length = cp == '.' ? 0 : at - from;
            if (next.spans)
                next.spans[span] = (CeCslCaseSpan){from, at - from, written, length};
            ++span;
            if (length)
                memcpy(t->text + written, source->text + from, length);
            written += length;
        }
        if (written == start)
            continue;
        CeCslInlineRun run = {start, written - start, original->format, original->href, original->nocase};
        if (t->count && same_style(&t->runs[t->count - 1], &run)) {
            t->runs[t->count - 1].length += run.length;
            continue;
        }
        run.href = NULL;
        if (original->href) {
            size_t length = strlen(original->href);
            if (length > CE_CSL_INLINE_MAX_BYTES - t->payload) {
                fail(e, c, "strip-periods link payload limit exceeded");
                goto done;
            }
            char *link = allocate(&m, length + 1);
            if (!link)
                goto oom;
            memcpy(link, original->href, length + 1);
            run.href = link;
            t->payload += length;
        }
        t->runs[t->count++] = run;
    }
    t->text[written] = 0;
    detach_text(t);
    detach(next.spans);
    ce_csl_inline_free(*out);
    *out = t;
    t = NULL;
    if (map) {
        ce_csl_inline_map_free(map);
        *map = next;
        memset(&next, 0, sizeof(next));
    }
    ok = true;
    goto done;
oom:
    allocation_fail(&m, e, c);
done:
    ce_csl_inline_free(t);
    ce_csl_inline_map_free(&next);
    return ok;
}
bool ce_csl_inline_to_output(const CeCslInline *source, CeCslOutput *destination, char *e, size_t c) {
    clear(e, c);
    if (!source || !destination)
        return fail(e, c, "missing final output argument");
    Memory m;
    memory_init(&m, &source->allocator, e, c);
    /* Destination allocation uses its own allocator. Its bounded concat adds
     * <=1 MiB payload, <=131072 sentinels and <=65536 run records (plus the
     * output's geometric array). Reserve 8 MiB for that independent work. */
    m.used = 8 * 1024 * 1024;
    CeCslOutputAllocator shared = {&m, allocate, release};
    CeCslOutput *temporary = ce_csl_output_new(&shared, e, c);
    if (!temporary)
        return allocation_fail(&m, e, c);
    bool ok = true;
    for (size_t i = 0; i < source->count && ok; ++i) {
        const CeCslInlineRun *r = &source->runs[i];
        ok = ce_csl_output_append(temporary, source->text + r->offset, r->length, r->format, r->href, e, c);
    }
    if (ok)
        ok = ce_csl_output_concat(destination, temporary, e, c);
    ce_csl_output_free(temporary);
    if (!ok && m.limit)
        allocation_fail(&m, e, c);
    return ok;
}
