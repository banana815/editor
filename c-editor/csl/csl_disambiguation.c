#include "csl_disambiguation.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef union AllocationHeader {
    long double alignment;
    void *pointer;
    size_t size;
    unsigned char padding[32];
} AllocationHeader;
typedef struct Memory {
    CeCslOutputAllocator base;
    size_t live, limit;
    bool limit_hit;
} Memory;
struct CeCslDisambiguation {
    Memory memory;
    CeCslDisambiguationItem *items;
    size_t *item_rows;
    size_t count, item_count, unresolved;
};
typedef struct Driver {
    CeCslDisambiguation *result;
    CeCslOutputAllocator memory;
    CeCslDisambiguationOptions options;
    CeCslDisambiguationProvider provider;
    const CeCslInput *input;
    CeCslDisambiguationProbe **probes;
    CeCslDisambiguationProfile profiles[3];
    size_t profile_count, calls, names, emitted, compared, changes, work;
    size_t order_capacity;
    size_t *parents, *sizes, *order, *scratch, *members;
    size_t *versions, *failed_versions, *failed_groups;
    bool *dirty;
    CeCslDisambiguationItem *saved;
    bool *saved_rows;
    size_t *saved_versions;
    bool transaction;
    CeCslDisambiguationStatus status;
    char *error;
    size_t capacity;
} Driver;

