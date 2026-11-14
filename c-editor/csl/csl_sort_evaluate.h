#ifndef CE_CSL_SORT_EVALUATE_H
#define CE_CSL_SORT_EVALUATE_H

#include "csl_collation.h"
#include "csl_evaluate.h"

#define CE_CSL_SORT_EVALUATE_MAX_WORKSPACE ((size_t)256 * 1024 * 1024)

typedef enum CeCslSortDomain {
    CE_CSL_SORT_LEXICAL, CE_CSL_SORT_INTEGER, CE_CSL_SORT_DATE,
    CE_CSL_SORT_MONTH_DAY, CE_CSL_SORT_MONTH, CE_CSL_SORT_DAY
} CeCslSortDomain;
typedef struct CeCslSortEvaluated {
    CeCslSortKey key;
    CeCslSortDomain domain;
    CeCslCollationLanguage language;
    bool missing, descending;
} CeCslSortEvaluated;

/* Evaluate one original cs:key belonging to context.section's cs:sort.
 * The context supplies immutable item/cite/generated values and the same
 * environment as semantic evaluation; entry/sorting/key-name overrides are
 * selected here. No citation numbers, positions or suffixes are assigned.
 *
 * Direct names use the complete LONG list and ALL sort order. Macro names
 * retain actual truncation/substitution/initialization, with cs:key overrides.
 * Structured SortViews supply name components; literal names lose a leading
 * English article. Text and name keys share a lexical tuple encoding, allowing
 * a macro's title substitute to compare with its name branch. Collation is
 * the bounded DUCET adapter (not whole-locale CLDR tailoring).
 *
 * Direct integers and macro cs:number/count-names use exact integer keys.
 * Wholly nonnumeric number values use text; ranges, prefixes and decimals are
 * explicitly unsupported. Dates use typed endpoints, selected macro parts,
 * and ignore seasons. Year-containing masks share DATE with unselected parts
 * zero; masks without a year have separate domains. Literal/open dates,
 * year/day-without-month masks, mixed typed/text or multiple typed macro
 * leaves, and outer text decorations of typed macro keys are unsupported.
 * Pure-text macros use finalizer plain text. No rendered string is parsed to
 * recover names or dates, and no unsupported typed key becomes a text key.
 *
 * Initialize out to {0}/live. Success atomically replaces it; failures preserve
 * it and inputs. Result owns its key only; allocator context outlives result.
 * Operation allocations share MAX_WORKSPACE; the old result and final owned
 * copy are excluded. Child module limits also apply. Mojibake's internal
 * allocations follow the separate documented COLLATION.md contract. */
bool ce_csl_sort_evaluate(const CeCslEvalRequest *context,
    const CeCslXmlNode *key, CeCslCollationLanguage language,
    const CeCslOutputAllocator *allocator, CeCslSortEvaluated *out,
    char *error, size_t capacity);
void ce_csl_sort_evaluated_free(CeCslSortEvaluated *value);
CeCslSortKeyView ce_csl_sort_evaluated_view(const CeCslSortEvaluated *value);

/* Safe row-major matrix adapter. Before invoking the existing stable sorter,
 * checks each column's direction and all present keys' encoding/language.
 * Missing values do not constrain a column's domain and remain last in either
 * direction. Mixing numeric/text number values or differing no-year date masks
 * is currently unsupported, never silently ordered by unrelated byte encodings.
 * Atomic ownership and bounds follow ce_csl_sort_order. */
bool ce_csl_sort_evaluated_order(const CeCslSortEvaluated *keys,
    size_t rows, size_t columns, const CeCslOutputAllocator *allocator,
    CeCslSortOrder *out, char *error, size_t capacity);

#endif
