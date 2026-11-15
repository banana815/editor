#ifndef CE_CSL_PAGE_FIELD_H
#define CE_CSL_PAGE_FIELD_H
#include "csl_number_field.h"

typedef enum CeCslPageFieldPolicy {
    CE_CSL_PAGE_FIELD_PAGE, CE_CSL_PAGE_FIELD_PAGE_LOCATOR,
    CE_CSL_PAGE_FIELD_OTHER_LOCATOR
} CeCslPageFieldPolicy;
typedef struct CeCslPageFieldOptions {
    CeCslPageFieldPolicy policy;
    bool has_format;
    CeCslPageRangeFormat format;
} CeCslPageFieldOptions;
typedef struct CeCslPageFieldInfo {
    bool numeric, missing_term;
    size_t range_count; /* Ranges actually formatted; zero for inactive/literal fields. */
} CeCslPageFieldInfo;

/* Mixed page/locator fields, using the shared number-field grammar and exact
 * decimal page-pair primitive. PAGE without has_format is byte-for-byte literal.
 * Active page ranges preserve comma/& separators, all whitespace, and letter/
 * combining-mark affixes on BOTH endpoints. Identical prefix/suffix pairs allow
 * decimal expansion/shortening; different affixes preserve both digit strings
 * (no inferred common numbering domain). Adjacent ranges such as 1-2-3 are
 * explicitly unsupported when a range format is active. Non-numeric PAGE text
 * remains exact, including ordinary hyphenated prose. No prefix-only parsing.
 *
 * PAGE_LOCATOR applies page-range-format only for a caller-confirmed page label.
 * This follows the CSL maintainers' clarification, not an explicit sentence in
 * the 1.0.2 specification: https://discourse.citationstyles.org/t/1178/4 . Other
 * locators never shorten. Without active shortening, locator hyphens become
 * en-dashes; other bytes/whitespace are untouched, even in nonnumeric prose.
 * The locale page-range-delimiter applies to active numeric page ranges. An
 * explicitly empty term is honored; a missing term uses the specification's
 * en-dash default and reports missing_term=true only if a range uses it.
 *
 * Recognized inline markup is unsupported when transformation is active, rather
 * than splitting tags or losing formatting. Unknown tag-like literal fields
 * remain byte-for-byte literal; markup support belongs to a later typed layer.
 * UTF-8/NUL/control/1 MiB limits match number_field. Pair decimals are bounded by
 * NUMBER_MAX_DECIMAL_DIGITS. Output/info replacement is atomic, including every
 * allocation failure. NULL allocator selects malloc/free; free output with
 * ce_csl_output_string_free. Inputs need live only during the call. */
bool ce_csl_page_field_render(const char *text, size_t size,
    const CeCslPageFieldOptions *options, const CeCslLocale *locale,
    const CeCslOutputAllocator *allocator, CeCslOutputString *output,
    CeCslPageFieldInfo *info, char *error, size_t capacity);
#endif