static void *default_allocate(void *context, size_t size) {
    (void)context; return malloc(size);
}
static void default_deallocate(void *context, void *pointer) {
    (void)context; free(pointer);
}
static void *tracked_allocate(void *context, size_t size) {
    Memory *m = context;
    m->limit_hit = false;
    if (size > SIZE_MAX - sizeof(AllocationHeader) ||
        size + sizeof(AllocationHeader) > m->limit - m->live) {
        m->limit_hit = true; return NULL;
    }
    size_t bytes = size + sizeof(AllocationHeader);
    AllocationHeader *h = m->base.allocate(m->base.context, bytes);
    if (!h) return NULL;
    h->size = bytes; m->live += bytes;
    return h + 1;
}
static void tracked_deallocate(void *context, void *pointer) {
    if (!pointer) return;
    Memory *m = context;
    AllocationHeader *h = (AllocationHeader *)pointer - 1;
    m->live -= h->size;
    m->base.deallocate(m->base.context, h);
}
static void item_clear(Memory *memory, CeCslDisambiguationItem *item) {
    for (size_t i = 0; i < item->adjustment_count; ++i) {
        tracked_deallocate(memory, (void *)item->adjustments[i].occurrence.macro_calls);
        tracked_deallocate(memory, (void *)item->adjustments[i].overrides);
    }
    tracked_deallocate(memory, (void *)item->adjustments);
    memset(item, 0, sizeof(*item));
}
void ce_csl_disambiguation_free(CeCslDisambiguation *result) {
    if (!result) return;
    Memory *m = &result->memory;
    if (result->items)
        for (size_t i = 0; i < result->count; ++i) item_clear(m, &result->items[i]);
    tracked_deallocate(m, result->items);
    tracked_deallocate(m, result->item_rows);
    m->base.deallocate(m->base.context, result);
}
const CeCslDisambiguationItem *ce_csl_disambiguation_item(
    const CeCslDisambiguation *result, size_t index) {
    if (!result || index >= result->item_count) return NULL;
    size_t row = result->item_rows[index];
    return row < result->count ? &result->items[row] : NULL;
}
size_t ce_csl_disambiguation_unresolved_count(const CeCslDisambiguation *result) {
    return result ? result->unresolved : 0;
}
static bool occurrence_equal(const CeCslEvalOccurrence *a, const CeCslEvalOccurrence *b) {
    if (a->source != b->source || a->macro_depth != b->macro_depth) return false;
    for (size_t i = 0; i < a->macro_depth; ++i)
        if (a->macro_calls[i] != b->macro_calls[i]) return false;
    return true;
}
bool ce_csl_disambiguation_adjust_names(const CeCslDisambiguationItem *item,
    CeCslDisambiguationProfile profile, const CeCslEvalOccurrence *occurrence,
    CeCslEvalNameAdjustment *out) {
    if (!item || !occurrence || !out ||
        (occurrence->macro_depth && !occurrence->macro_calls)) return false;
    CeCslEvalNameAdjustment value = {0};
    for (size_t i = 0; i < item->adjustment_count; ++i) {
        const CeCslDisambiguationAdjustment *a = &item->adjustments[i];
        if (a->profile == profile && occurrence_equal(&a->occurrence, occurrence)) {
            value.minimum_names = a->minimum_names;
            value.overrides = a->overrides; value.override_count = a->override_count;
            break;
        }
    }
    *out = value; return true;
}
static bool stop(Driver *d, CeCslDisambiguationStatus status, const char *message) {
    if (d->status == CE_CSL_DISAMBIGUATION_OK) {
        d->status = status;
        if (d->error && d->capacity) snprintf(d->error, d->capacity, "%s", message);
    }
    return false;
}
static bool charge(Driver *d, size_t *used, size_t amount, size_t limit, const char *message) {
    if (d->status != CE_CSL_DISAMBIGUATION_OK) return false;
    if (amount > limit - *used) return stop(d, CE_CSL_DISAMBIGUATION_LIMIT, message);
    *used += amount; return true;
}
static bool work(Driver *d, size_t amount) {
    if (d->options.cancelled && d->options.cancelled(d->options.cancel_context))
        return stop(d, CE_CSL_DISAMBIGUATION_CANCELLED, "Citation disambiguation cancelled");
    return charge(d, &d->work, amount, d->options.max_work_units,
                  "Citation disambiguation work limit exceeded");
}
static void *allocate(Driver *d, size_t size) {
    void *p = tracked_allocate(&d->result->memory, size);
    if (!p) stop(d, d->result->memory.limit_hit ? CE_CSL_DISAMBIGUATION_LIMIT :
        CE_CSL_DISAMBIGUATION_OOM, "Cannot allocate bounded citation disambiguation workspace");
    return p;
}
static void *zero_array(Driver *d, size_t count, size_t size) {
    if (!count) return NULL;
    if (size > SIZE_MAX / count) {
        stop(d, CE_CSL_DISAMBIGUATION_LIMIT, "Citation disambiguation array size overflow"); return NULL;
    }
    void *p = allocate(d, count * size);
    if (p) memset(p, 0, count * size);
    return p;
}
static bool occurrence_copy(Driver *d, const CeCslEvalOccurrence *source, CeCslEvalOccurrence *out) {
    *out = *source; out->macro_calls = NULL;
    if (source->macro_depth) {
        const CeCslXmlNode **calls = allocate(d, source->macro_depth * sizeof(*calls));
        if (!calls) return false;
        memcpy(calls, source->macro_calls, source->macro_depth * sizeof(*calls));
        out->macro_calls = calls;
    }
    return true;
}
static bool item_copy(Driver *d, const CeCslDisambiguationItem *source, CeCslDisambiguationItem *out) {
    *out = *source; out->adjustments = NULL; out->adjustment_count = 0;
    if (!source->adjustment_count) return true;
    CeCslDisambiguationAdjustment *a = zero_array(d, source->adjustment_count, sizeof(*a));
    if (!a) return false;
    out->adjustments = a;
    for (size_t i = 0; i < source->adjustment_count; ++i) {
        a[i] = source->adjustments[i]; a[i].overrides = NULL; a[i].occurrence.macro_calls = NULL;
        out->adjustment_count = i + 1;
        if (!occurrence_copy(d, &source->adjustments[i].occurrence, &a[i].occurrence)) return false;
        if (a[i].override_count) {
            CeCslNameOverride *v = allocate(d, a[i].override_count * sizeof(*v));
            if (!v) return false;
            memcpy(v, source->adjustments[i].overrides, a[i].override_count * sizeof(*v));
            a[i].overrides = v;
        }
    }
    return true;
}
static bool capture(Driver *d, size_t row) {
    if (!d->transaction || d->saved_rows[row]) return true;
    d->saved_rows[row] = true; d->saved_versions[row] = d->versions[row];
    return item_copy(d, &d->result->items[row], &d->saved[row]);
}
static bool changed(Driver *d, size_t row) {
    if (!charge(d, &d->changes, 1, d->options.max_changes,
                "Citation disambiguation state-change limit exceeded")) return false;
    d->dirty[row] = true; ++d->versions[row]; return true;
}
static CeCslDisambiguationAdjustment *adjustment(Driver *d, size_t row,
    CeCslDisambiguationProfile profile, const CeCslEvalOccurrence *occurrence) {
    if (!capture(d, row)) return NULL;
    CeCslDisambiguationItem *item = &d->result->items[row];
    for (size_t i = 0; i < item->adjustment_count; ++i) {
        if (!work(d, 1)) return NULL;
        if (item->adjustments[i].profile == profile &&
            occurrence_equal(&item->adjustments[i].occurrence, occurrence))
            return (CeCslDisambiguationAdjustment *)&item->adjustments[i];
    }
    size_t count = item->adjustment_count;
    CeCslDisambiguationAdjustment *a = zero_array(d, count + 1, sizeof(*a));
    if (!a) return NULL;
    if (count) memcpy(a, item->adjustments, count * sizeof(*a));
    a[count].profile = profile;
    if (!occurrence_copy(d, occurrence, &a[count].occurrence)) {
        tracked_deallocate(&d->result->memory, a); return NULL;
    }
    tracked_deallocate(&d->result->memory, (void *)item->adjustments);
    item->adjustments = a; item->adjustment_count = count + 1;
    return &a[count];
}
static bool set_given(Driver *d, size_t row, CeCslDisambiguationProfile profile,
    const CeCslEvalOccurrence *occurrence, size_t binding, size_t person) {
    CeCslDisambiguationAdjustment *a = adjustment(d, row, profile, occurrence);
    if (!a) return false;
    for (size_t i = 0; i < a->override_count; ++i) {
        if (!work(d, 1)) return false;
        if (a->overrides[i].binding == binding && a->overrides[i].person_index == person) return true;
    }
    CeCslNameOverride *v = allocate(d, (a->override_count + 1) * sizeof(*v));
    if (!v) return false;
    if (a->override_count) memcpy(v, a->overrides, a->override_count * sizeof(*v));
    v[a->override_count] = (CeCslNameOverride){binding, person, CE_CSL_GIVEN_INITIALS};
    tracked_deallocate(&d->result->memory, (void *)a->overrides);
    a->overrides = v; ++a->override_count;
    return changed(d, row);
}
static bool set_minimum(Driver *d, size_t row, CeCslDisambiguationProfile profile,
    const CeCslEvalOccurrence *occurrence, size_t minimum) {
    CeCslDisambiguationAdjustment *a = adjustment(d, row, profile, occurrence);
    if (!a) return false;
    if (minimum <= a->minimum_names) return true;
    a->minimum_names = minimum; return changed(d, row);
}

