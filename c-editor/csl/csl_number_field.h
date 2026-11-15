#ifndef CE_CSL_NUMBER_FIELD_H
#define CE_CSL_NUMBER_FIELD_H
#include "csl_number.h"

typedef struct CeCslNumberFieldInfo {
    bool numeric;
    size_t number_count;
    bool single_greater_than_one;
} CeCslNumberFieldInfo;

/* CSL numeric-field grammar: unsigned ASCII decimal numbers, each optionally
 * surrounded by letter/combining-mark affixes; comma, hyphen/en-dash or '&'
 * separate numbers, with optional Unicode whitespace. A token has one decimal
 * component. Decimal points, signed numbers, slash, semicolon, words such as
 * "and", or embedded whitespace in a token are non-numeric and remain text.
 * This deliberately does not adopt undocumented citeproc-js extensions.
 * UTF-8/NUL/control/size errors fail; ordinary non-numeric text succeeds with
 * numeric=false. No allocation. Failure leaves info unchanged. */
bool ce_csl_number_field_scan(const char *text,size_t size,CeCslNumberFieldInfo *info,
    char *error,size_t capacity);

typedef struct CeCslNumberFieldFirst {
    bool numeric; /* The entire field, not merely a numeric prefix. */
    size_t offset, length; /* Borrowed original first component, including affixes. */
} CeCslNumberFieldFirst;
/* Shared page-first extraction. Validates the whole field with scan's grammar.
 * Numeric fields return the first component without surrounding whitespace;
 * spelling/letter affixes/leading zeros are unchanged. Ordinary text succeeds
 * with numeric=false and offset=length=0; invalid input preserves result. */
bool ce_csl_number_field_first(const char *text,size_t size,
    CeCslNumberFieldFirst *result,char *error,size_t capacity);

/* Render numeric content, normalizing separator whitespace as required by CSL.
 * A bare decimal is converted individually; affixed numbers are never
 * ordinalized/Romanized. Non-numeric content is copied byte-for-byte. Unicode
 * affixes and source hyphen/en-dash choice are preserved. Page-range shortening
 * and locator dash substitution are separate, explicit caller operations.
 * Scalar forms/limits follow csl_number.h. Bare numeric-form decimals retain
 * their exact digit spelling (including leading zeros and >uint64_t values).
 * Atomic output and optional missing_term replacement; info and output may
 * only be observed after success. */
bool ce_csl_number_field_render(const char *text,size_t size,CeCslNumberForm form,
    const CeCslLocale *locale,CeCslLocaleGender gender,
    const CeCslOutputAllocator *allocator,CeCslOutputString *output,
    bool *missing_term,char *error,size_t capacity);
#endif
