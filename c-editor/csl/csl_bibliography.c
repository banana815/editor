#include "csl_bibliography.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef union Allocation {
    unsigned char padding[32];
    size_t size;
    long double alignment;
    void *pointer;
} Allocation;
struct CeCslBibliographyEntry {
    CeCslOutputAllocator user, allocator;
    CeCslBibliographyOptions options;
    CeCslBibliographyRow *rows;
    CeCslBibliographySource *sources;
    size_t row_count, source_count, item_index, bytes, payload, nodes, live;
    bool limit;
};
typedef struct SpanView {
    size_t offset, length;
    bool found;
} SpanView;
typedef struct Display {
    const CeCslEvalNode *node;
    SpanView span;
} Display;
static void *normal_allocate(void *context, size_t size) {
    (void)context;
    return malloc(size);
}
static void normal_release(void *context, void *memory) {
    (void)context;
    free(memory);
}
static bool fail(char *error, size_t capacity, const char *message) {
    if (error && capacity)
        snprintf(error, capacity, "CSL bibliography: %s", message);
    return false;
}
static bool memory_options(const CeCslOutputAllocator *input, CeCslOutputAllocator *out) {
    *out = input ? *input : (CeCslOutputAllocator){NULL, normal_allocate, normal_release};
    return out->allocate && out->deallocate;
}
static void *entry_allocate(void *context, size_t size) {
    CeCslBibliographyEntry *e = context;
    if (size > CE_CSL_BIBLIOGRAPHY_MAX_WORKSPACE - sizeof(Allocation) ||
        e->live > CE_CSL_BIBLIOGRAPHY_MAX_WORKSPACE - sizeof(Allocation) - size) {
        e->limit = true;
        return NULL;
    }
    Allocation *a = e->user.allocate(e->user.context, sizeof(*a) + size);
    if (!a)
        return NULL;
    a->size = sizeof(*a) + size;
    e->live += a->size;
    return a + 1;
}
static void entry_release(void *context, void *memory) {
    if (!memory)
        return;
    CeCslBibliographyEntry *e = context;
    Allocation *a = (Allocation *)memory - 1;
    e->live -= a->size;
    e->user.deallocate(e->user.context, a);
}
void ce_csl_bibliography_entry_free(CeCslBibliographyEntry *e) {
    if (!e)
        return;
    for (size_t i = 0; i < e->row_count; ++i) {
        ce_csl_output_free((CeCslOutput *)e->rows[i].left.output);
        ce_csl_output_free((CeCslOutput *)e->rows[i].body.output);
    }
    entry_release(e, e->rows);
    entry_release(e, e->sources);
    CeCslOutputAllocator user = e->user;
    user.deallocate(user.context, e);
}
size_t ce_csl_bibliography_entry_text_bytes(const CeCslBibliographyEntry *e) { return e ? e->bytes : 0; }
size_t ce_csl_bibliography_entry_source_item_index(const CeCslBibliographyEntry *e) {
    return e ? e->item_index : SIZE_MAX;
}
const CeCslBibliographyOptions *ce_csl_bibliography_entry_options(const CeCslBibliographyEntry *e) {
    return e ? &e->options : NULL;
}
size_t ce_csl_bibliography_entry_row_count(const CeCslBibliographyEntry *e) { return e ? e->row_count : 0; }
const CeCslBibliographyRow *ce_csl_bibliography_entry_row(const CeCslBibliographyEntry *e, size_t i) {
    return e && i < e->row_count ? &e->rows[i] : NULL;
}
size_t ce_csl_bibliography_entry_source_count(const CeCslBibliographyEntry *e) {
    return e ? e->source_count : 0;
}
const CeCslBibliographySource *ce_csl_bibliography_entry_source(const CeCslBibliographyEntry *e, size_t i) {
    return e && i < e->source_count ? &e->sources[i] : NULL;
}
static bool integer(const char *text, uint32_t default_value, bool positive, uint32_t *out) {
    if (!text) {
        *out = default_value;
        return true;
    }
    if (*text == '+')
        ++text;
    uint32_t value = 0;
    if (!*text)
        return false;
    for (; *text; ++text) {
        if (*text < '0' || *text > '9' || value > (UINT32_MAX - (unsigned)(*text - '0')) / 10)
            return false;
        value = value * 10 + (unsigned)(*text - '0');
    }
    if (positive && !value)
        return false;
    *out = value;
    return true;
}
static bool resolve(const CeCslXmlNode *bibliography, CeCslBibliographyOptions *out, char *error,
                    size_t cap) {
    CeCslBibliographyOptions o = {0};
    const char *value = ce_csl_xml_attribute(bibliography, "second-field-align");
    if (value) {
        if (!strcmp(value, "flush"))
            o.alignment = CE_CSL_BIBLIOGRAPHY_ALIGN_FLUSH;
        else if (!strcmp(value, "margin"))
            o.alignment = CE_CSL_BIBLIOGRAPHY_ALIGN_MARGIN;
        else
            return fail(error, cap, "invalid second-field-align");
    }
    value = ce_csl_xml_attribute(bibliography, "hanging-indent");
    if (value) {
        if (!strcmp(value, "true") || !strcmp(value, "1"))
            o.hanging_indent = true;
        else if (strcmp(value, "false") && strcmp(value, "0"))
            return fail(error, cap, "invalid hanging-indent");
    }
    if (!integer(ce_csl_xml_attribute(bibliography, "line-spacing"), 1, true, &o.line_spacing) ||
        !integer(ce_csl_xml_attribute(bibliography, "entry-spacing"), 1, false, &o.entry_spacing))
        return fail(error, cap, "spacing integer limit exceeded");
    if (o.hanging_indent && o.alignment != CE_CSL_BIBLIOGRAPHY_ALIGN_NONE)
        return fail(error, cap, "unsupported simultaneous hanging-indent and second-field-align");
    *out = o;
    return true;
}
static bool ancestor(const CeCslEvalNode *node, const CeCslEvalNode *target) {
    for (; node; node = ce_csl_eval_node_info(node)->parent)
        if (node == target)
            return true;
    return false;
}
static const CeCslEvalNode *first_field(const CeCslEvalNode *node) {
    for (; node; node = ce_csl_eval_node_info(node)->next) {
        const CeCslEvalNodeInfo *info = ce_csl_eval_node_info(node);
        if (info->kind != CE_CSL_EVAL_CHOOSE)
            return node;
        const CeCslEvalNode *first = first_field(info->first_child);
        if (first)
            return first;
    }
    return NULL;
}
static const CeCslEvalNode *display_parent(const CeCslEvalNode *node) {
    for (; node; node = ce_csl_eval_node_info(node)->parent)
        if (ce_csl_eval_node_info(node)->decoration.display != CE_CSL_EVAL_DISPLAY_NONE)
            return node;
    return NULL;
}
static bool cell(CeCslBibliographyEntry *e, const CeCslInline *text, size_t offset, size_t length,
                 size_t node, CeCslBibliographyCell *out, char *error, size_t cap) {
    CeCslInline *slice = NULL;
    CeCslOutput *output = ce_csl_output_new(&e->allocator, error, cap);
    bool ok = output && ce_csl_inline_slice(text, offset, length, &e->allocator, &slice, error, cap) &&
              ce_csl_inline_to_output(slice, output, error, cap);
    ce_csl_inline_free(slice);
    if (ok) {
        size_t payload = ce_csl_output_text_bytes(output);
        for (size_t i = 0; i < ce_csl_output_run_count(output); ++i) {
            const char *href = ce_csl_output_run(output, i)->href;
            if (href) {
                size_t bytes = strlen(href);
                if (bytes > CE_CSL_BIBLIOGRAPHY_MAX_ENTRY_BYTES - payload) {
                    ok = false;
                    break;
                }
                payload += bytes;
            }
        }
        if (!ok || payload > CE_CSL_BIBLIOGRAPHY_MAX_ENTRY_BYTES - e->payload)
            ok = fail(error, cap, "entry payload limit exceeded");
        else
            e->payload += payload;
    }
    if (!ok) {
        ce_csl_output_free(output);
        return false;
    }
    *out = (CeCslBibliographyCell){output, node, offset, length};
    return true;
}
static int compare_display(const void *a, const void *b) {
    const Display *x = a, *y = b;
    if (x->span.offset != y->span.offset)
        return x->span.offset < y->span.offset ? -1 : 1;
    return x->span.length < y->span.length ? -1 : x->span.length != y->span.length;
}
static bool rows_allocate(CeCslBibliographyEntry *e, size_t count, char *error, size_t cap) {
    if (count > CE_CSL_BIBLIOGRAPHY_MAX_ROWS)
        return fail(error, cap, "row limit exceeded");
    if (!count)
        return true;
    e->rows = entry_allocate(e, count * sizeof(*e->rows));
    if (!e->rows)
        return fail(error, cap, "out of memory for rows");
    memset(e->rows, 0, count * sizeof(*e->rows));
    e->row_count = count;
    return true;
}
static bool display_rows(CeCslBibliographyEntry *e, const CeCslFinalized *finalized, Display *displays,
                         size_t count, char *error, size_t cap) {
    const CeCslInline *text = ce_csl_finalized_inline(finalized);
    const CeCslEvalNode *root = ce_csl_evaluation_root(ce_csl_finalized_evaluation(finalized));
    if (e->options.alignment != CE_CSL_BIBLIOGRAPHY_ALIGN_NONE || e->options.hanging_indent)
        return fail(error, cap, "unsupported explicit display with bibliography alignment or hanging-indent");
    qsort(displays, count, sizeof(*displays), compare_display);
    for (size_t i = 0; i < count; ++i) {
        const CeCslEvalNode *node = displays[i].node;
        if (display_parent(ce_csl_eval_node_info(node)->parent))
            return fail(error, cap, "unsupported nested display");
        for (const CeCslEvalNode *parent = ce_csl_eval_node_info(node)->parent; parent && parent != root;
             parent = ce_csl_eval_node_info(parent)->parent) {
            const CeCslEvalDecoration *d = &ce_csl_eval_node_info(parent)->decoration;
            if ((d->prefix && *d->prefix) || (d->suffix && *d->suffix) || (d->delimiter && *d->delimiter))
                return fail(error, cap, "unsupported display under decorated anonymous container");
        }
        if (i && displays[i].span.offset != displays[i - 1].span.offset + displays[i - 1].span.length)
            return fail(error, cap, "unsupported display gap or overlapping punctuation ranges");
    }
    /* Every emitted leaf must belong to a display; only layout affixes may lie
     * outside the first/last block. Do not silently assign arbitrary text. */
    for (size_t i = 0; i < ce_csl_finalized_span_count(finalized); ++i) {
        const CeCslFinalSpan *s = ce_csl_finalized_span(finalized, i);
        if (s->kind == CE_CSL_FINAL_NODE && s->length && ce_csl_eval_node_content(s->node) &&
            !display_parent(s->node))
            return fail(error, cap, "unsupported uncontrolled bibliography display text");
    }
    size_t rows = 0;
    for (size_t i = 0; i < count; ++i) {
        ++rows;
        if (ce_csl_eval_node_info(displays[i].node)->decoration.display == CE_CSL_EVAL_DISPLAY_LEFT_MARGIN &&
            i + 1 < count &&
            ce_csl_eval_node_info(displays[i + 1].node)->decoration.display ==
                CE_CSL_EVAL_DISPLAY_RIGHT_INLINE)
            ++i;
    }
    if (!rows_allocate(e, rows, error, cap))
        return false;
    size_t row = 0;
    for (size_t i = 0; i < count; ++i, ++row) {
        const CeCslEvalNodeInfo *info = ce_csl_eval_node_info(displays[i].node);
        size_t start = i ? displays[i].span.offset : 0;
        size_t end = displays[i].span.offset + displays[i].span.length;
        bool pair = info->decoration.display == CE_CSL_EVAL_DISPLAY_LEFT_MARGIN && i + 1 < count &&
                    ce_csl_eval_node_info(displays[i + 1].node)->decoration.display ==
                        CE_CSL_EVAL_DISPLAY_RIGHT_INLINE;
        if (pair) {
            e->rows[row].kind = CE_CSL_BIBLIOGRAPHY_TWO_COLUMNS;
            if (!cell(e, text, start, end - start, info->index, &e->rows[row].left, error, cap))
                return false;
            ++i;
            info = ce_csl_eval_node_info(displays[i].node);
            start = displays[i].span.offset;
            end = i + 1 == count ? e->bytes : start + displays[i].span.length;
        } else {
            e->rows[row].kind = info->decoration.display == CE_CSL_EVAL_DISPLAY_INDENT
                                    ? CE_CSL_BIBLIOGRAPHY_INDENT
                                    : CE_CSL_BIBLIOGRAPHY_BLOCK;
            if (i + 1 == count)
                end = e->bytes;
        }
        if (!cell(e, text, start, end - start, info->index, &e->rows[row].body, error, cap))
            return false;
    }
    return true;
}
bool ce_csl_bibliography_entry_build(const CeCslStyle *style, const CeCslFinalized *finalized,
                                     size_t source_item_index, const CeCslOutputAllocator *allocator,
                                     CeCslBibliographyEntry **out, char *error, size_t capacity) {
    if (error && capacity)
        *error = 0;
    CeCslOutputAllocator user;
    const CeCslEvaluation *evaluation = ce_csl_finalized_evaluation(finalized);
    const CeCslEvalNode *root = ce_csl_evaluation_root(evaluation);
    const CeCslEvalNodeInfo *info = ce_csl_eval_node_info(root);
    const CeCslXmlNode *bibliography = ce_csl_style_bibliography(style);
    if (!style || !finalized || !evaluation || !info || !bibliography || !out ||
        source_item_index == SIZE_MAX || info->kind != CE_CSL_EVAL_LAYOUT ||
        info->decoration.cluster_envelope ||
        info->occurrence.source != ce_csl_xml_child(bibliography, "layout") ||
        !memory_options(allocator, &user))
        return fail(error, capacity, "invalid standalone bibliography entry");
    CeCslBibliographyEntry *e = user.allocate(user.context, sizeof(*e));
    if (!e)
        return fail(error, capacity, "out of memory for entry");
    memset(e, 0, sizeof(*e));
    e->user = user;
    e->allocator = (CeCslOutputAllocator){e, entry_allocate, entry_release};
    e->live = sizeof(*e);
    e->item_index = source_item_index;
    const CeCslInline *text = ce_csl_finalized_inline(finalized);
    ce_csl_inline_text(text, &e->bytes);
    e->nodes = ce_csl_evaluation_summary(evaluation)->node_count;
    bool ok = resolve(bibliography, &e->options, error, capacity);
    SpanView *spans = NULL;
    Display *displays = NULL;
    size_t display_count = 0;
    if (!ok)
        goto done;
    if (e->bytes > CE_CSL_BIBLIOGRAPHY_MAX_ENTRY_BYTES || e->nodes > CE_CSL_BIBLIOGRAPHY_MAX_NODES) {
        ok = fail(error, capacity, "entry size/node limit exceeded");
        goto done;
    }
    e->source_count = ce_csl_finalized_span_count(finalized);
    if (e->source_count) {
        e->sources = entry_allocate(e, e->source_count * sizeof(*e->sources));
        displays = entry_allocate(e, e->source_count * sizeof(*displays));
    }
    if (e->nodes)
        spans = entry_allocate(e, e->nodes * sizeof(*spans));
    if ((e->source_count && (!e->sources || !displays)) || (e->nodes && !spans)) {
        ok = fail(error, capacity, "out of memory for source ranges");
        goto done;
    }
    if (spans)
        memset(spans, 0, e->nodes * sizeof(*spans));
    for (size_t i = 0; i < e->source_count; ++i) {
        const CeCslFinalSpan *s = ce_csl_finalized_span(finalized, i);
        const CeCslEvalNodeInfo *n = ce_csl_eval_node_info(s->node);
        if (!n || n->index >= e->nodes || s->offset > e->bytes || s->length > e->bytes - s->offset) {
            ok = fail(error, capacity, "invalid source range");
            goto done;
        }
        e->sources[i] =
            (CeCslBibliographySource){n->index, s->person_index, s->offset, s->length, s->kind, s->display};
        if (s->kind == CE_CSL_FINAL_NODE) {
            spans[n->index] = (SpanView){s->offset, s->length, true};
            if (s->display != CE_CSL_EVAL_DISPLAY_NONE && s->length)
                displays[display_count++] = (Display){s->node, spans[n->index]};
        }
    }
    if (!e->bytes)
        goto done;
    if (display_count) {
        ok = display_rows(e, finalized, displays, display_count, error, capacity);
        goto done;
    }
    if (!rows_allocate(e, 1, error, capacity)) {
        ok = false;
        goto done;
    }
    if (e->options.alignment == CE_CSL_BIBLIOGRAPHY_ALIGN_NONE) {
        e->rows[0].kind = CE_CSL_BIBLIOGRAPHY_FLOW;
        ok = cell(e, text, 0, e->bytes, info->index, &e->rows[0].body, error, capacity);
    } else {
        e->rows[0].kind = CE_CSL_BIBLIOGRAPHY_TWO_COLUMNS;
        const CeCslEvalNode *field = first_field(info->first_child);
        const CeCslEvalNodeInfo *f = ce_csl_eval_node_info(field);
        SpanView left = f ? spans[f->index] : (SpanView){0};
        size_t boundary = left.found ? left.offset + left.length : 0;
        for (size_t i = 0; i < e->source_count; ++i) {
            const CeCslFinalSpan *s = ce_csl_finalized_span(finalized, i);
            if (s->length && !ancestor(s->node, field) && !ancestor(field, s->node) && s->offset < boundary) {
                ok = fail(error, capacity, "unsupported overlapping first-field punctuation range");
                goto done;
            }
        }
        ok = cell(e, text, 0, boundary, f ? f->index : SIZE_MAX, &e->rows[0].left, error, capacity) &&
             cell(e, text, boundary, e->bytes - boundary, SIZE_MAX, &e->rows[0].body, error, capacity);
    }
done:
    entry_release(e, spans);
    entry_release(e, displays);
    if (!ok) {
        if (e->limit)
            fail(error, capacity, "workspace limit exceeded");
        ce_csl_bibliography_entry_free(e);
        return false;
    }
    CeCslBibliographyEntry *old = *out;
    *out = e;
    ce_csl_bibliography_entry_free(old);
    return true;
}

