#ifndef CE_CSL_DISAMBIGUATION_H
#define CE_CSL_DISAMBIGUATION_H

#include "csl_evaluate.h"
#include "csl_registry.h"

#define CE_CSL_DISAMBIGUATION_MAX_KEY_BYTES ((size_t)64 * 1024)
#define CE_CSL_DISAMBIGUATION_MAX_PROBES ((size_t)200000)
#define CE_CSL_DISAMBIGUATION_MAX_NAMES ((size_t)500000)
#define CE_CSL_DISAMBIGUATION_MAX_CHANGES ((size_t)1000000)
#define CE_CSL_DISAMBIGUATION_MAX_WORK ((size_t)16 * 1024 * 1024)
#define CE_CSL_DISAMBIGUATION_MAX_BYTES ((size_t)128 * 1024 * 1024)
#define CE_CSL_DISAMBIGUATION_MAX_WORKSPACE ((size_t)64 * 1024 * 1024)

typedef enum CeCslDisambiguationStatus {
    CE_CSL_DISAMBIGUATION_OK, CE_CSL_DISAMBIGUATION_INVALID,
    CE_CSL_DISAMBIGUATION_UNSUPPORTED, CE_CSL_DISAMBIGUATION_LIMIT,
    CE_CSL_DISAMBIGUATION_OOM, CE_CSL_DISAMBIGUATION_CANCELLED,
    CE_CSL_DISAMBIGUATION_PROVIDER_ERROR
} CeCslDisambiguationStatus;
typedef enum CeCslGivenNameRule {
    CE_CSL_DISAMBIGUATE_BY_CITE,
    CE_CSL_DISAMBIGUATE_ALL_NAMES,
    CE_CSL_DISAMBIGUATE_ALL_NAMES_INITIALS,
    CE_CSL_DISAMBIGUATE_PRIMARY_NAME,
    CE_CSL_DISAMBIGUATE_PRIMARY_NAME_INITIALS
} CeCslGivenNameRule;
typedef enum CeCslDisambiguationProfile {
    CE_CSL_DISAMBIGUATION_FIRST = 1u,
    CE_CSL_DISAMBIGUATION_SUBSEQUENT = 2u,
    CE_CSL_DISAMBIGUATION_BIBLIOGRAPHY = 4u
} CeCslDisambiguationProfile;
typedef struct CeCslDisambiguationBytes {
    const unsigned char *data;
    size_t size;
} CeCslDisambiguationBytes;

typedef struct CeCslDisambiguationAdjustment {
    CeCslDisambiguationProfile profile;
    CeCslEvalOccurrence occurrence;
    size_t minimum_names;
    const CeCslNameOverride *overrides;
    size_t override_count;
} CeCslDisambiguationAdjustment;
typedef struct CeCslDisambiguationItem {
    size_t item_index;
    bool disambiguate;
    /* Always resolved, including an empty suffix. Max 50,000 eligible items
     * needs at most four ASCII letters; no inherited input year-suffix. */
    char year_suffix[8];
    uint64_t suffix_ordinal; /* 0 absent; 1=a, 26=z, 27=aa. */
    const CeCslDisambiguationAdjustment *adjustments;
    size_t adjustment_count;
    size_t unresolved_group; /* SIZE_MAX when unambiguous. */
} CeCslDisambiguationItem;

