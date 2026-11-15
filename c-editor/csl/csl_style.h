#ifndef CE_CSL_STYLE_H
#define CE_CSL_STYLE_H

#include "csl_xml.h"

#define CE_CSL_STYLE_MAX_MACROS ((size_t)4096)
#define CE_CSL_STYLE_MAX_REFERENCES ((size_t)32768)
#define CE_CSL_STYLE_MAX_INLINE_LOCALES ((size_t)256)

typedef struct CeCslStyle CeCslStyle;
typedef struct CeCslStyleAllocator {
    void *context;
    void *(*allocate)(void *context, size_t size);
    void (*deallocate)(void *context, void *memory);
} CeCslStyleAllocator;

/* Load an independent CSL 1.0/1.0.2 style. Both standards use version="1.0".
 * Initialize *style to NULL. Success replaces/frees the previous style; any
 * failure leaves it and its borrowed views unchanged. XML, indexes and all
 * strings are owned independently of the input bytes. No network/file access
 * or dependent-style resolution occurs.
 *
 * This is a bounded structural/attribute/enum and macro-graph validator, NOT a
 * renderer or a complete RELAX NG/XSD validator. Acceptance does not establish
 * rendering support or CSL conformance. It checks known element contexts,
 * attributes, common scalar values and cardinalities, including all branches
 * and unused macros. It does not validate metadata URI/dateTime lexical forms
 * or every cross-attribute semantic rule. Inline locales are retained; locale
 * fallback, term resolution and collation belong to other layers. Significant
 * affix/delimiter/term/metadata text is never trimmed or normalized here. */
bool ce_csl_style_load(CeCslStyle **style, const char *xml, size_t size,
                       char *error, size_t capacity);

/* Optional per-object hooks cover the style object, indexes and graph work
 * buffers. The XML AST continues to use the XML layer's own allocator. Both
 * callbacks are required, allocations must meet malloc's alignment contract,
 * and context must remain valid until the style is freed. NULL uses malloc/free.
 * Allocation-failure testing through these hooks covers this layer only. */
bool ce_csl_style_load_with_allocator(CeCslStyle **style, const char *xml, size_t size,
                                      const CeCslStyleAllocator *allocator,
                                      char *error, size_t capacity);
void ce_csl_style_free(CeCslStyle *style);

/* All returned nodes/strings are borrowed and must not be modified. Their
 * lifetime ends at free or the next successful replacement of this style.
 * Missing optional values and invalid/out-of-range queries return NULL. Empty
 * required metadata remains the empty string, as permitted by CSL's schema. */
const CeCslXmlNode *ce_csl_style_root(const CeCslStyle *style);
const CeCslXmlNode *ce_csl_style_citation(const CeCslStyle *style);
const CeCslXmlNode *ce_csl_style_bibliography(const CeCslStyle *style);
const CeCslXmlNode *ce_csl_style_macro(const CeCslStyle *style, const char *name);
size_t ce_csl_style_macro_count(const CeCslStyle *style);
const char *ce_csl_style_id(const CeCslStyle *style);
const char *ce_csl_style_title(const CeCslStyle *style);
const char *ce_csl_style_default_locale(const CeCslStyle *style);
size_t ce_csl_style_inline_locale_count(const CeCslStyle *style);
const CeCslXmlNode *ce_csl_style_inline_locale(const CeCslStyle *style, size_t index);

#endif
