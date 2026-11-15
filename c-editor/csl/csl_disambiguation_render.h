#ifndef CE_CSL_DISAMBIGUATION_RENDER_H
#define CE_CSL_DISAMBIGUATION_RENDER_H

#include "csl_disambiguation.h"
#include "csl_finalize.h"

#define CE_CSL_DISAMBIGUATION_RENDER_MAX_VARIANTS ((size_t)256)
#define CE_CSL_DISAMBIGUATION_RENDER_MAX_NODES ((size_t)262144)
#define CE_CSL_DISAMBIGUATION_RENDER_MAX_PEOPLE ((size_t)16384)
#define CE_CSL_DISAMBIGUATION_RENDER_MAX_WORK ((size_t)1048576)

typedef struct CeCslDisambiguationRenderConfig {
    const CeCslStyle *style;
    const CeCslLocale *locale;
    const CeCslInput *input;
    const CeCslRegistry *registry;
    void *short_context;
    CeCslVariableStatus (*short_value)(void *context, const char *canonical_name, const cJSON *long_value,
                                       CeCslVariableShortValue *value, char *error, size_t capacity);
    void *cancel_context;
    bool (*cancelled)(void *context);
    /* Zero selects the named hard default; only lower positive limits allowed.
     * Nodes/work include all INITIALS candidate reevaluations. Variants excludes
     * the initial STYLE evaluation. People includes empty source placeholders. */
    size_t max_variants, max_nodes, max_people, max_work;
} CeCslDisambiguationRenderConfig;

/* Allocation-free adapter; config must survive its render calls. Every probe
 * owns its evaluation/finalization and copied candidate keys. Probe release
 * does not access config, request, state, overrides or callback contexts.
 * Immutable input/style/locale must remain alive until probes are released.
 * The supplied allocator context must likewise survive release. All retained
 * and temporary heap allocations use that allocator and share the driver's
 * workspace; this provider additionally enforces a per-probe 64 MiB bound.
 *
 * FIRST/SUBSEQUENT use an empty, no-locator cite; BIBLIOGRAPHY uses cite=NULL.
 * Numbers come from registry; year-suffix is resolved even when empty, and
 * first-reference-note-number is resolved absent. No inherited source suffix.
 * A nonempty suffix requires an explicit year-suffix reference reachable from
 * the selected section, including all macro/choose branches. Missing implicit
 * suffix insertion is UNSUPPORTED, never guessed. A reachable reference in an
 * unchosen branch can legitimately leave a collision unresolved.
 *
 * Keys use finalized UTF-8 and typed PERSON spans, never preview strings.
 * INITIALS candidates reevaluate the whole context (parent case/strip included).
 * Hidden people retain empty keys; their initialization capability is tested
 * with a temporary visible prefix, which never changes the main probe.
 * All bindings, including collapsed roles/placeholders, retain original indices.
 * The first nonempty visible person in output order is the sole primary.
 * Limits/cancellation fail explicitly; no partial or simplified fallback.
 * This is a provider for the bounded driver profile, not a full CSL processor. */
CeCslDisambiguationProvider
ce_csl_disambiguation_render_provider(const CeCslDisambiguationRenderConfig *config);
/* *out must be NULL. Failure leaves it NULL; use provider.release or the direct
 * release function below on success. NULL allocator selects malloc/free. */
CeCslDisambiguationStatus ce_csl_disambiguation_render(const CeCslDisambiguationRenderConfig *config,
                                                       const CeCslDisambiguationRequest *request,
                                                       const CeCslOutputAllocator *allocator,
                                                       CeCslDisambiguationProbe **out, char *error,
                                                       size_t capacity);
void ce_csl_disambiguation_render_release(CeCslDisambiguationProbe *probe);
/* Optional owned-result views for document orchestration. Borrowed until release.
 * Strings exported from these outputs inherit the probe allocator; release
 * those strings before releasing the probe. No callbacks are invoked after
 * render returns, including by these subsequent output exports. */
const CeCslEvaluation *ce_csl_disambiguation_render_evaluation(const CeCslDisambiguationProbe *probe);
const CeCslFinalized *ce_csl_disambiguation_render_finalized(const CeCslDisambiguationProbe *probe);
#endif
