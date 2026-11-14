#ifndef CE_CSL_EVALUATE_H
#define CE_CSL_EVALUATE_H

#include "csl_date.h"
#include "csl_inline.h"
#include "csl_names.h"
#include "csl_number_field.h"
#include "csl_style.h"
#include "csl_variable.h"

#define CE_CSL_EVALUATE_MAX_DEPTH ((size_t)128)
#define CE_CSL_EVALUATE_MAX_NODES ((size_t)65536)
#define CE_CSL_EVALUATE_MAX_VARIABLES ((size_t)256)
/* Aggregate bound leaf payload for ONE evaluation, not a per-leaf allowance.
 * Counts copied source scalar/name strings PLUS the rendered leaf body; repeated
 * uses count again. Thus a single unmodified scalar of 512 KiB exactly fills
 * the 1 MiB budget (source copy + rendered body). Borrowed input JSON is excluded.
 * Intermediate/retained previews, node arenas and child module allocations all
 * additionally share the HEAP workspace budget. Each preview is <= MAX_BYTES.
 * Expanded-node work includes discarded substitute candidates. Evaluation uses
 * bounded C recursion; embedders should provide at least a 1 MiB thread stack. */
#define CE_CSL_EVALUATE_MAX_BYTES CE_CSL_OUTPUT_MAX_BYTES
#define CE_CSL_EVALUATE_MAX_WORKSPACE ((size_t)128 * 1024 * 1024)

typedef struct CeCslEvaluation CeCslEvaluation;
typedef struct CeCslEvalNode CeCslEvalNode;
typedef enum CeCslEvalKind {
    CE_CSL_EVAL_LAYOUT, CE_CSL_EVAL_GROUP, CE_CSL_EVAL_CHOOSE,
    CE_CSL_EVAL_MACRO, CE_CSL_EVAL_TEXT, CE_CSL_EVAL_NAMES,
    CE_CSL_EVAL_DATE, CE_CSL_EVAL_NUMBER, CE_CSL_EVAL_LABEL
} CeCslEvalKind;
typedef enum CeCslEvalDisplay {
    CE_CSL_EVAL_DISPLAY_NONE, CE_CSL_EVAL_DISPLAY_BLOCK,
    CE_CSL_EVAL_DISPLAY_LEFT_MARGIN, CE_CSL_EVAL_DISPLAY_RIGHT_INLINE,
    CE_CSL_EVAL_DISPLAY_INDENT
} CeCslEvalDisplay;
typedef enum CeCslEvalSuppression {
    CE_CSL_EVAL_NOT_SUPPRESSED, CE_CSL_EVAL_EMPTY_VARIABLE,
    CE_CSL_EVAL_ALL_GROUP_VARIABLES_EMPTY, CE_CSL_EVAL_NO_CHOSEN_BRANCH
} CeCslEvalSuppression;
typedef struct CeCslEvalOccurrence {
    const CeCslXmlNode *source;
    /* Original cs:text macro call sites from outermost to innermost. Together
     * with source, this distinguishes repeated uses of the same names macro.
     * Callback arrays are temporary; result arrays are copied and stable. */
    const CeCslXmlNode *const *macro_calls;
    size_t macro_depth;
} CeCslEvalOccurrence;
typedef struct CeCslEvalNameAdjustment {
    size_t minimum_names;
    const CeCslNameOverride *overrides;
    size_t override_count;
} CeCslEvalNameAdjustment;
typedef struct CeCslEvalRequest {
    const CeCslStyle *style;
    const CeCslLocale *locale;
    const CeCslXmlNode *section; /* Style's citation or bibliography; bibliography requires cite=NULL. */
    const CeCslXmlNode *entry;   /* NULL selects section's layout. */
    CeCslVariableContext variables;
    unsigned position;
    bool disambiguate, sorting;
    unsigned base_format;
    const char *item_language;  /* NULL derives item.language when present. */
    bool has_sort_min, has_sort_first, has_sort_last;
    size_t sort_min, sort_first;
    bool sort_last;
    void *name_context;
    /* Read-only query. Must not mutate caller-owned inputs or shared state.
     * Returned arrays must survive until the next callback or evaluate return.
     * The evaluator supplies locale/formatting/allocator and validates bounds. */
    bool (*adjust_names)(void *context, const CeCslEvalOccurrence *occurrence,
        const CeCslNamesBinding *bindings, size_t binding_count,
        const CeCslNameOptions *options, CeCslEvalNameAdjustment *adjustment,
        char *error, size_t capacity);
} CeCslEvalRequest;
typedef struct CeCslEvalDecoration {
    unsigned inherited_format, effective_format;
    CeCslTextCase text_case;
    bool quotes, strip_periods;
    CeCslEvalDisplay display;
    const char *prefix, *suffix, *delimiter;
    bool delimiter_boundary;
    /* Citation layout delimiter/affixes belong to the whole citation cluster,
     * not fields within this one cite. Kept for document orchestration. */
    bool cluster_envelope;
} CeCslEvalDecoration;
typedef struct CeCslEvalVariableUse {
    CeCslVariableValue value;
    bool rendered; /* Actual variable content, excluding descriptive labels. */
} CeCslEvalVariableUse;
typedef struct CeCslEvalNodeInfo {
    size_t index;
    CeCslEvalKind kind;
    CeCslEvalOccurrence occurrence;
    const CeCslXmlNode *target; /* Chosen branch or resolved macro definition. */
    const CeCslEvalNode *parent, *first_child, *next;
    CeCslEvalDecoration decoration;
    /* Descriptive labels keep their source use/presence, but do not constitute
     * variable calls for group suppression or consume a substitution variable. */
    bool variable_called, metadata_present, rendered_variable_present;
    bool nonempty, used_substitute, missing_term;
    CeCslEvalSuppression suppression;
    CeCslInlineDiagnostic inline_diagnostic;
} CeCslEvalNodeInfo;
typedef struct CeCslEvalSummary {
    size_t node_count, leaf_bytes;
    bool nonempty, missing_term, literal_markup, malformed_markup;
} CeCslEvalSummary;

