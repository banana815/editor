#ifndef CE_CSL_NUMBER_H
#define CE_CSL_NUMBER_H

#include "csl_locale.h"
#include "csl_output.h"

#define CE_CSL_NUMBER_MAX_DECIMAL_DIGITS ((size_t)4096)
#define CE_CSL_NUMBER_MAX_DELIMITER_BYTES ((size_t)4096)

typedef enum CeCslNumberForm {
    CE_CSL_NUMBER_NUMERIC, CE_CSL_NUMBER_ORDINAL,
    CE_CSL_NUMBER_LONG_ORDINAL, CE_CSL_NUMBER_ROMAN
} CeCslNumberForm;
typedef enum CeCslPageRangeFormat {
    CE_CSL_PAGE_EXPANDED, CE_CSL_PAGE_MINIMAL, CE_CSL_PAGE_MINIMAL_TWO,
    CE_CSL_PAGE_CHICAGO, CE_CSL_PAGE_CHICAGO_15, CE_CSL_PAGE_CHICAGO_16
} CeCslPageRangeFormat;
typedef enum CeCslNumberStatus {
    CE_CSL_NUMBER_OK, CE_CSL_NUMBER_INVALID_ARGUMENT, CE_CSL_NUMBER_INVALID_UTF8,
    CE_CSL_NUMBER_UNSUPPORTED, CE_CSL_NUMBER_LIMIT, CE_CSL_NUMBER_OOM
} CeCslNumberStatus;

/* Scalar primitives, not the complete cs:number field lexer/renderer. Parsing
 * mixed fields, numeric affixes, separators, signed values and arbitrary page
 * text belongs to the caller. No floating-point conversion or English suffix
 * assumptions occur here.
 *
 * Numeric supports all uint64_t values. Ordinal/long-ordinal use the locale's
 * existing selection/fallback/gender logic. A missing term becomes an empty
 * suffix, preserving the number and setting optional missing_term=true. A
 * found-but-empty term is intentional and does not set that diagnostic. NULL
 * locale behaves as missing terms. A complete long ordinal replaces the number.
 * Roman currently supports 1..3999 in lowercase; other values explicitly return
 * UNSUPPORTED rather than inventing an extended Roman numeral convention.
 *
 * out must be initialized to {0} or a live CeCslOutputString. Success atomically
 * replaces it; failure preserves out and missing_term. Free with
 * ce_csl_output_string_free(). Allocator and lifetime rules match csl_output.h;
 * NULL allocator selects malloc/free. Output is bounded by OUTPUT_MAX_BYTES. */
CeCslNumberStatus ce_csl_number_format(uint64_t number, CeCslNumberForm form,
    const CeCslLocale *locale, CeCslLocaleGender gender,
    const CeCslOutputAllocator *allocator, CeCslOutputString *out,
    bool *missing_term, char *error, size_t capacity);

/* Formats a single unsigned decimal page-range pair. Inputs are byte-counted
 * ASCII digits, not NUL-dependent. This is not a mixed page-string tokenizer:
 * letter prefixes, punctuation, signs and surrounding spaces are rejected.
 * The shorter end is expanded to the smallest value >= start with its supplied
 * decimal suffix; full-width descending endpoints return UNSUPPORTED. Equal
 * endpoints remain explicit. Both values keep input leading-zero widths;
 * leading-zero endpoints are expanded but never abbreviated.
 *
 * Decimal operations are exact up to MAX_DECIMAL_DIGITS, beyond uint64_t.
 * Chicago is the CSL 1.0.2 alias for chicago-15, including its four-digit rule;
 * chicago-16 is a distinct enum. The caller supplies the locale's range
 * delimiter explicitly. NULL is invalid; an explicit empty string is valid.
 * Delimiters are exact UTF-8 with no NUL or unsupported control characters.
 * Allocation/atomic result ownership follows ce_csl_number_format(). */
CeCslNumberStatus ce_csl_page_range_pair(const char *start, size_t start_size,
    const char *end, size_t end_size, CeCslPageRangeFormat format,
    const char *delimiter, size_t delimiter_size,
    const CeCslOutputAllocator *allocator, CeCslOutputString *out,
    char *error, size_t capacity);

const char *ce_csl_number_status_string(CeCslNumberStatus status);

#endif
