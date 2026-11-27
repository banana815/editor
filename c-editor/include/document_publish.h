#ifndef CE_DOCUMENT_PUBLISH_H
#define CE_DOCUMENT_PUBLISH_H
#include "document.h"
struct cJSON;
typedef enum { CE_PUBLISH_MARKDOWN, CE_PUBLISH_HTML } CePublishFormat;
/* Atom HTML must come from newly evaluated authoritative sources, never from
 * imported render-cache attributes. Return malloc-owned safe HTML or fail.
 * The serializer itself never interprets or trusts citation render caches. */
typedef char *(*CePublishAtomHtml)(void *context, const struct cJSON *atom,
                                 char *error, size_t capacity);
/* Read-only publication, not a lossless archive: source metadata is omitted.
 * Unknown nodes/marks, unsafe URLs/CSS and invalid structures fail explicitly.
 * Markdown uses inline HTML for marks without a faithful Markdown equivalent,
 * aligned blocks and evaluated citations. HTML is a pasteable body fragment.
 * Input/output each bounded to 16 MiB, nesting to 64. Result uses free(). */
char *ce_document_publish(const CeDocument *document, CePublishFormat format,
                          CePublishAtomHtml atom_html, void *context,
                          char *error, size_t capacity);
#endif
