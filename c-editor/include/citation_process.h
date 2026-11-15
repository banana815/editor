#ifndef CE_CITATION_PROCESS_H
#define CE_CITATION_PROCESS_H

#include "citation_cache.h"
#include "citation_styles.h"
#include "document.h"
#include "zotero.h"

typedef struct CeCitationPrepared CeCitationPrepared;
typedef struct CeCitationProcessOptions {
    /* Explicit style selection may persist settings even in an empty document.
     * Otherwise a document with no citations/sections remains byte-equivalent,
     * and no bibliography or profile attributes are created merely by opening it. */
    bool persist_profile;
} CeCitationProcessOptions;
typedef struct CeCitationProcessStats {
    size_t citation_count, reference_count, repaired_ids, removed_sections;
    bool missing_term, literal_markup, malformed_markup;
    size_t selected_count; /* Unique explicitly included identities, cited or not. */
    bool selected_bibliography; /* Explicit selected intent, including an empty set. */
} CeCitationProcessStats;

/* Reads an immutable revision and constructs a complete private candidate.
 * It preserves source/unknown metadata, resolves complete CSL snapshots,
 * detects conflicting metadata for one identity, evaluates the C processor and
 * produces controlled citation/bibliography caches. The caller commits exactly
 * once with ce_document_apply_json(prepared_json), after all fallible work.
 * No input document, library, style or old result is modified. Initialize *out
 * NULL/live; failure preserves it. The owned result borrows none of the inputs.
 *
 * Embedded cslJson is authoritative. Only an absent snapshot may be filled from
 * an exact unique match in fallback_library; incomplete display author/year
 * strings are never parsed into invented CSL data. User note stays separate
 * from the CSL locator and note index. No network/file access or approximate
 * fallback occurs. The current app profile has one item per citation atom.
 * Rebuilt bibliography sections declare attrs.bibliographyMode="citations" or
 * "selected". Selected sections carry selectedItemsJson, a complete ordered
 * source-attribute array; unique sources are explicitly included even without
 * a citation atom. Removing a citation cannot remove an explicitly selected
 * item. Empty selected intent retains an empty canonical bibliography. Both
 * managed modes permit removal of obsolete ordinary generated entries.
 * An unmatched legacy entry, unsupported mode, ambiguous cross-section identity,
 * or metadata that cannot be preserved rejects the entire preparation. Existing
 * render caches never establish this intent; initial loads still rebuild them.
 * A citation-bearing source and final JSON are limited to 16 MiB/250,000 JSON
 * values. Generated cache/compatibility strings have a combined 16 MiB budget
 * including their outer JSON escaping, checked before insertion; the temporary
 * legacy HTML aggregate is also bounded to 16 MiB. Ordinary citation-free
 * documents use a copy-only fast path unless explicit settings are requested.
 * Processor workspace and each cache's owned workspace retain their separate
 * limits; this is NOT one allocator budget covering the entire app/JSON parser.
 * Allocations use malloc/free and the dependencies' documented allocators.
 * Free only through the API. */
bool ce_citations_prepare(const CeDocument *document, const CeCitationStyle *style,
                          const CeZoteroLibrary *fallback_library, const CeCitationProcessOptions *options,
                          CeCitationPrepared **out, char *error, size_t capacity);
void ce_citation_prepared_free(CeCitationPrepared *prepared);
const char *ce_citation_prepared_json(const CeCitationPrepared *prepared);
const char *ce_citation_prepared_digest(const CeCitationPrepared *prepared);
const CeCitationProcessStats *ce_citation_prepared_stats(const CeCitationPrepared *prepared);
const CeCitationCache *ce_citation_prepared_citation(const CeCitationPrepared *prepared,
                                                     const char *citation_id);
const CeCitationCache *ce_citation_prepared_bibliography(const CeCitationPrepared *prepared);

/* Same source-envelope preparation and digest as prepare, without evaluating
 * citations, modifying the document or parsing existing render caches. Missing
 * snapshots/identity conflicts remain explicit failures. Success writes exactly
 * 65 bytes (lowercase SHA-256 plus NUL); failure preserves all output bytes.
 * Digest includes the complete ordered citation revision, canonical explicit
 * selections/intent and style/locale pins; ordinary prose and citation quotation
 * notes are excluded. Selected-source unknown attrs participate in the digest.
 * It is a cache key, not
 * an authenticity proof. Initial imports must still be processed afresh. */
bool ce_citations_source_digest(const CeDocument *document, const CeCitationStyle *style,
                                const CeZoteroLibrary *fallback_library, char digest[65], char *error,
                                size_t capacity);

/* Shared canonical identity for citation and selected-source attributes.
 * Pure read-only helper; initialize *owned_key to NULL/live malloc storage.
 * Success replaces it; error preserves it. URI takes precedence, followed by
 * scoped source ID/key, cite key, then citation ID for legacy citation atoms.
 * This does not validate the item's CSL metadata or grant source authority. */
bool ce_citations_source_identity(const cJSON *source_attrs,char **owned_key,
                                  char *error,size_t capacity);

#endif