static bool bytes_valid(Driver *d, CeCslDisambiguationBytes bytes) {
    if ((!bytes.data && bytes.size) || bytes.size > CE_CSL_DISAMBIGUATION_MAX_KEY_BYTES)
        return stop(d, CE_CSL_DISAMBIGUATION_PROVIDER_ERROR, "Invalid finalized disambiguation key");
    return charge(d, &d->emitted, bytes.size, d->options.max_emitted_bytes,
                  "Citation disambiguation cumulative output limit exceeded");
}
static bool text_valid(Driver *d, const char *text) {
    if (!text) return true;
    size_t size = 0;
    while (size <= CE_CSL_DISAMBIGUATION_MAX_KEY_BYTES && text[size]) ++size;
    if (size > CE_CSL_DISAMBIGUATION_MAX_KEY_BYTES)
        return stop(d, CE_CSL_DISAMBIGUATION_LIMIT, "Disambiguation name identity exceeds text limit");
    return charge(d, &d->emitted, size, d->options.max_emitted_bytes,
                  "Citation disambiguation cumulative identity limit exceeded");
}
static bool probe_valid(Driver *d, const CeCslDisambiguationProbe *p, size_t row,
                        CeCslDisambiguationProfile profile) {
    if (!p || (p->names_count && !p->names) || p->names_count > CE_CSL_DISAMBIGUATION_MAX_NAMES)
        return stop(d, CE_CSL_DISAMBIGUATION_PROVIDER_ERROR, "Invalid disambiguation probe");
    if (!bytes_valid(d, p->key)) return false;
    size_t primary = 0;
    for (size_t i = 0; i < p->names_count; ++i) {
        if (!work(d, 1)) return false;
        const CeCslDisambiguationNames *n = &p->names[i];
        if (!n->occurrence.source || n->occurrence.macro_depth > CE_CSL_EVALUATE_MAX_DEPTH ||
            (n->occurrence.macro_depth && !n->occurrence.macro_calls) ||
            (n->person_count && !n->people) || n->next_minimum_names > CE_CSL_NAMES_MAX_PEOPLE)
            return stop(d, CE_CSL_DISAMBIGUATION_PROVIDER_ERROR, "Invalid disambiguation names metadata");
        CeCslEvalNameAdjustment current = {0};
        (void)ce_csl_disambiguation_adjust_names(&d->result->items[row], profile, &n->occurrence, &current);
        if (n->next_minimum_names && n->next_minimum_names <= current.minimum_names)
            return stop(d, CE_CSL_DISAMBIGUATION_PROVIDER_ERROR, "Name expansion does not advance its minimum");
        if (!charge(d, &d->names, n->person_count, d->options.max_names,
                    "Citation disambiguation cumulative names limit exceeded")) return false;
        for (size_t j = 0; j < n->person_count; ++j) {
            if (!work(d, 1)) return false;
            const CeCslDisambiguationPerson *person = &n->people[j];
            const CeCslNameIdentity *identity = person->identity;
            if (!identity || !person->source_variable ||
                person->binding >= CE_CSL_NAMES_MAX_BINDINGS || person->person_index >= CE_CSL_NAMES_MAX_PEOPLE ||
                identity->kind < CE_CSL_NAME_EMPTY || identity->kind > CE_CSL_NAME_LITERAL ||
                (person->can_initials && identity->kind != CE_CSL_NAME_PERSONAL) ||
                (person->primary && (!person->rendered || ++primary > 1)))
                return stop(d, CE_CSL_DISAMBIGUATION_PROVIDER_ERROR, "Invalid typed disambiguation person");
            if (!bytes_valid(d, person->style_key) || !bytes_valid(d, person->initials_key) ||
                !text_valid(d, person->source_variable) || !text_valid(d, identity->family) ||
                !text_valid(d, identity->given) || !text_valid(d, identity->dropping_particle) ||
                !text_valid(d, identity->non_dropping_particle) || !text_valid(d, identity->suffix) ||
                !text_valid(d, identity->literal)) return false;
        }
    }
    return true;
}
static bool render_dirty(Driver *d) {
    for (size_t row = 0; row < d->result->count; ++row) {
        if (!work(d, 1)) return false;
        if (!d->dirty[row]) continue;
        for (size_t k = 0; k < d->profile_count; ++k) {
            if (!work(d, 1) || !charge(d, &d->calls, 1, d->options.max_probes,
                                      "Citation disambiguation probe limit exceeded")) return false;
            size_t slot = row * d->profile_count + k;
            CeCslDisambiguationRequest request = {d->result->items[row].item_index,
                d->profiles[k], &d->result->items[row], false};
            const CeCslInputItem *item = ce_csl_input_item(d->input, request.item_index);
            request.invisible = item->first_citation == SIZE_MAX;
            CeCslDisambiguationProbe *p = NULL;
            CeCslDisambiguationStatus status = d->provider.render(d->provider.context, &request,
                &d->memory, &p, d->error, d->capacity);
            if (status != CE_CSL_DISAMBIGUATION_OK || !p) {
                if (p) d->provider.release(d->provider.context, p);
                CeCslDisambiguationStatus failure = status <= CE_CSL_DISAMBIGUATION_OK ||
                    status > CE_CSL_DISAMBIGUATION_PROVIDER_ERROR ?
                    CE_CSL_DISAMBIGUATION_PROVIDER_ERROR :
                    d->result->memory.limit_hit ? CE_CSL_DISAMBIGUATION_LIMIT : status;
                if (d->error && d->capacity && *d->error) { d->status = failure; return false; }
                return stop(d, failure,
                    "Disambiguation provider could not produce a finalized probe");
            }
            if (!probe_valid(d, p, row, d->profiles[k])) { d->provider.release(d->provider.context, p); return false; }
            if (d->probes[slot]) d->provider.release(d->provider.context, d->probes[slot]);
            d->probes[slot] = p;
        }
        d->dirty[row] = false;
    }
    return true;
}
static int bytes_compare(Driver *d, CeCslDisambiguationBytes a, CeCslDisambiguationBytes b) {
    size_t size = a.size < b.size ? a.size : b.size;
    if (!work(d, 1) || !charge(d, &d->compared, size, d->options.max_compared_bytes,
                               "Disambiguation comparison byte limit exceeded")) return 0;
    int result = size ? memcmp(a.data, b.data, size) : 0;
    return result < 0 ? -1 : result > 0 ? 1 : a.size < b.size ? -1 : a.size > b.size ? 1 : 0;
}
typedef int (*Compare)(Driver *, size_t, size_t, void *);
static bool sort_indices(Driver *d, size_t count, Compare compare, void *context) {
    if (count > d->order_capacity) {
        size_t *order = allocate(d, count * sizeof(*order));
        if (!order) return false;
        size_t *scratch = allocate(d, count * sizeof(*scratch));
        if (!scratch) { tracked_deallocate(&d->result->memory, order); return false; }
        tracked_deallocate(&d->result->memory, d->order);
        tracked_deallocate(&d->result->memory, d->scratch);
        d->order = order; d->scratch = scratch; d->order_capacity = count;
    }
    for (size_t i = 0; i < count; ++i) d->order[i] = i;
    for (size_t width = 1; width < count; width *= 2) {
        for (size_t base = 0; base < count; base += 2 * width) {
            if (!work(d, 1)) return false;
            size_t middle = base + width < count ? base + width : count;
            size_t end = middle + width < count ? middle + width : count;
            size_t left = base, right = middle, to = base;
            while (left < middle && right < end) {
                if (compare(d, d->order[left], d->order[right], context) <= 0)
                    d->scratch[to++] = d->order[left++];
                else d->scratch[to++] = d->order[right++];
                if (d->status != CE_CSL_DISAMBIGUATION_OK) return false;
            }
            while (left < middle) d->scratch[to++] = d->order[left++];
            while (right < end) d->scratch[to++] = d->order[right++];
        }
        size_t *swap = d->order; d->order = d->scratch; d->scratch = swap;
    }
    return true;
}
static size_t group_root(Driver *d, size_t row) {
    size_t root = row;
    while (d->parents[root] != root) root = d->parents[root];
    while (row != root) { size_t next = d->parents[row]; d->parents[row] = root; row = next; }
    return root;
}
static int compare_probes(Driver *d, size_t a, size_t b, void *context) {
    (void)context;
    size_t ap = a % d->profile_count, bp = b % d->profile_count;
    if (!work(d, 1)) return 0;
    if (ap != bp) return ap < bp ? -1 : 1;
    return bytes_compare(d, d->probes[a]->key, d->probes[b]->key);
}
static bool groups(Driver *d, bool bibliography) {
    size_t count = d->result->count;
    for (size_t i = 0; i < count; ++i) { d->parents[i] = i; d->sizes[i] = 0; }
    if (!sort_indices(d, count * d->profile_count, compare_probes, NULL)) return false;
    for (size_t i = 1; i < count * d->profile_count; ++i) {
        size_t a = d->order[i-1], b = d->order[i];
        size_t profile = a % d->profile_count;
        if (!bibliography && d->profiles[profile] == CE_CSL_DISAMBIGUATION_BIBLIOGRAPHY) continue;
        if (profile == b % d->profile_count && !bytes_compare(d, d->probes[a]->key, d->probes[b]->key)) {
            size_t ar = group_root(d, a / d->profile_count), br = group_root(d, b / d->profile_count);
            if (ar < br) d->parents[br] = ar;
            else d->parents[ar] = br;
        }
        if (d->status != CE_CSL_DISAMBIGUATION_OK) return false;
    }
    for (size_t i = 0; i < count; ++i) ++d->sizes[group_root(d, i)];
    return true;
}

