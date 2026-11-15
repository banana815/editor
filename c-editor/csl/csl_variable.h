#ifndef CE_CSL_VARIABLE_H
#define CE_CSL_VARIABLE_H

#include "csl_condition.h"
#include "csl_input.h"
#include "csl_output.h"

#define CE_CSL_VARIABLE_MAX_NAME_BYTES ((size_t)63)
#define CE_CSL_VARIABLE_MAX_TEXT_BYTES CE_CSL_OUTPUT_MAX_BYTES

typedef enum CeCslVariableKind {
    CE_CSL_VARIABLE_TEXT, CE_CSL_VARIABLE_NUMBER,
    CE_CSL_VARIABLE_DATE, CE_CSL_VARIABLE_NAMES
} CeCslVariableKind;
typedef enum CeCslVariableState {
    CE_CSL_VARIABLE_MISSING, CE_CSL_VARIABLE_EMPTY,
    CE_CSL_VARIABLE_VALUE, CE_CSL_VARIABLE_SUPPRESSED
} CeCslVariableState;
typedef enum CeCslVariableStatus {
    CE_CSL_VARIABLE_OK, CE_CSL_VARIABLE_INVALID,
    CE_CSL_VARIABLE_UNKNOWN, CE_CSL_VARIABLE_UNRESOLVED,
    CE_CSL_VARIABLE_UNSUPPORTED, CE_CSL_VARIABLE_LIMIT
} CeCslVariableStatus;
typedef enum CeCslVariableForm {
    CE_CSL_VARIABLE_LONG, CE_CSL_VARIABLE_SHORT
} CeCslVariableForm;

typedef struct CeCslGeneratedNumber {
    bool resolved, present;
    uint64_t value;
} CeCslGeneratedNumber;
typedef struct CeCslGeneratedText {
    bool resolved;
    const char *text; size_t size;
} CeCslGeneratedText;
typedef struct CeCslGeneratedVariables {
    CeCslGeneratedNumber citation_number, first_reference_note_number;
    CeCslGeneratedText year_suffix, citation_label;
} CeCslGeneratedVariables;

typedef struct CeCslVariableValue {
    CeCslVariableKind kind;
    CeCslVariableState state;
    const char *canonical_name; /* Static vocabulary key; aliases share this. */
    const cJSON *node;          /* Borrowed original typed field, if any. */
    const char *borrowed_text;
    size_t text_size;
    char numeric_text[64];     /* Owned inline scalar spelling; no self-pointer. */
    bool text_is_inline, derived;
    const char *dependencies[2]; size_t dependency_count;
    bool uncertain_date;      /* Validated date flag; false for empty values. */
} CeCslVariableValue;
typedef struct CeCslVariableShortValue {
    bool found;               /* found + size=0 is intentional empty text. */
    const char *text; size_t size;
} CeCslVariableShortValue;
typedef struct CeCslVariableContext {
    const CeCslInputItem *item;
    const CeCslInputCite *cite; /* NULL for bibliography/no current cite. */
    const CeCslGeneratedVariables *generated;
    void *context;
    bool (*suppressed)(void *context, const char *canonical_name,
        bool *suppressed, char *error, size_t capacity);
    CeCslVariableStatus (*short_value)(void *context, const char *canonical_name,
        const cJSON *long_value, CeCslVariableShortValue *value,
        char *error, size_t capacity);
} CeCslVariableContext;

/* Read immutable items/cites from the strict input layer. No I/O, allocation,
 * input normalization or registry mutation. The optional callbacks must return
 * borrowed values that outlive the result; the evaluator owns suppression and
 * abbreviation tables. The result is safely copyable and never points into its
 * own numeric_text: use the accessor below. All failures preserve *out.
 *
 * MISSING means absent metadata or a computed absent value; EMPTY means an
 * explicit null/empty value; SUPPRESSED means the evaluator blocked the value.
 * Unknown variables, unresolved generated slots, invalid data/callback results,
 * and unsupported derivations are errors and never reported as MISSING. A
 * shared names/date validator rejection returns INVALID with its diagnostic,
 * including any explicit unsupported-syntax restriction of that validator.
 *
 * Generated citation numbers/preceding-note numbers must be resolved externally
 * and positive when present. Source metadata never supplies those two counters.
 * Resolved generated year-suffix/citation-label wins; otherwise an explicit
 * source value is used, or UNRESOLVED is returned. Generated integers are exact
 * uint64; ordinary JSON number fields accept only safe integral doubles within
 * +/-9007199254740991. Other numeric nodes are UNSUPPORTED; provide original
 * decimal spelling as a string to retain decimals, larger values or zeros.
 *
 * Long text never falls back to short. Short title/container-title first reads
 * the matching standard *-short property, then the optional short provider,
 * then long. Explicit empty stops fallback. Other short requests use the
 * provider then long, without inventing metadata fields or abbreviations.
 * Deprecated title-short/container-title-short queries select canonical SHORT.
 * event is a canonical event-title alias: the modern property wins (even empty).
 * Name/date SHORT queries are invalid; their rendering forms belong to their
 * own modules. Text is exact UTF-8, untrimmed, with no NUL/unsupported controls.
 *
 * Explicit editor-translator wins, even empty. Otherwise nonempty, unsuppressed
 * editor and translator arrays must compare structurally equal through the
 * shared names parser. Explicit page-first wins, even empty; otherwise the
 * shared numeric-field lexer derives the first complete component from page.
 * A present nonnumeric page has an UNSUPPORTED derivation, not a missing value.
 * Derived views record dependencies; the caller decides when to commit any
 * substitution suppression. Resolving a derived value alone consumes nothing.
 */
CeCslVariableStatus ce_csl_variable_resolve(const CeCslVariableContext *context,
    const char *variable, CeCslVariableForm form, CeCslVariableValue *out,
    char *error, size_t capacity);

/* Borrowed byte view; safely handles copied results and page-first substrings.
 * May not be NUL-terminated. Returns NULL for names/dates; empty scalar states
 * return a static empty string. size is set only when non-NULL. */
const char *ce_csl_variable_text(const CeCslVariableValue *value, size_t *size);

/* Direct CeCslConditionContext.variable adapter. Resolves LONG, then shares
 * numeric grammar with csl_number_field. Failure preserves *facts. */
bool ce_csl_variable_condition(void *context, const char *variable,
    CeCslVariableFacts *facts, char *error, size_t capacity);
const char *ce_csl_variable_status_string(CeCslVariableStatus status);

#endif
