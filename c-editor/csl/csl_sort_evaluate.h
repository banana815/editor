#ifndef CE_CSL_SORT_EVALUATE_H
#define CE_CSL_SORT_EVALUATE_H

#include "csl_collation.h"
#include "csl_evaluate.h"

#define CE_CSL_SORT_EVALUATE_MAX_WORKSPACE ((size_t)256 * 1024 * 1024)

typedef enum CeCslSortDomain {
    CE_CSL_SORT_LEXICAL, CE_CSL_SORT_NUMBER, CE_CSL_SORT_DATE,
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
 * Number variables, macro cs:number and count-names share NUMBER's tagged
 * integer/text domain. A bare signed integer retains exact signed ordering.
 * Otherwise, after inline markup removal, the ENTIRE field must satisfy the
 * shared number_field grammar before its first complete component's decimal
 * is used as the exact numeric key (at most 4096 digits, including leading
 * zeros). Letter affixes and later components create no hidden tie-breaker.
 * Non-numeric content uses its complete case-insensitive collation key, never
 * an arbitrary numeric prefix: "12 special", "12.5" and "1e3" remain text.
 * Integer keys precede text keys ascending, reverse descending. Equal first
 * numbers proceed to later style keys or retain input order. These direct and
 * macro rules are intentionally consistent with CSL sorting-macros, rather
 * than adopting citeproc-js's differing macro behavior/arbitrary truncation.
 * Dates use typed endpoints, selected macro parts,
 * and ignore seasons. Year-containing masks share DATE with unselected parts
 * zero; masks without a year have separate domains. Literal dates have no
 * chronological key and are missing FOR SORTING ONLY, retaining their original
 * renderable literal; they tie with absent dates at this key and sort last in
 * both directions, with later keys/stable input order resolving the tie.
 * Open dates,
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
 * direction. NUMBER's integer/text members may mix, but the whole NUMBER or
 * LEXICAL column must use one collation language. Differing no-year date masks
 * or unrelated domains remain unsupported, rather than comparing their bytes.
 * Atomic ownership and bounds follow ce_csl_sort_order. */
bool ce_csl_sort_evaluated_order(const CeCslSortEvaluated *keys,
    size_t rows, size_t columns, const CeCslOutputAllocator *allocator,
    CeCslSortOrder *out, char *error, size_t capacity);

#endif