typedef struct NameReference {
    size_t row, profile;
    const CeCslDisambiguationNames *names;
    const CeCslDisambiguationPerson *person;
} NameReference;
static int text_compare(Driver *d, const char *a, const char *b) {
    if (!a) a = "";
    if (!b) b = "";
    while (true) {
        if (!charge(d, &d->compared, 1, d->options.max_compared_bytes,
                    "Disambiguation identity comparison limit exceeded")) return 0;
        unsigned char ac = (unsigned char)*a++, bc = (unsigned char)*b++;
        if (ac != bc) return ac < bc ? -1 : 1;
        if (!ac) return 0;
    }
}
static int identity_compare(Driver *d, const CeCslNameIdentity *a, const CeCslNameIdentity *b) {
    if (!work(d, 1)) return 0;
    if (a->kind != b->kind) return a->kind < b->kind ? -1 : 1;
    const char *av[] = {a->family,a->given,a->dropping_particle,a->non_dropping_particle,a->suffix,a->literal};
    const char *bv[] = {b->family,b->given,b->dropping_particle,b->non_dropping_particle,b->suffix,b->literal};
    for (size_t i = 0; i < 6; ++i) { int result = text_compare(d, av[i], bv[i]); if (result) return result; }
    unsigned af = (unsigned)a->comma_suffix | ((unsigned)a->static_ordering << 1) |
        ((unsigned)a->parse_names << 2) | ((unsigned)a->parse_names_present << 3);
    unsigned bf = (unsigned)b->comma_suffix | ((unsigned)b->static_ordering << 1) |
        ((unsigned)b->parse_names << 2) | ((unsigned)b->parse_names_present << 3);
    return af < bf ? -1 : af > bf ? 1 : 0;
}
static CeCslDisambiguationBytes variant(const NameReference *reference) {
    return reference->person->can_initials ? reference->person->initials_key : reference->person->style_key;
}
static int compare_primary(Driver *d, size_t a, size_t b, void *context) {
    NameReference *refs = context;
    int result = bytes_compare(d, refs[a].person->style_key, refs[b].person->style_key);
    if (!result) result = bytes_compare(d, variant(&refs[a]), variant(&refs[b]));
    if (!result) result = identity_compare(d, refs[a].person->identity, refs[b].person->identity);
    return result;
}
static bool given_names(Driver *d) {
    if (!d->options.add_given_names) return true;
    size_t capacity = d->result->count * d->profile_count, count = 0;
    NameReference *refs = zero_array(d, capacity, sizeof(*refs));
    if (capacity && !refs) return false;
    for (size_t row = 0; row < d->result->count; ++row) {
        for (size_t profile = 0; profile < d->profile_count; ++profile) {
            if (d->profiles[profile] == CE_CSL_DISAMBIGUATION_BIBLIOGRAPHY) continue;
            const CeCslDisambiguationProbe *p = d->probes[row * d->profile_count + profile];
            for (size_t i = 0; i < p->names_count; ++i) {
                for (size_t j = 0; j < p->names[i].person_count; ++j) {
                    if (!work(d, 1)) goto cleanup;
                    const CeCslDisambiguationPerson *person = &p->names[i].people[j];
                    if (person->primary && person->rendered && person->identity->kind == CE_CSL_NAME_PERSONAL)
                        refs[count++] = (NameReference){row,profile,&p->names[i],person};
                }
            }
        }
    }
    if (!sort_indices(d, count, compare_primary, refs)) goto cleanup;
    for (size_t begin = 0; begin < count;) {
        size_t end = begin + 1;
        while (end < count && !bytes_compare(d, refs[d->order[begin]].person->style_key,
                                              refs[d->order[end]].person->style_key)) ++end;
        if (d->status != CE_CSL_DISAMBIGUATION_OK) goto cleanup;
        bool ambiguous = false;
        for (size_t i = begin + 1; i < end; ++i)
            if (identity_compare(d, refs[d->order[begin]].person->identity,
                                 refs[d->order[i]].person->identity)) ambiguous = true;
        if (d->status != CE_CSL_DISAMBIGUATION_OK) goto cleanup;
        if (ambiguous) {
            for (size_t first = begin; first < end;) {
                size_t last = first + 1;
                while (last < end && !bytes_compare(d, variant(&refs[d->order[first]]),
                                                    variant(&refs[d->order[last]]))) ++last;
                bool distinguishable = true;
                for (size_t i = first + 1; i < last; ++i)
                    if (identity_compare(d, refs[d->order[first]].person->identity,
                                         refs[d->order[i]].person->identity)) distinguishable = false;
                if (d->status != CE_CSL_DISAMBIGUATION_OK) goto cleanup;
                if (distinguishable) for (size_t i = first; i < last; ++i) {
                    const NameReference *r = &refs[d->order[i]];
                    if (r->person->can_initials && bytes_compare(d, r->person->style_key, r->person->initials_key)) {
                        if (!set_given(d, r->row, d->profiles[r->profile], &r->names->occurrence,
                                       r->person->binding, r->person->person_index)) goto cleanup;
                    }
                }
                first = last;
            }
        }
        begin = end;
    }
cleanup:
    tracked_deallocate(&d->result->memory, refs);
    return d->status == CE_CSL_DISAMBIGUATION_OK && render_dirty(d);
}

