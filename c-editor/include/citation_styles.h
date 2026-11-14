#ifndef CE_CITATION_STYLES_H
#define CE_CITATION_STYLES_H

#include "csl_style.h"
#include "csl_locale.h"
#include "csl_output.h"

typedef enum CeCitationProfile {
    CE_CITATION_PROFILE_APA,
    CE_CITATION_PROFILE_IEEE,
    CE_CITATION_PROFILE_GBT
} CeCitationProfile;
typedef struct CeCitationStyle CeCitationStyle;
typedef struct CeCitationStyleInfo {
    CeCitationProfile profile;
    const char *profile_id; /* Application identifier, independent of display text. */
    const char *style_id;   /* Original CSL <id>. */
    const char *title;      /* Includes the pinned bibliographic profile edition. */
    const char *csl_version; /* Schema family, not an APA/IEEE/GB edition. */
    const char *updated;
    const char *style_commit;
    const char *style_sha256;
    const char *locale_commit;
} CeCitationStyleInfo;

/* The five unchanged, hash-verified CSL resources are compiled as byte arrays.
 * No file, network, host-locale or scripting-runtime access occurs here.
 * This owns one parsed style, both locale XML trees and the resolved locale.
 * Loading does not promise that every possible item/style construct is supported
 * by the processor; unsupported rendering still fails explicitly downstream.
 *
 * chosen_language NULL uses the style default, then CSL's en-US default.
 * Explicit overrides must name an embedded dialect (en-US or zh-CN, ASCII
 * case-insensitive); other dialects and bare language tags are not guessed.
 * Inputs survive only the call. Initialize *out to NULL/live: every failure
 * preserves it and its views, and success replaces/frees the prior object.
 *
 * NULL allocator selects malloc/free. Both supplied callbacks are required,
 * with malloc-compatible alignment and context lifetime through free. Hooks
 * cover this wrapper and style/locale indexes; the XML AST/parser retain their
 * existing independent allocators. No global allocator configuration is used. */
bool ce_citation_style_load(CeCitationProfile profile, const char *chosen_language,
    const CeCslOutputAllocator *allocator, CeCitationStyle **out,
    char *error, size_t capacity);
void ce_citation_style_free(CeCitationStyle *style);

/* Static metadata is valid for the process lifetime; invalid enum returns NULL.
 * The loaded style/locale views survive only until free/successful replacement.
 * Keep this owner alive while any CeCslDocument borrows these views. */
const CeCitationStyleInfo *ce_citation_style_info(CeCitationProfile profile);
const CeCitationStyleInfo *ce_citation_style_loaded_info(const CeCitationStyle *style);
const CeCslStyle *ce_citation_style_style(const CeCitationStyle *style);
const CeCslLocale *ce_citation_style_locale(const CeCitationStyle *style);

#endif