typedef struct Writer {
    CeCslOutputAllocator allocator;
    char *data;
    size_t length, capacity, maximum;
    char *error;
    size_t error_capacity;
} Writer;
static bool reserve(Writer *w, size_t count) {
    if (count > w->maximum - w->length)
        return fail(w->error, w->error_capacity, "export size limit exceeded");
    size_t need = w->length + count + 1;
    if (need <= w->capacity)
        return true;
    size_t capacity = w->capacity ? w->capacity : 256;
    while (capacity < need) {
        if (capacity > (w->maximum + 1) / 2) {
            capacity = w->maximum + 1;
            break;
        }
        capacity *= 2;
    }
    if (capacity == w->maximum)
        ++capacity;
    if (capacity > CE_CSL_BIBLIOGRAPHY_MAX_WORKSPACE - w->capacity)
        return fail(w->error, w->error_capacity, "export workspace limit exceeded");
    char *next = w->allocator.allocate(w->allocator.context, capacity);
    if (!next)
        return fail(w->error, w->error_capacity, "out of memory exporting bibliography");
    if (w->length)
        memcpy(next, w->data, w->length);
    if (w->data)
        w->allocator.deallocate(w->allocator.context, w->data);
    w->data = next;
    w->capacity = capacity;
    w->data[w->length] = 0;
    return true;
}
static bool write_bytes(Writer *w, const char *text, size_t size) {
    if (!reserve(w, size))
        return false;
    if (size)
        memcpy(w->data + w->length, text, size);
    w->length += size;
    w->data[w->length] = 0;
    return true;
}
static bool write_text(Writer *w, const char *text) { return write_bytes(w, text, strlen(text)); }
static bool repeated(Writer *w, char byte, size_t count) {
    if (!reserve(w, count))
        return false;
    if (count)
        memset(w->data + w->length, byte, count);
    w->length += count;
    w->data[w->length] = 0;
    return true;
}
static bool finish(Writer *w, bool ok, CeCslOutputString *out) {
    if (ok)
        ok = reserve(w, 0);
    if (!ok) {
        if (w->data)
            w->allocator.deallocate(w->allocator.context, w->data);
        return false;
    }
    CeCslOutputString value = {w->data, w->length, w->allocator.deallocate, w->allocator.context};
    ce_csl_output_string_free(out);
    *out = value;
    return true;
}
static bool same_options(CeCslBibliographyOptions a, CeCslBibliographyOptions b) {
    return a.alignment == b.alignment && a.hanging_indent == b.hanging_indent &&
           a.line_spacing == b.line_spacing && a.entry_spacing == b.entry_spacing;
}
static bool array_validate(const CeCslBibliographyEntry *const *entries, size_t count,
                           CeCslBibliographyOptions *options, size_t *visible, char *error, size_t cap) {
    if ((!entries && count) || count > CE_CSL_BIBLIOGRAPHY_MAX_ENTRIES)
        return fail(error, cap, "invalid bibliography array");
    *options = (CeCslBibliographyOptions){.line_spacing = 1, .entry_spacing = 1};
    size_t bytes = 0, rows = 0, nodes = 0;
    *visible = 0;
    for (size_t i = 0; i < count; ++i) {
        const CeCslBibliographyEntry *e = entries[i];
        if (!e)
            return fail(error, cap, "NULL bibliography entry");
        if (!i)
            *options = e->options;
        else if (!same_options(*options, e->options))
            return fail(error, cap, "incompatible bibliography options");
        if (e->bytes > CE_CSL_BIBLIOGRAPHY_MAX_TEXT_BYTES - bytes ||
            e->row_count > CE_CSL_BIBLIOGRAPHY_MAX_ROWS - rows ||
            e->nodes > CE_CSL_BIBLIOGRAPHY_MAX_NODES - nodes)
            return fail(error, cap, "aggregate bibliography limit exceeded");
        bytes += e->bytes;
        rows += e->row_count;
        nodes += e->nodes;
        if (e->bytes)
            ++*visible;
    }
    return true;
}
static bool plain_cell(Writer *w, const CeCslOutput *cell_output) {
    for (size_t i = 0; i < ce_csl_output_run_count(cell_output); ++i) {
        const CeCslOutputRun *run = ce_csl_output_run(cell_output, i);
        if (!write_bytes(w, run->text, run->byte_length))
            return false;
    }
    return true;
}
bool ce_csl_bibliography_plain(const CeCslBibliographyEntry *const *entries, size_t count,
                               CeCslBibliographyPlainMode mode, const CeCslOutputAllocator *allocator,
                               CeCslOutputString *out, char *error, size_t capacity) {
    if (error && capacity)
        *error = 0;
    Writer w = {.maximum = CE_CSL_BIBLIOGRAPHY_MAX_TEXT_BYTES, .error = error, .error_capacity = capacity};
    CeCslBibliographyOptions options;
    size_t visible;
    if (!out || !memory_options(allocator, &w.allocator) ||
        (mode != CE_CSL_BIBLIOGRAPHY_PLAIN_TABS && mode != CE_CSL_BIBLIOGRAPHY_PLAIN_COMPACT))
        return fail(error, capacity, "invalid plain export options");
    if (!array_validate(entries, count, &options, &visible, error, capacity))
        return false;
    (void)visible;
    bool ok = true, prior = false;
    for (size_t i = 0; i < count && ok; ++i) {
        const CeCslBibliographyEntry *e = entries[i];
        if (!e->bytes)
            continue;
        if (prior) {
            uint64_t lines = (uint64_t)options.entry_spacing + 1;
            if (lines > CE_CSL_BIBLIOGRAPHY_MAX_TEXT_BYTES) {
                ok = fail(error, capacity, "plain spacing limit exceeded");
                break;
            }
            ok = repeated(&w, '\n', (size_t)lines);
        }
        for (size_t j = 0; j < e->row_count && ok; ++j) {
            const CeCslBibliographyRow *row = &e->rows[j];
            if (j)
                ok = write_text(&w, "\n");
            if (ok)
                ok = plain_cell(&w, row->left.output);
            if (ok && ce_csl_output_text_bytes(row->left.output) &&
                ce_csl_output_text_bytes(row->body.output))
                ok = write_text(&w, mode == CE_CSL_BIBLIOGRAPHY_PLAIN_TABS ? "\t" : " ");
            if (ok)
                ok = plain_cell(&w, row->body.output);
        }
        prior = true;
    }
    return finish(&w, ok, out);
}
static bool escaped(Writer *w, const char *text, size_t size) {
    size_t start = 0;
    for (size_t i = 0; i < size; ++i) {
        const char *entity = NULL;
        switch (text[i]) {
        case '&':
            entity = "&amp;";
            break;
        case '<':
            entity = "&lt;";
            break;
        case '>':
            entity = "&gt;";
            break;
        case '"':
            entity = "&quot;";
            break;
        case '\'':
            entity = "&#39;";
            break;
        case '\r':
            entity = "&#13;";
            break;
        default:
            break;
        }
        if (entity) {
            if (!write_bytes(w, text + start, i - start) || !write_text(w, entity))
                return false;
            start = i + 1;
        }
    }
    return write_bytes(w, text + start, size - start);
}
static bool css_units(Writer *w, uint64_t milli) {
    char value[64];
    int length = snprintf(value, sizeof(value), "%llu.%03lluem", (unsigned long long)(milli / 1000),
                          (unsigned long long)(milli % 1000));
    return length > 0 && (size_t)length < sizeof(value) && write_bytes(w, value, (size_t)length);
}
static bool html_cell(Writer *w, const CeCslOutput *output) {
    for (size_t i = 0; i < ce_csl_output_run_count(output); ++i) {
        const CeCslOutputRun *run = ce_csl_output_run(output, i);
        unsigned format = run->format;
        if (run->href && (!write_text(w, "<a href=\"") || !escaped(w, run->href, strlen(run->href)) ||
                          !write_text(w, "\" rel=\"noopener noreferrer\">")))
            return false;
        if (format) {
            if (!write_text(w, "<span style=\""))
                return false;
            if ((format & CE_CSL_BOLD) && !write_text(w, "font-weight:700;"))
                return false;
            if ((format & CE_CSL_LIGHT) && !write_text(w, "font-weight:300;"))
                return false;
            if ((format & CE_CSL_ITALIC) && !write_text(w, "font-style:italic;"))
                return false;
            if ((format & CE_CSL_OBLIQUE) && !write_text(w, "font-style:oblique;"))
                return false;
            if ((format & CE_CSL_UNDERLINE) && !write_text(w, "text-decoration:underline;"))
                return false;
            if ((format & CE_CSL_SMALL_CAPS) && !write_text(w, "font-variant:small-caps;"))
                return false;
            if ((format & CE_CSL_SUPERSCRIPT) && !write_text(w, "vertical-align:super;font-size:75%;"))
                return false;
            if ((format & CE_CSL_SUBSCRIPT) && !write_text(w, "vertical-align:sub;font-size:75%;"))
                return false;
            if (!write_text(w, "\">"))
                return false;
        }
        if (!escaped(w, run->text, run->byte_length))
            return false;
        if (format && !write_text(w, "</span>"))
            return false;
        if (run->href && !write_text(w, "</a>"))
            return false;
    }
    return true;
}
static bool html_td(Writer *w, const CeCslBibliographyRow *row, bool left, bool hanging, uint32_t indent,
                    uint32_t gap, uint64_t spacing) {
    const char *column = row->kind == CE_CSL_BIBLIOGRAPHY_TWO_COLUMNS ? "" : " colspan=\"2\"";
    if (!write_text(w, "<td") || !write_text(w, column) ||
        !write_text(w, left ? " class=\"csl-left-margin\" "
                              "style=\"vertical-align:top;white-space:pre;padding:0;padding-inline-end:"
                            : " class=\"csl-body\" "
                              "style=\"vertical-align:top;white-space:pre-wrap;padding:0;overflow-wrap:"
                              "anywhere;padding-inline-end:"))
        return false;
    if (!css_units(w, left ? gap : 0) || !write_text(w, ";padding-bottom:") || !css_units(w, spacing) ||
        !write_text(w, ";\"><div style=\""))
        return false;
    if (!left && (hanging || row->kind == CE_CSL_BIBLIOGRAPHY_INDENT)) {
        if (!write_text(w, "padding-inline-start:") || !css_units(w, indent) || !write_text(w, ";"))
            return false;
        if (hanging && (!write_text(w, "text-indent:-") || !css_units(w, indent) || !write_text(w, ";")))
            return false;
    }
    return write_text(w, "\">") && html_cell(w, left ? row->left.output : row->body.output) &&
           write_text(w, "</div></td>");
}
bool ce_csl_bibliography_html(const CeCslBibliographyEntry *const *entries, size_t count,
                              const CeCslBibliographyHtmlOptions *input,
                              const CeCslOutputAllocator *allocator, CeCslOutputString *out, char *error,
                              size_t capacity) {
    if (error && capacity)
        *error = 0;
    Writer w = {.maximum = CE_CSL_BIBLIOGRAPHY_MAX_HTML_BYTES, .error = error, .error_capacity = capacity};
    CeCslBibliographyOptions options;
    size_t visible;
    if (!out || !memory_options(allocator, &w.allocator))
        return fail(error, capacity, "invalid HTML export options");
    if (!array_validate(entries, count, &options, &visible, error, capacity))
        return false;
    CeCslBibliographyHtmlOptions metrics = input ? *input : (CeCslBibliographyHtmlOptions){0};
    if (!metrics.indent_milli_em)
        metrics.indent_milli_em = 2000;
    if (!metrics.column_gap_milli_em)
        metrics.column_gap_milli_em = 500;
    if (!metrics.base_line_height_milli_em)
        metrics.base_line_height_milli_em = 1200;
    if (metrics.indent_milli_em > 1024000 || metrics.column_gap_milli_em > 1024000 ||
        metrics.base_line_height_milli_em > 32000 || metrics.label_width_milli_em > 1024000)
        return fail(error, capacity, "HTML presentation metric limit exceeded");
    if (visible && options.alignment == CE_CSL_BIBLIOGRAPHY_ALIGN_MARGIN && !metrics.label_width_milli_em)
        return fail(error, capacity, "unsupported margin HTML without a measured label width");
    uint64_t line_height = (uint64_t)metrics.base_line_height_milli_em * options.line_spacing;
    if (options.entry_spacing && line_height > UINT64_MAX / options.entry_spacing)
        return fail(error, capacity, "HTML spacing multiplication limit exceeded");
    uint64_t entry_spacing = line_height * options.entry_spacing;
    if (!visible)
        return finish(&w, true, out);
    bool columns = false;
    for (size_t i = 0; i < count; ++i)
        for (size_t j = 0; j < entries[i]->row_count; ++j)
            if (entries[i]->rows[j].kind == CE_CSL_BIBLIOGRAPHY_TWO_COLUMNS)
                columns = true;
    uint64_t label_column = (uint64_t)metrics.label_width_milli_em + metrics.column_gap_milli_em;
    bool margin = options.alignment == CE_CSL_BIBLIOGRAPHY_ALIGN_MARGIN;
    bool ok = write_text(&w, "<table role=\"presentation\" class=\"csl-bibliography\" "
                             "style=\"border-collapse:collapse;border:0;table-layout:auto;line-height:") &&
              css_units(&w, line_height);
    if (ok && margin)
        ok = write_text(&w, ";margin-inline-start:-") && css_units(&w, label_column) &&
             write_text(&w, ";width:calc(100% + ") && css_units(&w, label_column) && write_text(&w, ")");
    else if (ok)
        ok = write_text(&w, ";width:100%");
    if (ok)
        ok = write_text(&w, ";\"><colgroup><col style=\"width:");
    if (ok)
        ok = columns && metrics.label_width_milli_em ? css_units(&w, label_column)
                                                     : write_text(&w, columns ? "1px" : "0");
    if (ok)
        ok = write_text(&w, "\"><col></colgroup>");
    size_t emitted = 0;
    for (size_t i = 0; i < count && ok; ++i) {
        const CeCslBibliographyEntry *e = entries[i];
        if (!e->bytes)
            continue;
        char source[80];
        int length =
            snprintf(source, sizeof(source), "<tbody data-source-item-index=\"%zu\">", e->item_index);
        ok = length > 0 && (size_t)length < sizeof(source) && write_bytes(&w, source, (size_t)length);
        ++emitted;
        for (size_t j = 0; j < e->row_count && ok; ++j) {
            const CeCslBibliographyRow *row = &e->rows[j];
            uint64_t spacing = j + 1 == e->row_count && emitted != visible ? entry_spacing : 0;
            ok = write_text(&w, "<tr>");
            if (ok && row->kind == CE_CSL_BIBLIOGRAPHY_TWO_COLUMNS)
                ok = html_td(&w, row, true, false, metrics.indent_milli_em, metrics.column_gap_milli_em,
                             spacing);
            if (ok)
                ok = html_td(&w, row, false, options.hanging_indent, metrics.indent_milli_em,
                             metrics.column_gap_milli_em, spacing) &&
                     write_text(&w, "</tr>");
        }
        if (ok)
            ok = write_text(&w, "</tbody>");
    }
    if (ok)
        ok = write_text(&w, "</table>");
    return finish(&w, ok, out);
}