static bool transaction_finish(Driver *d, bool commit) {
    d->transaction = false;
    for (size_t row = 0; row < d->result->count; ++row) {
        if (!d->saved_rows[row]) continue;
        if (commit) item_clear(&d->result->memory, &d->saved[row]);
        else {
            item_clear(&d->result->memory, &d->result->items[row]);
            d->result->items[row] = d->saved[row];
            memset(&d->saved[row], 0, sizeof(d->saved[row]));
            d->versions[row] = d->saved_versions[row]; d->dirty[row] = true;
        }
        d->saved_rows[row] = false;
    }
    return commit || render_dirty(d);
}
static bool add_names(Driver *d) {
    if (!d->options.add_names) return true;
    size_t count = d->result->count;
    while (true) {
        if (!groups(d, false)) return false;
        size_t selected = SIZE_MAX, member_count = 0;
        for (size_t row = 0; row < count; ++row) {
            if (!work(d, 1)) return false;
            size_t root = group_root(d, row);
            if (d->sizes[root] > 1 &&
                (d->failed_groups[row] != root || d->failed_versions[row] != d->versions[row])) {
                selected = root; break;
            }
        }
        if (selected == SIZE_MAX) return true;
        for (size_t row = 0; row < count; ++row) {
            if (!work(d, 1)) return false;
            if (group_root(d, row) == selected) d->members[member_count++] = row;
        }
        d->transaction = true;
        while (true) {
            size_t before = d->changes;
            for (size_t i = 0; i < member_count; ++i) {
                size_t row = d->members[i];
                for (size_t profile = 0; profile < d->profile_count; ++profile) {
                    if (d->profiles[profile] == CE_CSL_DISAMBIGUATION_BIBLIOGRAPHY) continue;
                    const CeCslDisambiguationProbe *p = d->probes[row * d->profile_count + profile];
                    for (size_t j = 0; j < p->names_count; ++j) {
                        if (!work(d, 1)) return false;
                        const CeCslDisambiguationNames *n = &p->names[j];
                        if (n->next_minimum_names &&
                            !set_minimum(d, row, d->profiles[profile], &n->occurrence, n->next_minimum_names)) return false;
                    }
                }
            }
            if (before == d->changes) {
                if (!transaction_finish(d, false) || !groups(d, false)) return false;
                for (size_t i = 0; i < member_count; ++i) {
                    size_t row = d->members[i];
                    d->failed_groups[row] = group_root(d, row); d->failed_versions[row] = d->versions[row];
                }
                break;
            }
            if (!render_dirty(d) || !given_names(d) || !groups(d, false)) return false;
            memset(d->scratch, 0, count * sizeof(*d->scratch));
            for (size_t i = 0; i < member_count; ++i) ++d->scratch[group_root(d, d->members[i])];
            bool split = false;
            size_t first = group_root(d, d->members[0]);
            for (size_t i = 0; i < member_count; ++i) {
                size_t root = group_root(d, d->members[i]);
                if (d->sizes[root] > d->scratch[root])
                    return stop(d, CE_CSL_DISAMBIGUATION_UNSUPPORTED,
                        "Added names merge previously distinct collision groups; profile is unsupported");
                if (root != first) split = true;
            }
            if (split) { if (!transaction_finish(d, true)) return false; break; }
        }
    }
}
static bool branch_and_suffix(Driver *d) {
    if (!groups(d, false)) return false;
    if (d->options.use_disambiguate_branch) {
        for (size_t row = 0; row < d->result->count; ++row) {
            if (!work(d, 1)) return false;
            if (d->sizes[group_root(d, row)] > 1) {
                d->result->items[row].disambiguate = true;
                if (!changed(d, row)) return false;
            }
        }
        if (!render_dirty(d) || !groups(d, false)) return false;
    }
    if (d->options.add_year_suffix) {
        if (d->result->count) memset(d->scratch, 0, d->result->count * sizeof(*d->scratch));
        for (size_t row = 0; row < d->result->count; ++row) {
            if (!work(d, 1)) return false;
            size_t root = group_root(d, row);
            if (d->sizes[root] < 2) continue;
            CeCslDisambiguationItem *item = &d->result->items[row];
            uint64_t number = ++d->scratch[root]; item->suffix_ordinal = number;
            char reversed[8]; size_t size = 0;
            do { --number; reversed[size++] = (char)('a' + number % 26); number /= 26; } while (number);
            for (size_t i = 0; i < size; ++i) item->year_suffix[i] = reversed[size - 1 - i];
            item->year_suffix[size] = 0;
            if (!changed(d, row)) return false;
        }
        if (!render_dirty(d)) return false;
    }
    return true;
}

