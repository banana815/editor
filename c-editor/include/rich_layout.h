#ifndef CE_RICH_LAYOUT_H
#define CE_RICH_LAYOUT_H
#include "document.h"
#include <SDL3/SDL.h>
#include <SDL3_ttf/SDL_ttf.h>

typedef struct CeRichLayout CeRichLayout;
typedef struct CeRichTheme {
    SDL_Color text, link, highlight, selection, atom_background, atom_border;
} CeRichTheme;
typedef void (*CeRichRectCallback)(const SDL_FRect *rect, void *userdata);

/* Renderer and base font must outlive the layout. Derived fonts and all text
 * objects are owned by the layout; use on the creating SDL thread. */
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
