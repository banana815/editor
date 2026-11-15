#ifndef CE_CSL_LOCALE_H
#define CE_CSL_LOCALE_H

#include "csl_xml.h"
#include <stdint.h>

#define CE_CSL_LOCALE_MAX_SOURCES ((size_t)256)
#define CE_CSL_LOCALE_MAX_TERMS ((size_t)65536)
#define CE_CSL_LOCALE_TAG_CAPACITY ((size_t)64)

typedef enum CeCslLocaleForm {
    CE_CSL_LOCALE_LONG, CE_CSL_LOCALE_SHORT, CE_CSL_LOCALE_VERB,
    CE_CSL_LOCALE_VERB_SHORT, CE_CSL_LOCALE_SYMBOL
} CeCslLocaleForm;
typedef enum CeCslLocaleGender {
    CE_CSL_LOCALE_NEUTER, CE_CSL_LOCALE_MASCULINE, CE_CSL_LOCALE_FEMININE
} CeCslLocaleGender;
typedef enum CeCslLocaleDate {
    CE_CSL_LOCALE_DATE_TEXT, CE_CSL_LOCALE_DATE_NUMERIC
} CeCslLocaleDate;
typedef enum CeCslLocaleOption {
    CE_CSL_LOCALE_PUNCTUATION_IN_QUOTE, CE_CSL_LOCALE_DAY_ORDINALS_ONLY_FIRST
} CeCslLocaleOption;

typedef struct CeCslLocalePrimary {
    const char *language;
    const char *primary_dialect;
} CeCslLocalePrimary;
typedef struct CeCslLocaleCatalog {
    const CeCslXmlNode *const *roots; /* Standalone cs:locale roots. */
    size_t root_count;
    /* Optional additional mappings for languages absent from the pinned CSL
     * metadata. Conflicting or duplicate mappings are errors, not overrides. */
    const CeCslLocalePrimary *primary;
    size_t primary_count;
} CeCslLocaleCatalog;
typedef struct CeCslLocaleAllocator {
    void *context;
    void *(*allocate)(void *context, size_t bytes);
    void (*deallocate)(void *context, void *pointer);
} CeCslLocaleAllocator;
typedef struct CeCslLocale CeCslLocale;
typedef struct CeCslLocaleTerm {
    bool found; /* Empty text is still found and stops fallback. */
    const char *text; /* Borrowed exact UTF-8, or static "" when missing. */
    size_t length;
    const CeCslXmlNode *node;
    CeCslLocaleForm form;
    CeCslLocaleGender gender_form;
} CeCslLocaleTerm;
typedef struct CeCslLocaleOrdinal {
    CeCslLocaleTerm term;
    /* true: complete long ordinal word; false: suffix appended to the caller's
     * unsigned decimal number. No double conversion or sign parsing occurs. */
    bool is_long;
} CeCslLocaleOrdinal;

/* No file/network/host-locale access. All XML roots and their strings remain
 * borrowed and immutable until free; the catalog/mapping arrays need survive
 * only this call. NULL style is allowed. chosen_language NULL uses the style's
 * default-locale, then en-US. Language tags follow xsd:language (hyphenated,
 * case-insensitive); no host aliases or guessed primary dialects are used.
 *
 * Validates locale headers, namespaces, structural/attribute values and
 * duplicate keys/layers. This is not validation of the rest of a stylesheet
 * or enumeration of the entire evolving CSL term-name vocabulary.
 * Standalone locale resources require version=1.0, xml:lang, style-options,
 * at least one date and nonempty terms, as in the official CSL schema.
 * In-style resources may define partial data. The en-US fallback is used only
 * if supplied in the catalog; missing units remain explicitly missing.
 * NULL allocator means malloc/free; custom callbacks must both be supplied and
 * provide malloc-compatible alignment. OOM never takes ownership of inputs. */
CeCslLocale *ce_csl_locale_new(const CeCslXmlNode *style,
    const CeCslLocaleCatalog *catalog, const char *chosen_language,
    const CeCslLocaleAllocator *allocator, char *error, size_t capacity);
void ce_csl_locale_free(CeCslLocale *locale);
const char *ce_csl_locale_language(const CeCslLocale *locale); /* normalized lower-case tag */

/* Form is resolved across every locale before falling back to a different
 * form. A requested gender-form is tried before the neuter variant. Ordinal
 * suffix terms are restricted to the selected complete ordinal set. */
CeCslLocaleTerm ce_csl_locale_term(const CeCslLocale *locale, const char *name,
    CeCslLocaleForm form, bool plural, CeCslLocaleGender gender_form);
CeCslLocaleGender ce_csl_locale_noun_gender(const CeCslLocale *locale, const char *name);
CeCslLocaleOrdinal ce_csl_locale_ordinal(const CeCslLocale *locale, uint64_t number,
    bool long_form, CeCslLocaleGender gender);
const CeCslXmlNode *ce_csl_locale_date(const CeCslLocale *locale, CeCslLocaleDate form);
/* Returns whether explicitly defined; sets value=false for an absent/default
 * option. Each property falls back independently of the other property. */
bool ce_csl_locale_option(const CeCslLocale *locale, CeCslLocaleOption option, bool *value);

#endif