static int compare_source(Driver *d, size_t a, size_t b, void *context) {
    NameReference *r = context;
    if (!work(d, 1)) return 0;
    if (r[a].row != r[b].row) return r[a].row < r[b].row ? -1 : 1;
    int result = text_compare(d, r[a].person->source_variable, r[b].person->source_variable);
    if (result) return result;
    size_t ai = r[a].person->person_index, bi = r[b].person->person_index;
    return ai < bi ? -1 : ai > bi ? 1 : 0;
}
static bool sync_bibliography(Driver *d) {
    if (!(d->options.profiles & CE_CSL_DISAMBIGUATION_BIBLIOGRAPHY)) return true;
    while (true) {
        size_t count = 0, before = d->changes;
        for (size_t slot = 0; slot < d->result->count * d->profile_count; ++slot) {
            const CeCslDisambiguationProbe *p = d->probes[slot];
            for (size_t i = 0; i < p->names_count; ++i) count += p->names[i].person_count;
        }
        NameReference *refs = zero_array(d, count, sizeof(*refs));
        if (count && !refs) return false;
        size_t at = 0;
        for (size_t slot = 0; slot < d->result->count * d->profile_count; ++slot) {
            const CeCslDisambiguationProbe *p = d->probes[slot];
            for (size_t i = 0; i < p->names_count; ++i)
                for (size_t j = 0; j < p->names[i].person_count; ++j)
                    refs[at++] = (NameReference){slot / d->profile_count,slot % d->profile_count,
                                                &p->names[i],&p->names[i].people[j]};
        }
        if (!sort_indices(d, count, compare_source, refs)) {
            tracked_deallocate(&d->result->memory, refs); return false;
        }
        for (size_t begin = 0; begin < count;) {
            size_t end = begin + 1;
            while (end < count && !compare_source(d, d->order[begin], d->order[end], refs)) ++end;
            bool visible = false, initials = false;
            const NameReference *first = &refs[d->order[begin]];
            for (size_t i = begin; i < end; ++i) {
                const NameReference *r = &refs[d->order[i]];
                if (identity_compare(d, first->person->identity, r->person->identity)) {
                    stop(d, CE_CSL_DISAMBIGUATION_PROVIDER_ERROR, "A source person changed identity across rendering profiles"); break;
                }
                if (d->profiles[r->profile] == CE_CSL_DISAMBIGUATION_BIBLIOGRAPHY) continue;
                CeCslEvalNameAdjustment adjustment = {0};
                (void)ce_csl_disambiguation_adjust_names(&d->result->items[r->row], d->profiles[r->profile],
                                                        &r->names->occurrence, &adjustment);
                if (adjustment.minimum_names && r->person->rendered) visible = true;
                for (size_t j = 0; j < adjustment.override_count; ++j)
                    if (adjustment.overrides[j].binding == r->person->binding &&
                        adjustment.overrides[j].person_index == r->person->person_index) initials = true;
            }
            for (size_t i = begin; i < end && d->status == CE_CSL_DISAMBIGUATION_OK; ++i) {
                const NameReference *r = &refs[d->order[i]];
                if (d->profiles[r->profile] != CE_CSL_DISAMBIGUATION_BIBLIOGRAPHY) continue;
                if (initials && r->person->can_initials && !r->person->given_visible &&
                    !set_given(d, r->row, d->profiles[r->profile], &r->names->occurrence,
                               r->person->binding, r->person->person_index)) break;
                if (visible && !r->person->rendered && r->names->next_minimum_names &&
                    !set_minimum(d, r->row, d->profiles[r->profile], &r->names->occurrence,
                                 r->names->next_minimum_names)) break;
            }
            if (d->status != CE_CSL_DISAMBIGUATION_OK) break;
            begin = end;
        }
        tracked_deallocate(&d->result->memory, refs);
        if (d->status != CE_CSL_DISAMBIGUATION_OK) return false;
        if (before == d->changes) return true;
        if (!render_dirty(d)) return false;
    }
}