typedef struct CeCslDisambiguationPerson {
    size_t binding, person_index;
    const char *source_variable; /* Canonical immutable input names variable. */
    const CeCslNameIdentity *identity;
    bool rendered, primary, can_initials;
    bool given_visible, given_initialized;
    /* Actual STYLE and INITIALS display keys in this rendering context, from
     * the names renderer/finalizer; never parsed from a citation string. */
    CeCslDisambiguationBytes style_key, initials_key;
} CeCslDisambiguationPerson;
typedef struct CeCslDisambiguationNames {
    CeCslEvalOccurrence occurrence;
    /* Next effective prefix expansion according to resolved name options,
     * considering et-al-use-last/bindings. Zero means no further expansion. */
    size_t next_minimum_names;
    const CeCslDisambiguationPerson *people; /* Includes hidden source names. */
    size_t person_count;
} CeCslDisambiguationNames;
typedef struct CeCslDisambiguationProbe {
    CeCslDisambiguationBytes key; /* Exact finalized collision key. Empty is valid. */
    const CeCslDisambiguationNames *names;
    size_t names_count;
} CeCslDisambiguationProbe;
typedef struct CeCslDisambiguationRequest {
    size_t item_index;
    CeCslDisambiguationProfile profile;
    const CeCslDisambiguationItem *state;
    bool invisible; /* Eligible bibliography item not cited in the input. */
} CeCslDisambiguationRequest;
typedef struct CeCslDisambiguationProvider {
    void *context;
    /* All provider heap allocations MUST use this allocator, including
     * temporary buffers, so the shared workspace bound is meaningful. A
     * callback must also bound its own internal work and honor cancellation;
     * this driver cannot preempt arbitrary external code. No shared mutation.
     * On error leave *out NULL; on success return a releaseable owned probe.
     * Immutable input/style views may be borrowed until release. The request,
     * its state/adjustments and generated scalar bytes must NOT escape: copy
     * those into the probe if needed after the callback returns. */
    CeCslDisambiguationStatus (*render)(void *context,
        const CeCslDisambiguationRequest *request,
        const CeCslOutputAllocator *allocator, CeCslDisambiguationProbe **out,
        char *error, size_t capacity);
    void (*release)(void *context, CeCslDisambiguationProbe *probe);
} CeCslDisambiguationProvider;

typedef struct CeCslDisambiguationOptions {
    bool add_given_names, add_names, use_disambiguate_branch, add_year_suffix;
    CeCslGivenNameRule given_rule;
    /* Zero selects FIRST; every explicit mask must include FIRST. Add
     * SUBSEQUENT when its name form can differ; include
     * BIBLIOGRAPHY when checking that entries retain citation distinctions. */
    unsigned profiles;
    /* Zero selects the hard default, otherwise only lower limits are valid.
     * Name count and emitted bytes accumulate over all probes. */
    size_t max_probes, max_names, max_changes, max_emitted_bytes;
    size_t max_compared_bytes, max_workspace_bytes;
    size_t max_work_units; /* Bounds traversal/sort work even for empty keys. */
    void *cancel_context;
    bool (*cancelled)(void *context);
} CeCslDisambiguationOptions;
typedef struct CeCslDisambiguation CeCslDisambiguation;

/* Pure document-level driver over typed provider results. Finalized keys are
 * required: CeCslEvaluation previews are NOT valid collision keys. Mock-provider
 * tests establish driver behavior, not APA rendering or full CSL conformance.
 *
 * This first implementation enables PRIMARY_NAME_INITIALS. Other valid rules
 * return UNSUPPORTED only when add_given_names is enabled. No guessed fallback.
 * Method order is given-name expansion, added names, conditional disambiguation,
 * then suffixes in registry bibliography order. Explicitly included uncited
 * bibliography items participate through invisible citation probes.
 * This initial profile rejects added-name trials that merge previously
 * distinct collision groups, rather than guessing a non-monotone solution.
 *
 * Rebuild from the original style state after every document change; previous
 * overrides are not inputs, so deletion can remove expansion/suffixes. Success
 * atomically replaces *out; errors/cancellation preserve the previous result.
 * Unresolved collisions after all enabled methods are a successful result with
 * explicit unresolved groups, not an implementation error or a claimed fix.
 * Initialize *out to NULL; per-object allocator context must outlive result.
 * Source XML nodes in occurrence identities remain borrowed; the same immutable
 * style and input revision must be used when applying the result. */
CeCslDisambiguationStatus ce_csl_disambiguation_build(const CeCslInput *input,
    const CeCslRegistry *registry, const CeCslDisambiguationOptions *options,
    const CeCslDisambiguationProvider *provider, const CeCslOutputAllocator *allocator,
    CeCslDisambiguation **out, char *error, size_t capacity);
void ce_csl_disambiguation_free(CeCslDisambiguation *result);
const CeCslDisambiguationItem *ce_csl_disambiguation_item(
    const CeCslDisambiguation *result, size_t input_item_index);
size_t ce_csl_disambiguation_unresolved_count(const CeCslDisambiguation *result);
/* Read-only adapter for evaluator adjust_names. No matching adjustment is a
 * successful zero adjustment. Occurrence comparison includes every macro call. */
bool ce_csl_disambiguation_adjust_names(const CeCslDisambiguationItem *item,
    CeCslDisambiguationProfile profile, const CeCslEvalOccurrence *occurrence,
    CeCslEvalNameAdjustment *out);

#endif
