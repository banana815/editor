#ifndef CE_CITATION_SESSION_H
#define CE_CITATION_SESSION_H
#include "citation_process.h"

typedef struct CeCitationSession CeCitationSession;

/* A session owns the parsed style and freshly generated display data. Imported
 * render caches are never trusted. No input document/library pointers escape.
 * Build preserves an old *out on failure; callers changing documents must
 * discard that old session if the new revision cannot be processed. */
bool ce_citation_session_build(const CeDocument *document,
    const CeZoteroLibrary *library, const CeCitationProfile *override_profile,
    CeCitationSession **out, char *error, size_t capacity);
void ce_citation_session_free(CeCitationSession *session);
CeCitationProfile ce_citation_session_profile(const CeCitationSession *session);
const CeCitationPrepared *ce_citation_session_prepared(const CeCitationSession *session);

/* Commit a privately edited document and any regenerated citation results as
 * one live undo transaction. On any failure live/session are unchanged.
 * override_profile persists an explicit style selection, including empty notes.
 * force_processing is used for citation insert/update/sync. Ordinary edits may
 * reuse freshly generated output when the complete source digest is unchanged.
 * An unprocessable legacy note may still receive ordinary edits only when its
 * entire citation/section nodes and root settings remain unchanged. */
bool ce_citation_session_apply(CeCitationSession **session, CeDocument *live,
    const CeDocument *candidate, const CeZoteroLibrary *library,
    const CeCitationProfile *override_profile, bool force_processing,
    char *error, size_t capacity);

/* Produce a fresh complete JSON snapshot for save/export, with current prose
 * and derived citation data. The returned prepared object owns its contents. */
bool ce_citation_session_snapshot(const CeCitationSession *session,
    const CeDocument *document, const CeZoteroLibrary *library,
    CeCitationPrepared **out, char *error, size_t capacity);
#endif