static void driver_clear(Driver *d) {
    if (!d->result) return;
    Memory *m = &d->result->memory;
    if (d->probes)
        for (size_t i = 0; i < d->result->count * d->profile_count; ++i)
            if (d->probes[i]) d->provider.release(d->provider.context, d->probes[i]);
    if (d->saved)
        for (size_t i = 0; i < d->result->count; ++i) item_clear(m, &d->saved[i]);
    tracked_deallocate(m, d->probes);
    tracked_deallocate(m, d->parents); tracked_deallocate(m, d->sizes);
    tracked_deallocate(m, d->order); tracked_deallocate(m, d->scratch);
    tracked_deallocate(m, d->members); tracked_deallocate(m, d->versions);
    tracked_deallocate(m, d->failed_versions); tracked_deallocate(m, d->failed_groups);
    tracked_deallocate(m, d->dirty); tracked_deallocate(m, d->saved);
    tracked_deallocate(m, d->saved_rows); tracked_deallocate(m, d->saved_versions);
}

CeCslDisambiguationStatus ce_csl_disambiguation_build(const CeCslInput *input,
    const CeCslRegistry *registry, const CeCslDisambiguationOptions *options,
    const CeCslDisambiguationProvider *provider, const CeCslOutputAllocator *allocator,
    CeCslDisambiguation **out, char *error, size_t capacity) {
    if (error && capacity) *error = 0;
    Driver d = {0}; d.input = input; d.error = error; d.capacity = capacity;
    if (options) d.options = *options;
    if (!input || !registry || !provider || !provider->render || !provider->release || !out ||
        (allocator && (!allocator->allocate || !allocator->deallocate))) {
        stop(&d, CE_CSL_DISAMBIGUATION_INVALID, "Invalid citation disambiguation argument"); return d.status;
    }
    if (d.options.given_rule < CE_CSL_DISAMBIGUATE_BY_CITE ||
        d.options.given_rule > CE_CSL_DISAMBIGUATE_PRIMARY_NAME_INITIALS) {
        stop(&d, CE_CSL_DISAMBIGUATION_INVALID, "Invalid given-name disambiguation rule"); return d.status;
    }
    if (d.options.add_given_names && d.options.given_rule != CE_CSL_DISAMBIGUATE_PRIMARY_NAME_INITIALS) {
        stop(&d, CE_CSL_DISAMBIGUATION_UNSUPPORTED, "Only primary-name-with-initials expansion is implemented"); return d.status;
    }
    if (!d.options.profiles) d.options.profiles = CE_CSL_DISAMBIGUATION_FIRST;
    if (!(d.options.profiles & CE_CSL_DISAMBIGUATION_FIRST) || (d.options.profiles & ~7u)) {
        stop(&d, CE_CSL_DISAMBIGUATION_INVALID, "Invalid canonical disambiguation profiles"); return d.status;
    }
#define LIMIT(field, maximum) do { \
    if (!d.options.field) d.options.field = (maximum); \
    if (d.options.field > (maximum)) { \
        stop(&d, CE_CSL_DISAMBIGUATION_INVALID, "Disambiguation budget exceeds hard limit"); return d.status; \
    } \
} while (0)
    LIMIT(max_probes, CE_CSL_DISAMBIGUATION_MAX_PROBES);
    LIMIT(max_names, CE_CSL_DISAMBIGUATION_MAX_NAMES);
    LIMIT(max_changes, CE_CSL_DISAMBIGUATION_MAX_CHANGES);
    LIMIT(max_emitted_bytes, CE_CSL_DISAMBIGUATION_MAX_BYTES);
    LIMIT(max_compared_bytes, CE_CSL_DISAMBIGUATION_MAX_BYTES);
    LIMIT(max_workspace_bytes, CE_CSL_DISAMBIGUATION_MAX_WORKSPACE);
    LIMIT(max_work_units, CE_CSL_DISAMBIGUATION_MAX_WORK);
