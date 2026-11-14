#ifndef CE_CSL_FORMAT_H
#define CE_CSL_FORMAT_H
#include "csl_output.h"
#include "csl_xml.h"

/* Resolve only CSL's five font/decoration/vertical properties. Missing attrs
 * inherit; explicit normal/none/baseline clears the corresponding property.
 * Other attributes (case, quotes, display, affixes) belong to separate layers.
 * Oblique and light have distinct IR flags and are never approximated as normal.
 * A NULL node simply validates/copies inherited. Failure leaves *resolved intact. */
bool ce_csl_format_resolve(const CeCslXmlNode *node, unsigned inherited,
    unsigned *resolved, char *error, size_t capacity);

/* Append one element's prefix + content + suffix atomically. No affixes are
 * emitted for empty content. The caller supplies the affix's effective format:
 * ordinarily the parent's format, but the layout's format for cs:layout.
 * This deliberately does not apply text-case, strip periods or quotes to affixes.
 * content may equal destination; both remain unchanged on any failure. */
bool ce_csl_format_affixes(const CeCslXmlNode *node, const CeCslOutput *content,
    unsigned affix_format, const CeCslOutputAllocator *allocator,
    CeCslOutput *destination, char *error, size_t capacity);
#endif
