#ifndef CE_CITATIONS_H
#define CE_CITATIONS_H
#include "document.h"
#include "zotero.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct CeCitationSyncResult {
    size_t citation_count;
    size_t reference_count;
    size_t repaired_ids;
    size_t removed_sections;
} CeCitationSyncResult;

/* Every successful mutation is one undo transaction and preserves the existing
 * savepoint/history. On failure the document and output arguments are unchanged.
 * new_id is optional; on success its owned UTF-8 result must be freed with free().
 * Insert only adds the citation; bibliography synchronization is explicit.
 * Nonempty item.id is retained as optional sourceId; positive library_id is
 * retained as decimal-string libraryId (no loss of 64-bit integer precision).
 * Nonempty item.csl_json is retained verbatim as optional cslJson for later
 * full CSL formatting; this does not change approximate synchronization.
 * Unknown/absent source metadata is not synthesized. */
bool ce_citation_insert(CeDocument *doc, size_t offset, const CeZoteroItem *item,
                         const char *page, const char *note, char **new_id,
                         char *error, size_t error_capacity);
/* NULL page/note retains that field. A missing or ambiguous citationId fails
 * instead of selecting an arbitrary block. Unknown attributes are retained. */
bool ce_citation_update(CeDocument *doc, const char *citation_id,
                         const char *page, const char *note,
                         char *error, size_t error_capacity);
/* Explicit approximate formatting, NOT a CSL processor. Each entry stores
 * formatting:"approximate" inside the legacy entriesJson string. URI identity
 * takes precedence; without URI use sourceId (scoped by libraryId if known),
 * libraryId+itemKey, itemKey,
 * citeKey, then the unique citationId. Different URI-bearing items never merge
 * because their local itemKey happens to match.
 * Identical source IDs without any library/source namespace cannot distinguish
 * separate imported libraries; callers must retain available source metadata.
 * Repairs absent/duplicate IDs, numbers references by first document occurrence,
 * and replaces the bibliography as one undoable edit. Existing first-section
 * metadata and matching entry metadata are retained. Extra sections containing
 * nonstandard metadata cause an error rather than data loss. With no citations,
 * explicit sync removes old bibliography sections (undo restores them). */
bool ce_citations_sync_approximate(CeDocument *doc, CeZoteroApproximateStyle style,
                                   CeCitationSyncResult *result,
                                   char *error, size_t error_capacity);
#ifdef __cplusplus
}
#endif
#endif
