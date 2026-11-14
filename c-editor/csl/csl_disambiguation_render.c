#include "csl_disambiguation_render.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef union Block {
    unsigned char padding[32];
    long double alignment;
    void *pointer_alignment;
    size_t size;
} Block;
typedef struct Capability {
    struct Capability *next;
    CeCslEvalOccurrence occurrence;
    size_t minimum;
    bool initials;
} Capability;
typedef struct KeyBlock {
    struct KeyBlock *next;
    unsigned char bytes[];
} KeyBlock;
typedef struct Probe {
    CeCslDisambiguationProbe public;
    CeCslOutputAllocator outer, allocator;
    CeCslEvaluation *evaluation;
    CeCslFinalized *finalized;
    CeCslDisambiguationNames *names;
    KeyBlock *keys;
    Capability *capabilities;
    size_t live, work, nodes, variants, people, maximum_work;
    CeCslDisambiguationStatus status;
    /* Construction-only environment. Cleared before returning the probe. */
    const CeCslDisambiguationRenderConfig *config;
    char *error;
    size_t capacity;
} Probe;
typedef struct Run {
    Probe *probe;
    const CeCslDisambiguationRequest *request;
    const CeCslEvalOccurrence *target;
    CeCslNameOverride candidate;
    size_t minimum;
    CeCslNameOverride *scratch;
} Run;
static void *default_allocate(void *context, size_t size) {
    (void)context;
    return malloc(size);
}
static void default_free(void *context, void *memory) {
    (void)context;
    free(memory);
}
static bool stop(Probe *p, CeCslDisambiguationStatus status, const char *message) {
    if (p->status == CE_CSL_DISAMBIGUATION_OK) {
        p->status = status;
        if (p->error && p->capacity)
            snprintf(p->error, p->capacity, "%s", message);
    }
    return false;
}
static size_t limit(size_t value, size_t maximum) { return value ? value : maximum; }
static bool work(Probe *p, size_t count) {
    if (p->status != CE_CSL_DISAMBIGUATION_OK)
        return false;
    if (p->config && p->config->cancelled && p->config->cancelled(p->config->cancel_context))
        return stop(p, CE_CSL_DISAMBIGUATION_CANCELLED, "Citation rendering cancelled");
    size_t maximum = p->maximum_work;
    if (count > maximum - p->work)
        return stop(p, CE_CSL_DISAMBIGUATION_LIMIT, "Citation rendering work limit exceeded");
    p->work += count;
    return true;
}
static void *allocate(void *context, size_t size) {
    Probe *p = context;
    if (!work(p, 1))
        return NULL;
    if (size > CE_CSL_DISAMBIGUATION_MAX_WORKSPACE - sizeof(Block) ||
        size + sizeof(Block) > CE_CSL_DISAMBIGUATION_MAX_WORKSPACE - p->live) {
        stop(p, CE_CSL_DISAMBIGUATION_LIMIT, "Citation rendering workspace limit exceeded");
        return NULL;
    }
    Block *b = p->outer.allocate(p->outer.context, sizeof(*b) + size);
    if (!b) {
        stop(p, CE_CSL_DISAMBIGUATION_OOM, "Out of memory rendering citation");
        return NULL;
    }
    b->size = sizeof(*b) + size;
    p->live += b->size;
    return b + 1;
}
static void release(void *context, void *memory) {
    if (!memory)
        return;
    Probe *p = context;
    Block *b = (Block *)memory - 1;
    p->live -= b->size;
    p->outer.deallocate(p->outer.context, b);
}
static bool same(Probe *p, const CeCslEvalOccurrence *a, const CeCslEvalOccurrence *b) {
    if (!work(p, 1))
        return false;
    if (a->source != b->source || a->macro_depth != b->macro_depth)
        return false;
    for (size_t i = 0; i < a->macro_depth; ++i) {
        if (!work(p, 1) || a->macro_calls[i] != b->macro_calls[i])
            return false;
    }
    return true;
}
static bool adjusted(Run *r, const CeCslEvalOccurrence *occurrence, CeCslEvalNameAdjustment *out) {
    Probe *p = r->probe;
    const CeCslDisambiguationItem *state = r->request->state;
    /* Bound the public helper's scan as well as our own comparisons. */
    if (!work(p, state->adjustment_count))
        return false;
    for (size_t i = 0; i < state->adjustment_count; ++i)
        if (!work(p, state->adjustments[i].occurrence.macro_depth))
            return false;
    return ce_csl_disambiguation_adjust_names(state, r->request->profile, occurrence, out);
}
static bool adjust(void *context, const CeCslEvalOccurrence *occurrence, const CeCslNamesBinding *bindings,
                   size_t count, const CeCslNameOptions *options, CeCslEvalNameAdjustment *out, char *error,
                   size_t capacity) {
    (void)bindings;
    (void)count;
    (void)error;
    (void)capacity;
    Run *r = context;
    Probe *p = r->probe;
    release(p, r->scratch);
    r->scratch = NULL;
    if (!adjusted(r, occurrence, out))
        return false;
    if (!r->target) {
        size_t path_size = occurrence->macro_depth * sizeof(*occurrence->macro_calls);
        Capability *c = allocate(p, sizeof(*c) + path_size);
        if (!c)
            return false;
        *c = (Capability){.next = p->capabilities,
                          .occurrence = *occurrence,
                          .minimum = out->minimum_names,
                          .initials = options->initialize && options->initialize_with != NULL};
        if (path_size) {
            c->occurrence.macro_calls = (const CeCslXmlNode **)(c + 1);
            memcpy((void *)c->occurrence.macro_calls, occurrence->macro_calls, path_size);
        }
        p->capabilities = c;
    } else if (same(p, r->target, occurrence)) {
        if (out->override_count >= CE_CSL_NAMES_MAX_PEOPLE)
            return stop(p, CE_CSL_DISAMBIGUATION_LIMIT, "Too many name overrides for candidate rendering");
        size_t count_overrides = out->override_count;
        r->scratch = allocate(p, (count_overrides + 1) * sizeof(*r->scratch));
        if (!r->scratch)
            return false;
        if (count_overrides)
            memcpy(r->scratch, out->overrides, count_overrides * sizeof(*r->scratch));
        size_t index = count_overrides;
        for (size_t i = 0; i < count_overrides; ++i) {
            if (!work(p, 1))
                return false;
            if (r->scratch[i].binding == r->candidate.binding &&
                r->scratch[i].person_index == r->candidate.person_index) {
                index = i;
                break;
            }
        }
        r->scratch[index] = r->candidate;
        out->overrides = r->scratch;
        out->override_count = count_overrides + (index == count_overrides);
        if (out->minimum_names < r->minimum)
            out->minimum_names = r->minimum;
    }
    return p->status == CE_CSL_DISAMBIGUATION_OK;
}
static bool child_failure(Probe *p) {
    if (p->status != CE_CSL_DISAMBIGUATION_OK)
        return false;
    const char *message = p->error && p->capacity ? p->error : "";
    /* Child modules predate typed statuses. Preserve their detailed message. */
    if (strstr(message, "unsupported") || strstr(message, "not implemented") ||
        strstr(message, "not supported") || strstr(message, "not yet supported") ||
        strstr(message, "unavailable"))
        p->status = CE_CSL_DISAMBIGUATION_UNSUPPORTED;
    else if (strstr(message, "limit"))
        p->status = CE_CSL_DISAMBIGUATION_LIMIT;
    else
        p->status = CE_CSL_DISAMBIGUATION_PROVIDER_ERROR;
    return false;
}
static bool evaluate(Run *run, CeCslEvaluation **evaluation, CeCslFinalized **finalized) {
    Probe *p = run->probe;
    if (!work(p, 1))
        return false;
    const CeCslDisambiguationRequest *r = run->request;
    const CeCslDisambiguationRenderConfig *c = p->config;
    CeCslGeneratedVariables generated = {
        .citation_number = {true, true, ce_csl_registry_citation_number(c->registry, r->item_index)},
        .first_reference_note_number = {true, false, 0},
        .year_suffix = {true, *r->state->year_suffix ? r->state->year_suffix : "",
                        strlen(r->state->year_suffix)}};
    cJSON empty_cite = {.type = cJSON_Object};
    CeCslInputCite cite = {.item_index = r->item_index, .data = &empty_cite};
    CeCslEvalRequest request = {
        .style = c->style,
        .locale = c->locale,
        .section = r->profile == CE_CSL_DISAMBIGUATION_BIBLIOGRAPHY ? ce_csl_style_bibliography(c->style)
                                                                    : ce_csl_style_citation(c->style),
        .position = r->profile == CE_CSL_DISAMBIGUATION_SUBSEQUENT ? CE_CSL_POSITION_SUBSEQUENT
                                                                   : CE_CSL_POSITION_FIRST,
        .disambiguate = r->state->disambiguate,
        .variables = {.item = ce_csl_input_item(c->input, r->item_index),
                      .cite = r->profile == CE_CSL_DISAMBIGUATION_BIBLIOGRAPHY ? NULL : &cite,
                      .generated = &generated,
                      .context = c->short_context,
                      .short_value = c->short_value},
        .name_context = run,
        .adjust_names = adjust};
    bool ok = ce_csl_evaluate(&request, &p->allocator, evaluation, p->error, p->capacity);
    release(p, run->scratch);
    run->scratch = NULL;
    if (!ok)
        return child_failure(p);
    size_t nodes = ce_csl_evaluation_summary(*evaluation)->node_count;
    if (nodes > limit(c->max_nodes, CE_CSL_DISAMBIGUATION_RENDER_MAX_NODES) - p->nodes)
        return stop(p, CE_CSL_DISAMBIGUATION_LIMIT, "Citation rendering cumulative node limit exceeded");
    p->nodes += nodes;
    if (!work(p, nodes))
        return false;
    if (!ce_csl_finalize(*evaluation, &p->allocator, finalized, p->error, p->capacity))
        return child_failure(p);
    return true;
}
static bool suffix_reachable(Probe *p, const CeCslXmlNode *node, size_t depth, bool *found) {
    if (!node || *found)
        return true;
    if (depth > CE_CSL_EVALUATE_MAX_DEPTH)
        return stop(p, CE_CSL_DISAMBIGUATION_LIMIT, "Year suffix macro traversal depth limit exceeded");
    for (; node; node = node->next) {
        if (!work(p, 1))
            return false;
        if (ce_csl_xml_is(node, "sort"))
            continue;
        const char *variable = ce_csl_xml_attribute(node, "variable");
        if (ce_csl_xml_is(node, "text") && variable && !strcmp(variable, "year-suffix")) {
            *found = true;
            return true;
        }
        const char *macro = ce_csl_xml_attribute(node, "macro");
        if (macro &&
            !suffix_reachable(p, ce_csl_style_macro(p->config->style, macro)->children, depth + 1, found))
            return false;
        if (!suffix_reachable(p, node->children, depth + 1, found))
            return false;
        if (*found)
            return true;
    }
    return true;
}
static const Capability *capability(Probe *p, const CeCslEvalOccurrence *occurrence) {
    for (Capability *c = p->capabilities; c; c = c->next)
        if (same(p, &c->occurrence, occurrence))
            return c;
    return NULL;
}
static bool key_span(Probe *p, const CeCslFinalized *finalized, const CeCslFinalSpan *span,
                     CeCslDisambiguationBytes *out) {
    size_t size;
    const char *text = ce_csl_inline_text(ce_csl_finalized_inline(finalized), &size);
    if (span->offset > size || span->length > size - span->offset)
        return stop(p, CE_CSL_DISAMBIGUATION_PROVIDER_ERROR, "Invalid finalized person span");
    if (span->length > CE_CSL_DISAMBIGUATION_MAX_KEY_BYTES)
        return stop(p, CE_CSL_DISAMBIGUATION_LIMIT, "Finalized name key exceeds 64 KiB");
    *out = (CeCslDisambiguationBytes){(const unsigned char *)text + span->offset, span->length};
    return true;
}
static bool initials(Probe *p, Run *run, CeCslDisambiguationNames *names, CeCslDisambiguationPerson *person,
                     size_t minimum) {
    if (p->variants >= limit(p->config->max_variants, CE_CSL_DISAMBIGUATION_RENDER_MAX_VARIANTS))
        return stop(p, CE_CSL_DISAMBIGUATION_LIMIT, "Citation rendering variant limit exceeded");
    ++p->variants;
    CeCslEvaluation *evaluation = NULL;
    CeCslFinalized *finalized = NULL;
    Run variant = {.probe = p,
                   .request = run->request,
                   .target = &names->occurrence,
                   .candidate = {person->binding, person->person_index, CE_CSL_GIVEN_INITIALS},
                   .minimum = minimum};
    bool ok = evaluate(&variant, &evaluation, &finalized);
    if (!ok)
        goto done;
    for (size_t i = 0; i < ce_csl_finalized_span_count(finalized); ++i) {
        if (!work(p, 1)) {
            ok = false;
            goto done;
        }
        const CeCslFinalSpan *s = ce_csl_finalized_span(finalized, i);
        if (s->kind != CE_CSL_FINAL_PERSON)
            continue;
        const CeCslEvalNodeInfo *info = ce_csl_eval_node_info(s->node);
        const CeCslNameRendered *rendered =
            ce_csl_names_result_person(ce_csl_eval_node_names(s->node), s->person_index);
        if (!rendered || rendered->binding != person->binding ||
            rendered->person_index != person->person_index || !same(p, &info->occurrence, &names->occurrence))
            continue;
        person->can_initials = rendered->given_initialized;
        if (person->rendered && person->can_initials) {
            CeCslDisambiguationBytes key;
            if (!key_span(p, finalized, s, &key)) {
                ok = false;
                goto done;
            }
            KeyBlock *block = allocate(p, sizeof(*block) + key.size);
            if (!block) {
                ok = false;
                goto done;
            }
            block->next = p->keys;
            p->keys = block;
            if (key.size)
                memcpy(block->bytes, key.data, key.size);
            person->initials_key = (CeCslDisambiguationBytes){block->bytes, key.size};
        }
        break;
    }
    if (p->status != CE_CSL_DISAMBIGUATION_OK)
        ok = false;
done:
    ce_csl_finalized_free(finalized);
    ce_csl_evaluation_free(evaluation);
    return ok;
}
static bool collect(Probe *p, Run *run) {
    size_t count = 0;
    for (size_t i = 0; i < ce_csl_finalized_span_count(p->finalized); ++i) {
        if (!work(p, 1))
            return false;
        const CeCslFinalSpan *s = ce_csl_finalized_span(p->finalized, i);
        const CeCslEvalNodeInfo *info = ce_csl_eval_node_info(s->node);
        if (s->kind == CE_CSL_FINAL_NODE && info->kind == CE_CSL_EVAL_NAMES && !info->used_substitute &&
            ce_csl_eval_node_names(s->node))
            ++count;
    }
    if (count) {
        p->names = allocate(p, count * sizeof(*p->names));
        if (!p->names)
            return false;
        memset(p->names, 0, count * sizeof(*p->names));
    }
    p->public.names = p->names;
    p->public.names_count = count;
    size_t which = 0;
    bool have_primary = false;
    for (size_t i = 0; i < ce_csl_finalized_span_count(p->finalized); ++i) {
        if (!work(p, 1))
            return false;
        const CeCslFinalSpan *s = ce_csl_finalized_span(p->finalized, i);
        const CeCslEvalNodeInfo *info = ce_csl_eval_node_info(s->node);
        const CeCslNamesResult *result = ce_csl_eval_node_names(s->node);
        if (s->kind != CE_CSL_FINAL_NODE || info->kind != CE_CSL_EVAL_NAMES || info->used_substitute ||
            !result)
            continue;
        CeCslDisambiguationNames *n = &p->names[which++];
        n->occurrence = info->occurrence;
        const Capability *cap = capability(p, &info->occurrence);
        if (!cap)
            return stop(p, CE_CSL_DISAMBIGUATION_PROVIDER_ERROR, "Missing resolved name capability");
        n->next_minimum_names = ce_csl_names_result_next_minimum(result, cap->minimum);
        for (size_t b = 0; b < ce_csl_names_result_binding_count(result); ++b) {
            size_t total = ce_csl_names_data_count(ce_csl_names_result_data(result, b));
            if (total > limit(p->config->max_people, CE_CSL_DISAMBIGUATION_RENDER_MAX_PEOPLE) - p->people)
                return stop(p, CE_CSL_DISAMBIGUATION_LIMIT, "Citation rendering person limit exceeded");
            p->people += total;
            n->person_count += total;
        }
        CeCslDisambiguationPerson *people = NULL;
        if (n->person_count) {
            people = allocate(p, n->person_count * sizeof(*people));
            if (!people)
                return false;
            memset(people, 0, n->person_count * sizeof(*people));
        }
        n->people = people;
        size_t index = 0;
        for (size_t b = 0; b < ce_csl_names_result_binding_count(result); ++b) {
            const CeCslNamesData *data = ce_csl_names_result_data(result, b);
            const CeCslNamesSelection *selection = ce_csl_names_result_selection(result, b);
            size_t prefix = 0;
            for (size_t j = 0; j < ce_csl_names_data_count(data); ++j) {
                if (!work(p, 1))
                    return false;
                CeCslDisambiguationPerson *person = &people[index++];
                person->binding = b;
                person->person_index = j;
                person->source_variable = ce_csl_names_data_origin(data)->variable;
                person->identity = ce_csl_names_data_person(data, j);
                if (person->identity->kind != CE_CSL_NAME_EMPTY)
                    ++prefix;
                for (size_t k = 0; k < ce_csl_finalized_span_count(p->finalized); ++k) {
                    if (!work(p, 1))
                        return false;
                    const CeCslFinalSpan *ps = ce_csl_finalized_span(p->finalized, k);
                    if (ps->kind != CE_CSL_FINAL_PERSON || ps->node != s->node)
                        continue;
                    const CeCslNameRendered *pr = ce_csl_names_result_person(result, ps->person_index);
                    if (pr->binding != b || pr->person_index != j)
                        continue;
                    if (!key_span(p, p->finalized, ps, &person->style_key))
                        return false;
                    person->rendered = ps->length != 0;
                    person->given_visible = pr->given_visible;
                    person->given_initialized = pr->given_initialized;
                    person->primary = person->rendered && !have_primary;
                    have_primary = have_primary || person->primary;
                    break;
                }
                if (selection && selection->collapsed)
                    continue;
                if (!cap->initials || person->identity->kind != CE_CSL_NAME_PERSONAL ||
                    !*person->identity->given)
                    continue;
                if (person->rendered && person->given_initialized) {
                    person->can_initials = true;
                    person->initials_key = person->style_key;
                } else if (!initials(p, run, n, person, person->rendered ? 0 : prefix))
                    return false;
            }
        }
    }
    return true;
}
static void free_capabilities(Probe *p) {
    while (p->capabilities) {
        Capability *next = p->capabilities->next;
        release(p, p->capabilities);
        p->capabilities = next;
    }
}
void ce_csl_disambiguation_render_release(CeCslDisambiguationProbe *probe) {
    if (!probe)
        return;
    Probe *p = (Probe *)probe;
    for (size_t i = 0; i < p->public.names_count; ++i)
        release(p, (void *)p->names[i].people);
    release(p, p->names);
    free_capabilities(p);
    while (p->keys) {
        KeyBlock *next = p->keys->next;
        release(p, p->keys);
        p->keys = next;
    }
    ce_csl_finalized_free(p->finalized);
    ce_csl_evaluation_free(p->evaluation);
    CeCslOutputAllocator outer = p->outer;
    outer.deallocate(outer.context, p);
}
const CeCslEvaluation *ce_csl_disambiguation_render_evaluation(const CeCslDisambiguationProbe *probe) {
    return probe ? ((const Probe *)probe)->evaluation : NULL;
}
const CeCslFinalized *ce_csl_disambiguation_render_finalized(const CeCslDisambiguationProbe *probe) {
    return probe ? ((const Probe *)probe)->finalized : NULL;
}
CeCslDisambiguationStatus ce_csl_disambiguation_render(const CeCslDisambiguationRenderConfig *config,
                                                       const CeCslDisambiguationRequest *request,
                                                       const CeCslOutputAllocator *allocator,
                                                       CeCslDisambiguationProbe **out, char *error,
                                                       size_t capacity) {
    if (error && capacity)
        *error = 0;
    if (!config || !config->style || !config->locale || !config->input || !config->registry || !request ||
        !request->state || request->item_index != request->state->item_index ||
        !ce_csl_input_item(config->input, request->item_index) ||
        !ce_csl_registry_citation_number(config->registry, request->item_index) ||
        (request->profile != CE_CSL_DISAMBIGUATION_FIRST &&
         request->profile != CE_CSL_DISAMBIGUATION_SUBSEQUENT &&
         request->profile != CE_CSL_DISAMBIGUATION_BIBLIOGRAPHY) ||
        !memchr(request->state->year_suffix, 0, sizeof(request->state->year_suffix)) ||
        (request->state->adjustment_count && !request->state->adjustments) ||
        request->state->adjustment_count > CE_CSL_DISAMBIGUATION_RENDER_MAX_NODES ||
        config->max_variants > CE_CSL_DISAMBIGUATION_RENDER_MAX_VARIANTS ||
        config->max_nodes > CE_CSL_DISAMBIGUATION_RENDER_MAX_NODES ||
        config->max_people > CE_CSL_DISAMBIGUATION_RENDER_MAX_PEOPLE ||
        config->max_work > CE_CSL_DISAMBIGUATION_RENDER_MAX_WORK || !out || *out ||
        (allocator && (!allocator->allocate || !allocator->deallocate))) {
        if (error && capacity)
            snprintf(error, capacity, "Invalid citation rendering request");
        return CE_CSL_DISAMBIGUATION_INVALID;
    }
    for (size_t i = 0; i < request->state->adjustment_count; ++i) {
        const CeCslDisambiguationAdjustment *a = &request->state->adjustments[i];
        if (!a->occurrence.source || a->occurrence.macro_depth > CE_CSL_EVALUATE_MAX_DEPTH ||
            (a->occurrence.macro_depth && !a->occurrence.macro_calls) ||
            a->override_count > CE_CSL_NAMES_MAX_PEOPLE || (a->override_count && !a->overrides) ||
            a->minimum_names > CE_CSL_NAMES_MAX_PEOPLE) {
            if (error && capacity)
                snprintf(error, capacity, "Invalid name adjustment");
            return CE_CSL_DISAMBIGUATION_INVALID;
        }
    }
    CeCslOutputAllocator outer =
        allocator ? *allocator : (CeCslOutputAllocator){NULL, default_allocate, default_free};
    Probe *p = outer.allocate(outer.context, sizeof(*p));
    if (!p) {
        if (error && capacity)
            snprintf(error, capacity, "Out of memory rendering citation");
        return CE_CSL_DISAMBIGUATION_OOM;
    }
    memset(p, 0, sizeof(*p));
    p->outer = outer;
    p->allocator = (CeCslOutputAllocator){p, allocate, release};
    p->live = sizeof(*p);
    p->maximum_work = limit(config->max_work, CE_CSL_DISAMBIGUATION_RENDER_MAX_WORK);
    p->config = config;
    p->error = error;
    p->capacity = capacity;
    const CeCslXmlNode *section = request->profile == CE_CSL_DISAMBIGUATION_BIBLIOGRAPHY
                                      ? ce_csl_style_bibliography(config->style)
                                      : ce_csl_style_citation(config->style);
    bool ok = section != NULL;
    if (!ok)
        stop(p, CE_CSL_DISAMBIGUATION_UNSUPPORTED, "Requested style has no bibliography");
    if (ok && *request->state->year_suffix) {
        bool explicit_suffix = false;
        ok = suffix_reachable(p, section->children, 0, &explicit_suffix);
        if (ok && !explicit_suffix)
            ok = stop(p, CE_CSL_DISAMBIGUATION_UNSUPPORTED, "Implicit year suffix insertion is unsupported");
    }
    Run run = {.probe = p, .request = request};
    if (ok)
        ok = evaluate(&run, &p->evaluation, &p->finalized);
    if (ok) {
        size_t size;
        const char *text = ce_csl_inline_text(ce_csl_finalized_inline(p->finalized), &size);
        if (size > CE_CSL_DISAMBIGUATION_MAX_KEY_BYTES)
            ok = stop(p, CE_CSL_DISAMBIGUATION_LIMIT, "Finalized citation key exceeds 64 KiB");
        else
            p->public.key = (CeCslDisambiguationBytes){(const unsigned char *)text, size};
    }
    if (ok)
        ok = collect(p, &run);
    if (!ok) {
        CeCslDisambiguationStatus status = p->status;
        if (status == CE_CSL_DISAMBIGUATION_OK)
            status = CE_CSL_DISAMBIGUATION_PROVIDER_ERROR;
        ce_csl_disambiguation_render_release(&p->public);
        return status;
    }
    free_capabilities(p);
    p->config = NULL;
    p->error = NULL;
    p->capacity = 0;
    *out = &p->public;
    return CE_CSL_DISAMBIGUATION_OK;
}
static CeCslDisambiguationStatus provider_render(void *context, const CeCslDisambiguationRequest *request,
                                                 const CeCslOutputAllocator *allocator,
                                                 CeCslDisambiguationProbe **out, char *error,
                                                 size_t capacity) {
    return ce_csl_disambiguation_render(context, request, allocator, out, error, capacity);
}
static void provider_release(void *context, CeCslDisambiguationProbe *probe) {
    (void)context;
    ce_csl_disambiguation_render_release(probe);
}
CeCslDisambiguationProvider
ce_csl_disambiguation_render_provider(const CeCslDisambiguationRenderConfig *config) {
    return (CeCslDisambiguationProvider){(void *)config, provider_render, provider_release};
}
