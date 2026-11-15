#ifndef CE_BIBLIOGRAPHY_SELECTION_H
#define CE_BIBLIOGRAPHY_SELECTION_H
#include "document.h"
#include "zotero.h"

/* Replace the explicit bibliography selection on a private candidate document.
 * Each item must have a complete CSL snapshot and an unambiguous source key.
 * The section stores bibliographyMode="selected" and selectedItemsJson, an
 * array of complete source-attribute objects. It creates no citation atoms,
 * fake citation IDs, rendered strings or approximate bibliography entries.
 * Existing body citations and unknown document/section metadata are retained.
 * Unknown source fields survive for identities retained in the selection.
 * Unmanaged legacy entries are never silently converted/deleted: if safe
 * ownership cannot be established the operation fails without mutation.
 *
 * count=0 clears explicit items but retains the explicit selected intent. The
 * caller must then use ce_citation_session_apply(..., force_processing=true)
 * to process and commit its candidate as one live undo transaction. Success
 * applies one candidate transaction; any error/OOM preserves candidate content
 * and history. Input items are borrowed only during this call. Maximum 50000
 * items, 16 MiB strict JSON source/final document; no I/O or global state. */
bool ce_bibliography_select(CeDocument *candidate,const CeZoteroItem *const *items,size_t count,
                            char *error,size_t capacity);
#endif
