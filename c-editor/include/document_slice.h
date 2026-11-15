#ifndef CE_DOCUMENT_SLICE_H
#define CE_DOCUMENT_SLICE_H

#include "document.h"

#define CE_DOCUMENT_SLICE_MAX_BYTES ((size_t)16 * 1024 * 1024)
#define CE_DOCUMENT_SLICE_MAX_NODES ((size_t)250000)
#define CE_DOCUMENT_SLICE_MAX_DEPTH ((size_t)128)
#define CE_DOCUMENT_SLICE_MIME "application/x-notelab-document-slice+json"

typedef struct CeDocumentSlice CeDocumentSlice;
typedef struct CeDocumentCutGuard CeDocumentCutGuard;
struct cJSON;

/* Candidate-local position, before the citation session regenerates/moves its
 * bibliography. No node pointers escape. The caller must resolve its final
 * position after any later document transformation. */
typedef struct CeDocumentSliceAnchor {
    size_t offset, block_index, block_offset;
} CeDocumentSliceAnchor;

/* Optional readable atom text provider. The snapshot is borrowed only during
 * this call. Success returns malloc-owned, NUL-terminated UTF-8 through text;
 * the slice exporter frees it. NULL text requests the built-in metadata label.
 * A provider can use the current, independently validated citation session;
 * imported render caches are never parsed or trusted by this module. */
typedef bool (*CeDocumentSliceAtomText)(void *context, const struct cJSON *snapshot,
    char **text, char *error, size_t capacity);

/* Owned snapshots: no document/source/library pointers survive capture.
 * Offsets are ce_document_text() UTF-8 byte offsets. Empty selections are valid.
 * Marks, attributes, hard breaks, complete atoms and list ancestry are retained.
 * Partial selection through an unknown structural wrapper is rejected; a whole
 * unknown subtree is copied without interpreting its attributes.
 *
 * Initialize outputs NULL/live. Success replaces them; failure preserves BOTH
 * outputs. guard may be NULL for Copy. A Cut guard owns the full canonical
 * source JSON and range; the application must also check its active-note token.
 * All source/slice JSON and exported representations are limited to 16 MiB,
 * 250,000 JSON values and 128 nested containers. These are input/structure
 * limits, not a claimed shared heap limit across cJSON and CeDocument. */
bool ce_document_slice_capture(const CeDocument *document, size_t start, size_t end,
    CeDocumentSlice **slice, CeDocumentCutGuard **guard, char *error, size_t capacity);
void ce_document_slice_free(CeDocumentSlice *slice);
void ce_document_cut_guard_free(CeDocumentCutGuard *guard);
bool ce_document_cut_guard_matches(const CeDocumentCutGuard *guard,
    const CeDocument *document, size_t start, size_t end, bool *matches,
    char *error, size_t capacity);

/* Strict version-1 private format: {type:"notelab-slice",version:1,
 * openStart:N,openEnd:N,content:[TipTap nodes]}. Open depths describe clipped
 * paragraph/list boundaries, never arbitrary HTML. Unknown envelope fields,
 * duplicate JSON keys, invalid UTF-8/NUL and malformed open paths are rejected.
 * Numeric attributes follow the existing document's finite-double contract;
 * exact printing preserves values already representable in that model. */
bool ce_document_slice_parse(const char *data, size_t size, CeDocumentSlice **slice,
    char *error, size_t capacity);
bool ce_document_slice_json(const CeDocumentSlice *slice, char **text,
    char *error, size_t capacity);
bool ce_document_slice_plain(const CeDocumentSlice *slice,
    CeDocumentSliceAtomText atom_text, void *context, char **text,
    char *error, size_t capacity);
/* Human-readable, escaped HTML with controlled tags/styles only. A private
 * data-notelab-slice attribute carries the complete versioned slice; the body
 * is a readable fallback, not an assertion that every unknown mark is rendered.
 * Raw entryHtml and callback text are never executed as HTML. All exported
 * strings use ordinary malloc/free and may outlive the originating slice. */
bool ce_document_slice_html(const CeDocumentSlice *slice,
    CeDocumentSliceAtomText atom_text, void *context, char **text,
    char *error, size_t capacity);
/* Recover only our single, quoted data-notelab-slice envelope from HTML.
 * Entity decoding is bounded and performed once. The visible HTML body is
 * never interpreted as document data. No envelope: success, found=false, old
 * slice unchanged. Malformed/duplicate/multiple envelopes: failure preserves
 * BOTH outputs. Generic HTML import remains a separate explicit caller step. */
bool ce_document_slice_from_html(const char *data, size_t size,
    CeDocumentSlice **slice, bool *found, char *error, size_t capacity);

/* Build a private replacement document, without touching document/history or
 * savepoints. NULL slice means deletion. Paste joins compatible open paragraph
 * and list boundaries while preserving closed blocks and unknown atoms.
 * Deletion across incompatible list ancestry/custom metadata is explicitly
 * unsupported, rather than flattening nodes or silently retaining a separator.
 * Only newly inserted conflicting/missing citation IDs are repaired; original
 * citation IDs and source attributes are never rewritten. Conflicting complete
 * CSL snapshots for one known source identity reject the operation. The caller
 * still runs the citation session's authoritative validation before committing.
 * Standard automatically generated bibliography sections in the incoming slice
 * are omitted only when all entries are covered by its copied source citations;
 * the citation session rebuilds one global bibliography. Custom section/entry
 * metadata rejects this conversion instead of being discarded.
 * A bibliography without its cited source nodes cannot be pasted as a derived
 * bibliography: this returns an explicit error; readable exports remain usable.
 * Explicit bibliographyMode="selected" sections instead carry complete
 * selectedItemsJson sources and may be pasted alone. Their source/cache/unknown
 * attributes are preserved without trusting rendered caches or generating fake
 * citation atoms. The citation session must merge selected identities, reject
 * conflicts and rebuild controlled output before committing the candidate.
 *
 * Initialize *candidate NULL/live. On failure candidate AND optional anchor
 * remain unchanged. The candidate is owned and has no prior history. Commit
 * only with ce_document_apply_json or the application's citation transaction,
 * after the platform accepts clipboard data and a Cut guard still matches. */
bool ce_document_slice_prepare_replace(const CeDocument *document,
    size_t start, size_t end, const CeDocumentSlice *slice,
    CeDocument **candidate, CeDocumentSliceAnchor *anchor,
    char *error, size_t capacity);

#endif
