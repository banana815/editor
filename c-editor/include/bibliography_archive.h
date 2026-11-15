#ifndef CE_BIBLIOGRAPHY_ARCHIVE_H
#define CE_BIBLIOGRAPHY_ARCHIVE_H
#include "csl_bibliography.h"
struct cJSON;

#define CE_BIBLIOGRAPHY_ARCHIVE_MAX_BYTES ((size_t)16 * 1024 * 1024)
#define CE_BIBLIOGRAPHY_ARCHIVE_MAX_ENTRIES ((size_t)10000)
#define CE_BIBLIOGRAPHY_ARCHIVE_MAX_ROWS ((size_t)65536)
#define CE_BIBLIOGRAPHY_ARCHIVE_MAX_RUNS ((size_t)131072)
#define CE_BIBLIOGRAPHY_ARCHIVE_MAX_WORKSPACE ((size_t)128 * 1024 * 1024)
enum CeBibliographyArchiveDiagnostic {
    CE_BIBLIOGRAPHY_ARCHIVE_FALLBACK_TEXT = 1u << 0,
    CE_BIBLIOGRAPHY_ARCHIVE_INVALID_METADATA = 1u << 1
};
typedef struct CeBibliographyArchiveRow {
    CeCslBibliographyRowKind kind;
    const CeCslOutput *left; /* Only present for TWO_COLUMNS. */
    const CeCslOutput *body;
} CeBibliographyArchiveRow;
typedef struct CeBibliographyArchiveEntry {
    size_t source_index;
    const char *citation_id, *key, *item_uri, *pdf_uri;
    const CeCslOutput *label; /* Stored number, never a newly assigned number. */
    unsigned diagnostics;
    size_t row_count;
    const CeBibliographyArchiveRow *rows;
} CeBibliographyArchiveEntry;
typedef struct CeBibliographyArchive CeBibliographyArchive;

/* Shape-only check, with no allocations. Only a zoteroEndnoteSection whose
 * attrs lack bibliographyMode and contain a string entriesJson is eligible.
 * Missing mode is distinct from null, empty and every known/unknown mode.
 * cslRenderJson is never consulted. */
bool ce_bibliography_archive_eligible(const struct cJSON *snapshot_atom);
/* Read-only archived display, NOT processor output or an authenticated cache.
 * Strictly parses entriesJson. Unknown source metadata is never changed.
 * Owns copied metadata, controlled rows and runs; borrows inputs only in this
 * call. Initialize *out to NULL/live; all failures leave it unchanged.
 * The allocator context must outlive the archive. Workspace includes this
 * module's transient/retained allocations, not the independent bounded cJSON
 * tree or prior result. Each HTML/text field is bounded by 1 MiB, nesting 64.
 * Supported HTML: b/strong/i/em/u/sup/sub, br, a with validated HTTP(S) href,
 * span with a small formatting allowlist, and CSL div wrappers/display rows.
 * Entities decode once. Unknown/malformed tags, attributes or entities fall
 * back to exact entryText (if present), else exact entryHtml as literal text;
 * diagnostics distinguish this from faithful controlled rendering. No scripts,
 * CSS engine, external resources, document mutation or network I/O exist here.
 * Invalid typed fields are diagnosed; malformed entries JSON fails. Numeric
 * labels are only displayed when the JSON parser proves its numeric tokens
 * exact safe integers; otherwise no approximate number is invented. */
bool ce_bibliography_archive_decode(const struct cJSON *snapshot_atom,
                                    const CeCslOutputAllocator *allocator,
                                    CeBibliographyArchive **out, char *error, size_t capacity);
void ce_bibliography_archive_free(CeBibliographyArchive *archive);
size_t ce_bibliography_archive_count(const CeBibliographyArchive *archive);
unsigned ce_bibliography_archive_diagnostics(const CeBibliographyArchive *archive);
const CeBibliographyArchiveEntry *ce_bibliography_archive_entry(const CeBibliographyArchive *archive,
                                                               size_t index);
/* Owned plain display, with stored labels and TAB-separated HTML columns.
 * Rows/entries are newline-separated. No archive caption/provenance is added.
 * The returned string owns independent default malloc storage and can outlive
 * the archive. Initialize to {0}; failure preserves the prior string. */
bool ce_bibliography_archive_plain(const CeBibliographyArchive *archive, CeCslOutputString *out,
                                   char *error, size_t capacity);
#endif