#undef LIMIT
    size_t count = ce_csl_registry_count(registry), items = ce_csl_input_item_count(input);
    if (items > CE_CSL_INPUT_MAX_ITEMS || count > items) {
        stop(&d, CE_CSL_DISAMBIGUATION_INVALID, "Invalid disambiguation registry size"); return d.status;
    }
    d.provider = *provider;
    for (unsigned profile = 1; profile <= 4; profile *= 2)
        if (d.options.profiles & profile) d.profiles[d.profile_count++] = (CeCslDisambiguationProfile)profile;
    CeCslOutputAllocator base = {NULL, default_allocate, default_deallocate};
    if (allocator) base = *allocator;
    if (!work(&d, 1)) return d.status;
    if (sizeof(*d.result) > d.options.max_workspace_bytes) {
        stop(&d, CE_CSL_DISAMBIGUATION_LIMIT, "Disambiguation workspace is too small"); return d.status;
    }
    d.result = base.allocate(base.context, sizeof(*d.result));
    if (!d.result) {
        stop(&d, CE_CSL_DISAMBIGUATION_OOM, "Cannot allocate citation disambiguation result"); return d.status;
    }
    memset(d.result, 0, sizeof(*d.result));
    d.result->memory = (Memory){base,sizeof(*d.result),d.options.max_workspace_bytes,false};
    d.memory = (CeCslOutputAllocator){&d.result->memory,tracked_allocate,tracked_deallocate};
    d.result->count = count; d.result->item_count = items;
    d.result->items = zero_array(&d, count, sizeof(*d.result->items));
    if (count && !d.result->items) goto cleanup;
    d.result->item_rows = zero_array(&d, items, sizeof(*d.result->item_rows));
    if (items && !d.result->item_rows) goto cleanup;
    for (size_t i = 0; i < items; ++i) d.result->item_rows[i] = SIZE_MAX;
    for (size_t row = 0; row < count; ++row) {
        if (!work(&d, 1)) goto cleanup;
        const CeCslRegistryRow *entry = ce_csl_registry_row(registry, row);
        if (!entry || entry->item_index >= items || d.result->item_rows[entry->item_index] != SIZE_MAX) {
            stop(&d, CE_CSL_DISAMBIGUATION_INVALID, "Registry does not match immutable disambiguation input"); goto cleanup;
        }
        d.result->items[row].item_index = entry->item_index;
        d.result->items[row].unresolved_group = SIZE_MAX;
        d.result->item_rows[entry->item_index] = row;
    }
    for (size_t i = 0; i < items; ++i) {
        if (!work(&d, 1)) goto cleanup;
        if (ce_csl_input_item(input, i)->first_citation != SIZE_MAX && d.result->item_rows[i] == SIZE_MAX) {
            stop(&d, CE_CSL_DISAMBIGUATION_INVALID, "Registry omits a cited input item"); goto cleanup;
        }
    }
#define ARRAY(field, elements) do { \
    d.field = zero_array(&d, (elements), sizeof(*d.field)); \
    if ((elements) != 0 && !d.field) goto cleanup; \
} while (0)
    ARRAY(probes, count * d.profile_count);
    ARRAY(parents, count); ARRAY(sizes, count); ARRAY(members, count);
    ARRAY(versions, count); ARRAY(failed_versions, count); ARRAY(failed_groups, count);
    ARRAY(dirty, count); ARRAY(saved, count); ARRAY(saved_rows, count); ARRAY(saved_versions, count);
#undef ARRAY
    for (size_t i = 0; i < count; ++i) { d.dirty[i] = true; d.failed_groups[i] = SIZE_MAX; }
    if (!render_dirty(&d) || !given_names(&d) || !add_names(&d) ||
        !branch_and_suffix(&d) || !sync_bibliography(&d) || !groups(&d, true)) goto cleanup;
    for (size_t row = 0; row < count; ++row) {
        if (!work(&d, 1)) goto cleanup;
        if (group_root(&d, row) == row && d.sizes[row] > 1) d.scratch[row] = d.result->unresolved++;
    }
    for (size_t row = 0; row < count; ++row) {
        size_t root = group_root(&d, row);
        if (d.sizes[root] > 1) d.result->items[row].unresolved_group = d.scratch[root];
    }
cleanup:
    driver_clear(&d);
    if (d.status != CE_CSL_DISAMBIGUATION_OK) ce_csl_disambiguation_free(d.result);
    else { ce_csl_disambiguation_free(*out); *out = d.result; }
    return d.status;
}
