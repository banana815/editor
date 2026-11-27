#ifndef CE_DOCUMENT_SEARCH_H
#define CE_DOCUMENT_SEARCH_H
#include "document.h"

/* Literal single-line text search: ASCII letters are case-insensitive; other
 * UTF-8 is exact. Matches never cross paragraph boundaries or atomic blocks.
 * Empty/invalid UTF-8 queries and U+FFFC placeholders are not searchable.
 * Returned offsets use the document's UTF-8 byte coordinates. */
bool ce_document_search_query_valid(const char *query);
bool ce_document_search_next(const CeDocument *doc, const char *query, size_t from,
                             bool wrap, size_t *start, size_t *end);
/* Replace the first match at/after from (wrapping), or all non-overlapping
 * matches in the ORIGINAL document. A replacement is literal single-line
 * UTF-8 (empty means deletion); inserted text is never searched again.
 * The first matched text node supplies the replacement's marks. Unmatched
 * text, all block/wrapper attributes and unknown metadata are retained.
 * The operation commits once through apply_json: allocation/parse/validation
 * failure preserves text, savepoint and history; success is one undo step.
 * No matches is successful with count=0 and no history change. Outputs are
 * written only on success. cursor is the end of the last replacement. */
bool ce_document_search_replace(CeDocument *doc, const char *query,
    const char *replacement, size_t from, bool all, size_t *count, size_t *cursor,
    char *error, size_t capacity);

/* Application-owned article-search result; populated only from isolated note
 * reads or the currently open document, not citation render caches. */
typedef struct SearchHit {
    char *name;
    char *snippet;
    size_t start, end;
} SearchHit;
#endif