/* Semantic tree evaluation, NOT a complete citation/document formatter.
 * Keeps names/typed variables/date sources/display boundaries for subsequent
 * sorting, disambiguation and collapse. It does not assign numbers or suffixes,
 * perform locale collation, join citation clusters, inject implicit year suffixes,
 * render display layout or apply cross-node punctuation cleanup. Preview strings are therefore NOT final
 * collision keys. Unknown/unimplemented features fail explicitly.
 *
 * All calls have isolated transactional substitution state. Predicates only read
 * it; empty candidates roll back owned objects and consumed variables. A winner
 * commits canonical variable dependencies only. Inputs/callback state are never
 * mutated. Source style/locale/input remain borrowed until result free. Bound
 * generated/provider scalar bytes are copied. Request/callback contexts and
 * generated-slot/cite wrappers need survive only this call; node source JSON
 * values (including any cited locator JSON) still follow the input lifetime.
 * Custom allocator context must
 * outlive the result; NULL selects malloc/free. Initialize *out to NULL/live;
 * all errors, including OOM and limits, preserve *out and all source objects. */
bool ce_csl_evaluate(const CeCslEvalRequest *request,
    const CeCslOutputAllocator *allocator, CeCslEvaluation **out,
    char *error, size_t capacity);
void ce_csl_evaluation_free(CeCslEvaluation *evaluation);
const CeCslEvalNode *ce_csl_evaluation_root(const CeCslEvaluation *evaluation);
const CeCslEvalSummary *ce_csl_evaluation_summary(const CeCslEvaluation *evaluation);
const CeCslLocale *ce_csl_evaluation_locale(const CeCslEvaluation *evaluation);
const char *ce_csl_evaluation_item_language(const CeCslEvaluation *evaluation);
const char *ce_csl_evaluation_default_locale(const CeCslEvaluation *evaluation);
const CeCslEvalNodeInfo *ce_csl_eval_node_info(const CeCslEvalNode *node);
size_t ce_csl_eval_node_variable_count(const CeCslEvalNode *node);
const CeCslEvalVariableUse *ce_csl_eval_node_variable(const CeCslEvalNode *node, size_t index);
const CeCslNamesResult *ce_csl_eval_node_names(const CeCslEvalNode *node);
const CeCslDateResult *ce_csl_eval_node_date(const CeCslEvalNode *node);
const CeCslDateValueView *ce_csl_eval_node_date_value(const CeCslEvalNode *node);
unsigned ce_csl_eval_node_date_parts(const CeCslEvalNode *node);
const CeCslNumberFieldInfo *ce_csl_eval_node_number(const CeCslEvalNode *node);
/* Leaf body before THIS node's decoration. Names/date inner formatting remains
 * applied once. Containers/substitution winners use children and return NULL.
 * Retained independently of preview so finalization never reverses strings. */
const CeCslInline *ce_csl_eval_node_content(const CeCslEvalNode *node);
/* Locally decorated inline preview, preserving nocase/font/source metadata.
 * Exact for implemented local operations, but excludes document-level quote
 * punctuation, citation layout envelope and display positioning. */
const CeCslInline *ce_csl_eval_node_preview(const CeCslEvalNode *node);

#endif
