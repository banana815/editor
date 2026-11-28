#ifndef CE_RICH_LAYOUT_H
#define CE_RICH_LAYOUT_H
#include "document.h"
#include <SDL3/SDL.h>
#include <SDL3_ttf/SDL_ttf.h>
#include <stdint.h>

typedef struct CeRichLayout CeRichLayout;
typedef struct CeCitationCache CeCitationCache;
struct cJSON;
typedef enum CeRichCitationStatus {
    CE_RICH_CITATION_ABSENT,
    CE_RICH_CITATION_VALID,
    CE_RICH_CITATION_STALE,
    CE_RICH_CITATION_UNSUPPORTED,
    CE_RICH_CITATION_ERROR,
    CE_RICH_CITATION_ARCHIVED
} CeRichCitationStatus;
typedef struct CeRichCitationSource {
    /* Increment whenever resolved content/status changes independently of the
     * document JSON. Revision zero is valid. No callback/context is retained. */
    uint64_t revision;
    void *context;
    CeRichCitationStatus (*resolve)(void *context, const struct cJSON *snapshot_atom,
                                    const CeCitationCache **cache, char *error, size_t capacity);
} CeRichCitationSource;
typedef struct CeRichTheme {
    SDL_Color text, link, highlight, selection, atom_background, atom_border;
} CeRichTheme;
typedef struct CeRichImageSource {
    uint64_t revision;
    void *context;
    /* Return an owned surface, or NULL with a diagnostic. No network access.
     * Decode limits must be enforced before allocating pixels. */
    SDL_Surface *(*resolve)(void *context, const struct cJSON *image,
                            char *error, size_t capacity);
} CeRichImageSource;
/* Copies the descriptor; context remains borrowed until reset/destruction.
 * Changing the descriptor/revision invalidates cached layout. */
void ce_rich_layout_set_images(CeRichLayout *layout, const CeRichImageSource *source);
typedef void (*CeRichRectCallback)(const SDL_FRect *rect, void *userdata);

/* Renderer and base font must outlive the layout. Derived fonts, text objects
 * and cached glyph textures are owned by it; use on the creating SDL thread. */
CeRichLayout *ce_rich_layout_create(SDL_Renderer *renderer, TTF_Font *base_font);
/* Optional fallback must outlive the layout. Derived fonts receive their own
 * resized copy; SDL_ttf does not copy fallback chains with TTF_CopyFont. */
void ce_rich_layout_set_fallback(CeRichLayout *layout, TTF_Font *fallback);
void ce_rich_layout_destroy(CeRichLayout *layout);
CeRichTheme ce_rich_theme_default(bool dark);
/* Transactional: a failed update retains the previous layout. JSON content,
 * width and theme are cached; identical updates do not reshape. No document
 * pointers survive this call. Horizontal LTR flow is supported; bidi controls
 * and RTL script ranges currently return a descriptive error. */
bool ce_rich_layout_update(CeRichLayout *layout, const CeDocument *doc, float width,
                           const CeRichTheme *theme);
/* Controlled citation mode. Resolve only borrows its snapshot node during the
 * call; returned cache must remain immutable through this update. On success
 * no cache, JSON, callback or context pointers survive. VALID requires a cache
 * of the atom's kind. Missing/stale/unsupported content gets an explicit neutral
 * placeholder, never approximate formatting. ERROR or allocation/layout failure
 * retains the prior layout. NULL source behaves as all caches absent.
 * ARCHIVED requires cache=NULL and an eligible unmarked legacy bibliography.
 * It decodes only entriesJson using bibliography_archive, labels the display
 * as an old archive, and never treats old cslRenderJson as processor output.
 * The temporary archive is released after shaping; malformed JSON/allocation
 * failures retain the prior layout. Unsupported HTML uses diagnosed literal
 * text fallback. It does not modify the document or regenerate references.
 * Supports bold/italic/underline/super/subscript, LTR Unicode/emoji and typed
 * bibliography columns, hanging indentation and spacing. Small caps/light/
 * oblique or bidi cache content receives an unsupported-format placeholder.
 * The atom remains one document byte range regardless of its visual lines.
 * A layout is bounded by 1,048,576 citation shaping steps, 131,072 citation
 * fragments, 10,000,000 px height and 128 MiB cached italic texture payload.
 * Narrow columns wrap at shaped cluster boundaries; one indivisible glyph may
 * exceed an extremely narrow available width. Margin labels reserve an inner
 * gutter inside the atom, so scrolling/selection includes their full bounds. */
bool ce_rich_layout_update_citations(CeRichLayout *layout, const CeDocument *doc, float width,
                                     const CeRichTheme *theme, const CeRichCitationSource *source);
const char *ce_rich_layout_error(const CeRichLayout *layout);
float ce_rich_layout_height(const CeRichLayout *layout);
size_t ce_rich_layout_generation(const CeRichLayout *layout);

/* All geometry APIs use layout-local coordinates (origin 0,0), including
 * selection callbacks. Offsets index ce_document_text() in UTF-8 bytes.
 * Interior cluster offsets snap to cluster start. Atoms are indivisible. */
bool ce_rich_layout_caret(const CeRichLayout *layout, size_t offset, SDL_FRect *rect);
/* trailing chooses the preceding visual line at a soft-wrap boundary. */
bool ce_rich_layout_caret_affinity(const CeRichLayout *layout, size_t offset,
                                  bool trailing, SDL_FRect *rect);
size_t ce_rich_layout_step(const CeRichLayout *layout, size_t offset, int direction);
size_t ce_rich_layout_hit_test(const CeRichLayout *layout, float x, float y);
void ce_rich_layout_selection_rects(const CeRichLayout *layout, size_t start, size_t end,
                                   CeRichRectCallback callback, void *userdata);
/* x,y translate layout coordinates into renderer coordinates. viewport is in
 * renderer coordinates; pass NULL to use the existing renderer clip. Draws
 * selection and rich text, but no blinking caret or IME preedit overlay. */
bool ce_rich_layout_draw(CeRichLayout *layout, float x, float y, const SDL_FRect *viewport,
                         size_t selection_start, size_t selection_end);
#endif
