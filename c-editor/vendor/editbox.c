/*
  Copyright (C) 1997-2025 Sam Lantinga <slouken@libsdl.org>

  This software is provided 'as-is', without any express or implied
  warranty.  In no event will the authors be held liable for any damages
  arising from the use of this software.

  Permission is granted to anyone to use this software for any purpose,
  including commercial applications, and to alter it and redistribute it
  freely.
*/
#include "editbox.h"
#include <limits.h>

/* Adapted for Note Lab from SDL_ttf release-3.2.2. Local changes: transactional
 * UTF-8 editing, IME snapshot/cancel/commit, bounds-checked selection and cursor,
 * clipboard lifetime/failure handling, and cursor colors for dark themes. */

#define CURSOR_BLINK_INTERVAL_MS    500

static void ClearCandidates(EditBox *edit);
static bool HasLayout(EditBox *edit)
{
    return edit->layout && edit->layout->prepare && edit->layout->prepare(edit->layout_userdata, edit);
}
void EditBox_SetLayout(EditBox *edit, const EditBoxLayout *layout, void *userdata)
{
    if (!edit) return;
    edit->layout = layout; edit->layout_userdata = userdata; edit->cursor_trailing = false;
}
bool EditBox_CaretRect(EditBox *edit, int offset, SDL_FRect *rect)
{
    if (!edit || !rect) return false;
    if (HasLayout(edit)) return edit->layout->caret(edit->layout_userdata, offset, edit->cursor_trailing, rect);
    TTF_SubString sub;
    if (!TTF_GetTextSubString(edit->text, offset, &sub)) return false;
    SDL_RectToFRect(&sub.rect, rect);
    if ((sub.flags & TTF_SUBSTRING_DIRECTION_MASK) == TTF_DIRECTION_RTL) rect->x += rect->w;
    rect->w = 1;
    return true;
}
typedef struct { EditBox *edit; float thickness; } CompositionUnderline;
static void UnderlineCompositionRect(const SDL_FRect *r, void *userdata)
{
    CompositionUnderline *context = userdata;
    SDL_FRect line = {r->x + context->edit->rect.x, r->y + context->edit->rect.y + r->h - 2,
                      r->w, context->thickness};
    SDL_RenderFillRect(context->edit->renderer, &line);
}

static void DrawText(EditBox *edit, TTF_Text *text, float x, float y)
{
#ifdef TEST_SURFACE_ENGINE
    if (edit->window_surface) {
        /* Flush the renderer so we can draw directly to the window surface */
        SDL_FlushRenderer(edit->renderer);
        TTF_DrawSurfaceText(text, (int)SDL_roundf(x), (int)SDL_roundf(y), edit->window_surface);
        return;
    }
#endif /* TEST_SURFACE_ENGINE */

    TTF_DrawRendererText(text, x, y);
}

static int ClampPosition(const char *text, int position)
{
    size_t length = SDL_strlen(text ? text : "");
    if (length > INT_MAX) length = INT_MAX;
    position = SDL_clamp(position, 0, (int)length);
    while (position > 0 && ((unsigned char)text[position] & 0xc0) == 0x80) --position;
    return position;
}

static void NormalizeBounds(EditBox *edit)
{
    const char *text = edit->text->text ? edit->text->text : "";
    edit->cursor = ClampPosition(text, edit->cursor);
    if ((edit->highlight1 >= 0 && ClampPosition(text, edit->highlight1) != edit->highlight1) ||
        (edit->highlight2 >= 0 && ClampPosition(text, edit->highlight2) != edit->highlight2)) {
        edit->highlight1 = edit->highlight2 = -1;
    }
}

static bool ValidText(const char *text)
{
    const unsigned char *p = (const unsigned char *)text;
    if (!text || SDL_strlen(text) > INT_MAX) return SDL_SetError("Text is too large");
    while (*p) {
        unsigned code, minimum, count;
        if (*p < 0x80) { ++p; continue; }
        if (*p >= 0xc2 && *p <= 0xdf) { code = *p & 31; minimum = 0x80; count = 1; }
        else if (*p >= 0xe0 && *p <= 0xef) { code = *p & 15; minimum = 0x800; count = 2; }
        else if (*p >= 0xf0 && *p <= 0xf4) { code = *p & 7; minimum = 0x10000; count = 3; }
        else return SDL_SetError("Invalid UTF-8 input");
        ++p;
        while (count--) {
            if ((*p & 0xc0) != 0x80) return SDL_SetError("Invalid UTF-8 input");
            code = (code << 6) | (*p++ & 63);
        }
        if (code < minimum || code > 0x10ffff || (code >= 0xd800 && code <= 0xdfff)) return SDL_SetError("Invalid UTF-8 input");
    }
    return true;
}

/* Build the complete replacement first. No failed allocation/TTF operation may
 * leave a selection deleted or a preedit half-committed. */
static bool ReplaceRange(EditBox *edit, const char *base, int start, int end, const char *insert)
{
    size_t length, added, total;
    char *replacement;
    bool result;
    if (!ValidText(insert)) return false;
    length = SDL_strlen(base);
    added = SDL_strlen(insert);
    if (start < 0 || end < start || (size_t)end > length ||
        ClampPosition(base, start) != start || ClampPosition(base, end) != end)
        return SDL_SetError("Invalid UTF-8 selection");
    total = length - (size_t)(end - start);
    if (total > INT_MAX || added > (size_t)INT_MAX - total) return SDL_SetError("Text is too large");
    total += added;
    replacement = SDL_malloc(total + 1);
    if (!replacement) return false;
    SDL_memcpy(replacement, base, (size_t)start);
    SDL_memcpy(replacement + start, insert, added);
    SDL_memcpy(replacement + start + added, base + end, length - (size_t)end + 1);
    result = TTF_SetTextString(edit->text, replacement, total);
    SDL_free(replacement);
    return result;
}

static bool GetHighlightExtents(EditBox *edit, int *marker, int *length)
{
    NormalizeBounds(edit);
    if (edit->highlight1 >= 0 && edit->highlight2 >= 0) {
        int marker1 = SDL_min(edit->highlight1, edit->highlight2);
        int marker2 = SDL_max(edit->highlight1, edit->highlight2);
        size_t text_length = edit->text->text ? SDL_strlen(edit->text->text) : 0;
        if (marker2 > marker1 && (size_t)marker2 <= text_length) {
            *marker = marker1;
            *length = marker2 - marker1;
            return true;
        }
    }
    return false;
}

static void ResetComposition(EditBox *edit)
{
    edit->composition_start = 0;
    edit->composition_length = 0;
    edit->composition_cursor = 0;
    edit->composition_cursor_length = 0;
    edit->composition_removed_length = 0;
}

static void DiscardComposition(EditBox *edit)
{
    SDL_free(edit->composition_snapshot);
    edit->composition_snapshot = NULL;
    ResetComposition(edit);
    ClearCandidates(edit);
}

static bool RestoreComposition(EditBox *edit)
{
    if (edit->composition_snapshot) {
        if (!TTF_SetTextString(edit->text, edit->composition_snapshot, 0)) return false;
        edit->cursor = edit->composition_saved_cursor;
        edit->cursor_trailing = edit->composition_saved_trailing;
        edit->highlight1 = edit->composition_saved_highlight1;
        edit->highlight2 = edit->composition_saved_highlight2;
    }
    DiscardComposition(edit);
    NormalizeBounds(edit);
    return true;
}

bool EditBox_CancelComposition(EditBox *edit)
{
    bool active = edit && edit->composition_snapshot;
    if (!edit || !edit->text || !RestoreComposition(edit)) return false;
    if (active) SDL_ClearComposition(edit->window);
    return true;
}

bool EditBox_SetText(EditBox *edit, const char *text)
{
    bool composing = edit && edit->composition_snapshot;
    if (!edit || !edit->text || !ValidText(text)) return false;
    if (!TTF_SetTextString(edit->text, text, 0)) return false;
    DiscardComposition(edit);
    if (composing) SDL_ClearComposition(edit->window);
    edit->highlight1 = edit->highlight2 = -1;
    edit->highlighting = false;
    edit->cursor_trailing = false;
    NormalizeBounds(edit);
    return true;
}

static int UTF8ByteLength(const char *text, int num_codepoints)
{
    const char *start = text;
    while (num_codepoints > 0) {
        Uint32 ch = SDL_StepUTF8(&text, NULL);
        if (ch == 0) {
            break;
        }
        --num_codepoints;
    }
    return (int)(uintptr_t)(text - start);
}

static void HandleComposition(EditBox *edit, const SDL_TextEditingEvent *event)
{
    int marker, removed = 0;
    bool first = !edit->composition_snapshot;
    if (!event->text || !*event->text) { RestoreComposition(edit); return; }
    if (!ValidText(event->text)) return;
    if (first) {
        NormalizeBounds(edit);
        edit->composition_snapshot = SDL_strdup(edit->text->text ? edit->text->text : "");
        if (!edit->composition_snapshot) return;
        edit->composition_saved_cursor = edit->cursor;
        edit->composition_saved_trailing = edit->cursor_trailing;
        edit->composition_saved_highlight1 = edit->highlight1;
        edit->composition_saved_highlight2 = edit->highlight2;
        marker = edit->cursor;
        GetHighlightExtents(edit, &marker, &removed);
        edit->composition_start = marker;
        edit->composition_removed_length = removed;
    }
    marker = edit->composition_start;
    removed = edit->composition_removed_length;
    if (!ReplaceRange(edit, edit->composition_snapshot, marker, marker + removed, event->text)) {
        if (first) DiscardComposition(edit);
        return;
    }
    edit->composition_length = (int)SDL_strlen(event->text);
    edit->cursor = marker;
    edit->cursor_trailing = false;
    edit->highlight1 = edit->highlight2 = -1;
    edit->highlighting = false;
    if (event->start > 0 || event->length > 0) {
        edit->composition_cursor = UTF8ByteLength(event->text, SDL_max(0, event->start));
        edit->composition_cursor_length = UTF8ByteLength(event->text + edit->composition_cursor, SDL_max(0, event->length));
    } else {
        edit->composition_cursor = edit->composition_length;
        edit->composition_cursor_length = 0;
    }
}

static void DrawComposition(EditBox *edit)
{
    if (HasLayout(edit)) {
        Uint8 r,g,b,a; TTF_GetTextColor(edit->text,&r,&g,&b,&a);
        SDL_SetRenderDrawColor(edit->renderer,r,g,b,a);
        CompositionUnderline context = {edit,1};
        edit->layout->selection(edit->layout_userdata,edit->composition_start,
            edit->composition_start+edit->composition_length,UnderlineCompositionRect,&context);
        if (edit->composition_cursor_length) {
            context.thickness=2;
            int start=edit->composition_start+edit->composition_cursor;
            edit->layout->selection(edit->layout_userdata,start,start+edit->composition_cursor_length,UnderlineCompositionRect,&context);
        }
        return;
    }
    /* Draw an underline under the composed text */
    SDL_Renderer *renderer = edit->renderer;
    int font_height = TTF_GetFontHeight(edit->font);
    TTF_SubString **substrings = TTF_GetTextSubStringsForRange(edit->text, edit->composition_start, edit->composition_length, NULL);
    if (substrings) {
        for (int i = 0; substrings[i]; ++i) {
            SDL_FRect rect;
            SDL_RectToFRect(&substrings[i]->rect, &rect);
            rect.x += edit->rect.x;
            rect.y += (edit->rect.y + font_height);
            rect.h = 1.0f;
            SDL_RenderFillRect(renderer, &rect);
        }
        SDL_free(substrings);
    }

    /* Thicken the underline under the active clause in the composed text */
    if (edit->composition_cursor_length > 0) {
        substrings = TTF_GetTextSubStringsForRange(edit->text, edit->composition_start + edit->composition_cursor, edit->composition_cursor_length, NULL);
        if (substrings) {
            for (int i = 0; substrings[i]; ++i) {
                SDL_FRect rect;
                SDL_RectToFRect(&substrings[i]->rect, &rect);
                rect.x += edit->rect.x;
                rect.y += (edit->rect.y + font_height) - 1;
                rect.h = 1.0f;
                SDL_RenderFillRect(renderer, &rect);
            }
            SDL_free(substrings);
        }
    }
}

static void DrawCompositionCursor(EditBox *edit)
{
    SDL_Renderer *renderer = edit->renderer;
    if (HasLayout(edit)) {
        if (!edit->composition_cursor_length) {
            SDL_FRect rect;
            if (EditBox_CaretRect(edit,edit->composition_start+edit->composition_cursor,&rect)) {
                rect.x+=edit->rect.x; rect.y+=edit->rect.y;
                Uint8 r,g,b,a; TTF_GetTextColor(edit->text,&r,&g,&b,&a);
                SDL_SetRenderDrawColor(renderer,r,g,b,a); SDL_RenderFillRect(renderer,&rect);
            }
        }
        return;
    }
    if (edit->composition_cursor_length == 0) {
        TTF_SubString cursor;
        if (TTF_GetTextSubString(edit->text, edit->composition_start + edit->composition_cursor, &cursor)) {
            SDL_FRect rect;

            SDL_RectToFRect(&cursor.rect, &rect);
            if ((cursor.flags & TTF_SUBSTRING_DIRECTION_MASK) == TTF_DIRECTION_RTL) {
                rect.x += cursor.rect.w;
            }
            rect.x += edit->rect.x;
            rect.y += edit->rect.y;
            rect.w = 1.0f;

            SDL_SetRenderDrawColor(renderer, 0, 0, 0, 0xFF);
            SDL_RenderFillRect(renderer, &rect);
        }
    }
}

static void ClearCandidates(EditBox *edit)
{
    if (edit->candidates) {
        TTF_DestroyText(edit->candidates);
        edit->candidates = NULL;
    }
    edit->selected_candidate_start = 0;
    edit->selected_candidate_length = 0;
}

static void SaveCandidates(EditBox *edit, const SDL_Event *event)
{
    int i;

    ClearCandidates(edit);

    bool horizontal = event->edit_candidates.horizontal;
    int num_candidates = event->edit_candidates.num_candidates;
    int selected_candidate = event->edit_candidates.selected_candidate;

    /* Calculate the length of the candidates text */
    size_t length = 0;
    for (i = 0; i < num_candidates; ++i) {
        if (horizontal) {
            if (i > 0) {
                ++length;
            }
        }

        length += SDL_strlen(event->edit_candidates.candidates[i]);

        if (!horizontal) {
            length += 1;
        }
    }
    if (length == 0) {
        return;
    }
    ++length; /* For null terminator */

    char *candidate_text = (char *)SDL_malloc(length);
    if (!candidate_text) {
        return;
    }

    char *dst = candidate_text;
    for (i = 0; i < num_candidates; ++i) {
        if (horizontal) {
            if (i > 0) {
                *dst++ = ' ';
            }
        }

        int length = (int)SDL_strlen(event->edit_candidates.candidates[i]);
        if (i == selected_candidate) {
            edit->selected_candidate_start = (int)(uintptr_t)(dst - candidate_text);
            edit->selected_candidate_length = length;
        }
        SDL_memcpy(dst, event->edit_candidates.candidates[i], length);
        dst += length;

        if (!horizontal) {
            *dst++ = '\n';
        }
    }
    *dst = '\0';

    edit->candidates = TTF_CreateText(TTF_GetTextEngine(edit->text), edit->font, candidate_text, 0);
    SDL_free(candidate_text);
    if (edit->candidates) {
        float r, g, b, a;
        TTF_GetTextColorFloat(edit->text, &r, &g, &b, &a);
        TTF_SetTextColorFloat(edit->candidates, r, g, b, a);
    } else {
        ClearCandidates(edit);
    }
}

static void DrawCandidates(EditBox *edit)
{
    SDL_Renderer *renderer = edit->renderer;
    SDL_Rect safe_rect;
    SDL_FRect candidates_rect;
    int candidates_w;
    int candidates_h;
    float x, y;

    /* Position the candidate window */
    SDL_FRect cursor;
    int offset = edit->composition_start;
    if (edit->composition_cursor_length > 0) {
        // Place the candidates at the active clause
        offset += edit->composition_cursor;
    }
    if (!EditBox_CaretRect(edit, offset, &cursor)) {
        return;
    }

    SDL_GetRenderSafeArea(renderer, &safe_rect);
    TTF_GetTextSize(edit->candidates, &candidates_w, &candidates_h);
    candidates_rect.x = edit->rect.x + cursor.x;
    candidates_rect.y = edit->rect.y + cursor.y + cursor.h + 2.0f;
    candidates_rect.w = 1.0f + 2.0f + candidates_w + 2.0f + 1.0f;
    candidates_rect.h = 1.0f + 2.0f + candidates_h + 2.0f + 1.0f;
    if ((candidates_rect.x + candidates_rect.w) > safe_rect.x + safe_rect.w) {
        candidates_rect.x = (safe_rect.x + safe_rect.w - candidates_rect.w);
        if (candidates_rect.x < safe_rect.x) {
            candidates_rect.x = (float)safe_rect.x;
        }
    }
    if (candidates_rect.y + candidates_rect.h > safe_rect.y + safe_rect.h)
        candidates_rect.y = SDL_max((float)safe_rect.y,edit->rect.y+cursor.y-candidates_rect.h-2);

    /* Draw the candidate background */
    SDL_Rect saved_clip;
    bool had_clip=SDL_RenderClipEnabled(renderer);
    SDL_GetRenderClipRect(renderer,&saved_clip);
    SDL_SetRenderClipRect(renderer,NULL);
    SDL_SetRenderDrawColor(renderer, 0xAA, 0xAA, 0xAA, 0xFF);
    SDL_RenderFillRect(renderer, &candidates_rect);
    SDL_SetRenderDrawColor(renderer, 0x00, 0x00, 0x00, 0xFF);
    SDL_RenderRect(renderer, &candidates_rect);

    /* Draw the candidates */
    x = candidates_rect.x + 3.0f;
    y = candidates_rect.y + 3.0f;
    DrawText(edit, edit->candidates, x, y);

    /* Underline the selected candidate */
    if (edit->selected_candidate_length > 0) {
        int font_height = TTF_GetFontHeight(edit->font);
        TTF_SubString **substrings = TTF_GetTextSubStringsForRange(edit->candidates, edit->selected_candidate_start, edit->selected_candidate_length, NULL);
        if (substrings) {
            for (int i = 0; substrings[i]; ++i) {
                SDL_FRect rect;
                SDL_RectToFRect(&substrings[i]->rect, &rect);
                rect.x += x;
                rect.y += (y + font_height);
                rect.h = 1.0f;
                SDL_RenderFillRect(renderer, &rect);
            }
            SDL_free(substrings);
        }
    }
    SDL_SetRenderClipRect(renderer,had_clip?&saved_clip:NULL);
}

static void UpdateTextInputArea(EditBox *edit)
{
    /* Convert the text input area and cursor into window coordinates */
    SDL_Renderer *renderer = edit->renderer;
    SDL_FPoint window_edit_rect_min;
    SDL_FPoint window_edit_rect_max;
    SDL_FPoint window_cursor;
    if (!SDL_RenderCoordinatesToWindow(renderer, edit->rect.x, edit->rect.y, &window_edit_rect_min.x, &window_edit_rect_min.y) ||
        !SDL_RenderCoordinatesToWindow(renderer, edit->rect.x + edit->rect.w, edit->rect.y + edit->rect.h, &window_edit_rect_max.x, &window_edit_rect_max.y) ||
        !SDL_RenderCoordinatesToWindow(renderer, edit->cursor_rect.x, edit->cursor_rect.y, &window_cursor.x, &window_cursor.y)) {
        return;
    }

    SDL_Rect rect;
    float bottom_x,bottom_y;
    if (!SDL_RenderCoordinatesToWindow(renderer,edit->cursor_rect.x+edit->cursor_rect.w,
        edit->cursor_rect.y+edit->cursor_rect.h,&bottom_x,&bottom_y)) return;
    rect.x = (int)SDL_roundf(window_cursor.x);
    rect.y = (int)SDL_roundf(window_cursor.y);
    rect.w = SDL_max(1,(int)SDL_ceilf(bottom_x-window_cursor.x));
    rect.h = SDL_max(1,(int)SDL_ceilf(bottom_y-window_cursor.y));
    SDL_SetTextInputArea(edit->window, &rect, 0);
}

static void DrawCursor(EditBox *edit)
{
    if (edit->composition_length > 0) {
        DrawCompositionCursor(edit);
        return;
    }

    SDL_Renderer *renderer = edit->renderer;
    Uint8 r, g, b, a;
    TTF_GetTextColor(edit->text, &r, &g, &b, &a);
    SDL_SetRenderDrawColor(renderer, r, g, b, a);
    SDL_RenderFillRect(renderer, &edit->cursor_rect);
}

EditBox *EditBox_Create(SDL_Window *window, SDL_Renderer *renderer, TTF_TextEngine *engine, TTF_Font *font, const SDL_FRect *rect)
{
    EditBox *edit = (EditBox *)SDL_calloc(1, sizeof(*edit));
    if (!edit) {
        return NULL;
    }

    edit->window = window;
    edit->renderer = renderer;
    edit->font = font;
    edit->text = TTF_CreateText(engine, font, NULL, 0);
    if (!edit->text) {
        EditBox_Destroy(edit);
        return NULL;
    }
    edit->rect = *rect;
    edit->highlight1 = -1;
    edit->highlight2 = -1;

    /* Wrap the editbox text within the editbox area */
    TTF_SetTextWrapWidth(edit->text, (int)SDL_floorf(rect->w));

    /* Show the whitespace when wrapping, so it can be edited */
    TTF_SetTextWrapWhitespaceVisible(edit->text, true);

#ifdef TEST_SURFACE_ENGINE
    /* Grab the window surface if we want to test the surface text engine.
     * This isn't strictly necessary, we can still use the renderer if it's
     * a software renderer targeting an SDL_Surface.
     */
    edit->window_surface = (SDL_Surface *)SDL_GetPointerProperty(SDL_GetRendererProperties(renderer), SDL_PROP_RENDERER_SURFACE_POINTER, NULL);
#endif

    /* We support rendering the composition and candidates */
    SDL_SetHint(SDL_HINT_IME_IMPLEMENTED_UI, "composition,candidates");

    return edit;
}

void EditBox_Destroy(EditBox *edit)
{
    if (!edit) {
        return;
    }

    DiscardComposition(edit);
    TTF_DestroyText(edit->text);
    SDL_free(edit);
}

void EditBox_SetFocus(EditBox *edit, bool focus)
{
    if (!edit) {
        return;
    }

    if (edit->has_focus == focus) {
        return;
    }

    if (!focus && !EditBox_CancelComposition(edit)) return;

    edit->has_focus = focus;

    if (edit->has_focus) {
        SDL_StartTextInput(edit->window);
    } else {
        SDL_StopTextInput(edit->window);
    }
}

void EditBox_Draw(EditBox *edit)
{
    if (!edit) {
        return;
    }

    SDL_Renderer *renderer = edit->renderer;
    float x = edit->rect.x;
    float y = edit->rect.y;

    /* Draw any highlight */
    bool rich = HasLayout(edit);
    int marker = 0, length = 0;
    bool selected = GetHighlightExtents(edit, &marker, &length);
    if (rich) {
        edit->layout->draw(edit->layout_userdata,x,y,marker,marker+length);
    } else if (selected) {
        TTF_SubString **highlights = TTF_GetTextSubStringsForRange(edit->text, marker, length, NULL);
        if (highlights) {
            int i;
            SDL_SetRenderDrawColor(renderer, 0xEE, 0xEE, 0x00, 0xFF);
            for (i = 0; highlights[i]; ++i) {
                SDL_FRect rect;
                SDL_RectToFRect(&highlights[i]->rect, &rect);
                rect.x += x;
                rect.y += y;
                SDL_RenderFillRect(renderer, &rect);
            }
            SDL_free(highlights);
        }
    }

    if (!rich) DrawText(edit, edit->text, x, y);

    if (edit->has_focus) {
        /* Draw the cursor */
        Uint64 now = SDL_GetTicks();
        if ((now - edit->last_cursor_change) >= CURSOR_BLINK_INTERVAL_MS) {
            edit->cursor_visible = !edit->cursor_visible;
            edit->last_cursor_change = now;
        }

        /* Calculate the cursor rect, used for positioning candidates */
        SDL_FRect cursor_rect;
        int position = edit->composition_length ? edit->composition_start + edit->composition_cursor : edit->cursor;
        if (EditBox_CaretRect(edit, position, &cursor_rect)) {
            cursor_rect.x += edit->rect.x;
            cursor_rect.y += edit->rect.y;
            cursor_rect.w = 1.0f;
            SDL_copyp(&edit->cursor_rect, &cursor_rect);

            UpdateTextInputArea(edit);
        }

        if (edit->composition_length > 0) {
            DrawComposition(edit);
        }

        if (edit->candidates) {
            DrawCandidates(edit);
        }

        if (edit->cursor_visible) {
            DrawCursor(edit);
        }
    }
}

static int GetCursorTextIndex(int x, const TTF_SubString *substring)
{
    if (substring->flags & (TTF_SUBSTRING_LINE_END | TTF_SUBSTRING_TEXT_END)) {
        return substring->offset;
    }

    bool round_down;
    if ((substring->flags & TTF_SUBSTRING_DIRECTION_MASK) == TTF_DIRECTION_RTL) {
        round_down = (x > (substring->rect.x + substring->rect.w / 2));
    } else {
        round_down = (x < (substring->rect.x + substring->rect.w / 2));
    }
    if (round_down) {
        /* Start the cursor before the selected text */
        return substring->offset;
    } else {
        /* Place the cursor after the selected text */
        return substring->offset + substring->length;
    }
}

static void SetCursorPosition(EditBox *edit, int position)
{
    position = ClampPosition(edit->text->text ? edit->text->text : "", position);
    if (edit->composition_snapshot) {
        /* Don't let the cursor be moved into the composition */
        if (position >= edit->composition_start && position <= (edit->composition_start + edit->composition_length)) {
            return;
        }

        if (position > edit->composition_start + edit->composition_length)
            position += edit->composition_removed_length - edit->composition_length;
        if (!EditBox_CancelComposition(edit)) return;
    }

    edit->cursor = ClampPosition(edit->text->text ? edit->text->text : "", position);
    edit->cursor_trailing = false;
}

static void MoveCursorIndex(EditBox *edit, int direction)
{
    TTF_SubString substring;
    NormalizeBounds(edit);
    if (HasLayout(edit)) {
        SetCursorPosition(edit, edit->layout->step(edit->layout_userdata,edit->cursor,direction));
        return;
    }

    if (direction < 0) {
        if (TTF_GetTextSubString(edit->text, edit->cursor - 1, &substring)) {
            SetCursorPosition(edit, substring.offset);
        }
    } else {
        if (TTF_GetTextSubString(edit->text, edit->cursor, &substring) &&
            TTF_GetTextSubString(edit->text, substring.offset + SDL_max(substring.length, 1), &substring)) {
            SetCursorPosition(edit, substring.offset);
        }
    }
}

void EditBox_MoveCursorLeft(EditBox *edit)
{
    if (!edit) {
        return;
    }

    TTF_SubString substring;
    if (TTF_GetTextSubString(edit->text, edit->cursor, &substring) &&
        (substring.flags & TTF_SUBSTRING_DIRECTION_MASK) == TTF_DIRECTION_RTL) {
        MoveCursorIndex(edit, 1);
    } else {
        MoveCursorIndex(edit, -1);
    }
}

void EditBox_MoveCursorRight(EditBox *edit)
{
    if (!edit) {
        return;
    }

    TTF_SubString substring;
    if (TTF_GetTextSubString(edit->text, edit->cursor, &substring) &&
        (substring.flags & TTF_SUBSTRING_DIRECTION_MASK) == TTF_DIRECTION_RTL) {
        MoveCursorIndex(edit, -1);
    } else {
        MoveCursorIndex(edit, 1);
    }
}

void EditBox_MoveCursorUp(EditBox *edit)
{
    if (!edit) {
        return;
    }

    if (HasLayout(edit)) {
        SDL_FRect caret;
        if (EditBox_CaretRect(edit, edit->cursor, &caret)) {
            float target_y = caret.y - TTF_GetFontHeight(edit->font) / 2.0f;
            int position = edit->layout->hit(edit->layout_userdata, caret.x, target_y);
            SetCursorPosition(edit,position);
            SDL_FRect next;
            if (edit->layout->caret(edit->layout_userdata,position,false,&next))
                edit->cursor_trailing = next.y > target_y;
        }
        return;
    }

    TTF_SubString substring;
    if (TTF_GetTextSubString(edit->text, edit->cursor, &substring)) {
        int fontHeight = TTF_GetFontHeight(edit->font);
        int x, y;
        if ((substring.flags & TTF_SUBSTRING_DIRECTION_MASK) == TTF_DIRECTION_RTL) {
            x = substring.rect.x + substring.rect.w - 1;
        } else {
            x = substring.rect.x;
        }
        y = substring.rect.y - fontHeight / 2;
        if (TTF_GetTextSubStringForPoint(edit->text, x, y, &substring)) {
            SetCursorPosition(edit, GetCursorTextIndex(x, &substring));
        }
    }
}

void EditBox_MoveCursorDown(EditBox *edit)
{
    if (!edit) {
        return;
    }

    if (HasLayout(edit)) {
        SDL_FRect caret;
        if (EditBox_CaretRect(edit, edit->cursor, &caret)) {
            float target_y = caret.y + caret.h + 1;
            int position = edit->layout->hit(edit->layout_userdata, caret.x, target_y);
            SetCursorPosition(edit,position);
            SDL_FRect next;
            if (edit->layout->caret(edit->layout_userdata,position,false,&next))
                edit->cursor_trailing = next.y > target_y;
        }
        return;
    }

    TTF_SubString substring;
    if (TTF_GetTextSubString(edit->text, edit->cursor, &substring)) {
        int fontHeight = TTF_GetFontHeight(edit->font);
        int x, y;
        if ((substring.flags & TTF_SUBSTRING_DIRECTION_MASK) == TTF_DIRECTION_RTL) {
            x = substring.rect.x + substring.rect.w - 1;
        } else {
            x = substring.rect.x;
        }
        y = substring.rect.y + substring.rect.h + fontHeight / 2;
        if (TTF_GetTextSubStringForPoint(edit->text, x, y, &substring)) {
            SetCursorPosition(edit, GetCursorTextIndex(x, &substring));
        }
    }
}

void EditBox_MoveCursorBeginningOfLine(EditBox *edit)
{
    if (!edit) {
        return;
    }

    if (HasLayout(edit)) {
        SDL_FRect caret;
        if (EditBox_CaretRect(edit, edit->cursor, &caret)) {
            float target_y = caret.y + caret.h / 2;
            int position = edit->layout->hit(edit->layout_userdata, -1000000000.0f, target_y);
            SetCursorPosition(edit,position);
            SDL_FRect next;
            if (edit->layout->caret(edit->layout_userdata,position,false,&next))
                edit->cursor_trailing = next.y > target_y;
        }
        return;
    }

    TTF_SubString substring;
    if (TTF_GetTextSubString(edit->text, edit->cursor, &substring) &&
        TTF_GetTextSubStringForLine(edit->text, substring.line_index, &substring)) {
        SetCursorPosition(edit, substring.offset);
    }
}

void EditBox_MoveCursorEndOfLine(EditBox *edit)
{
    if (!edit) {
        return;
    }

    if (HasLayout(edit)) {
        SDL_FRect caret;
        if (EditBox_CaretRect(edit, edit->cursor, &caret)) {
            float target_y = caret.y + caret.h / 2;
            int position = edit->layout->hit(edit->layout_userdata, 1000000000.0f, target_y);
            SetCursorPosition(edit,position);
            SDL_FRect next;
            if (edit->layout->caret(edit->layout_userdata,position,false,&next))
                edit->cursor_trailing = next.y > target_y;
        }
        return;
    }

    TTF_SubString substring;
    if (TTF_GetTextSubString(edit->text, edit->cursor, &substring) &&
        TTF_GetTextSubStringForLine(edit->text, substring.line_index, &substring)) {
        int end=ClampPosition(edit->text->text,substring.offset+substring.length);
        while(end>substring.offset && (edit->text->text[end-1]=='\n' || edit->text->text[end-1]=='\r')) end--;
        SetCursorPosition(edit,end);
    }
}

void EditBox_MoveCursorBeginning(EditBox *edit)
{
    if (!edit) {
        return;
    }

    /* Move to the beginning of the text */
    SetCursorPosition(edit, 0);
}

void EditBox_MoveCursorEnd(EditBox *edit)
{
    if (!edit) {
        return;
    }

    /* Move to the end of the text */
    if (edit->text->text) {
        SetCursorPosition(edit, (int)SDL_strlen(edit->text->text));
    }
}

/* Deletion uses the same shaped cluster boundaries as navigation. A line
 * separator can be represented by a zero-length SDL substring; only that
 * boundary falls back to advancing one complete UTF-8 codepoint. */
static int ClusterBoundary(EditBox *edit, int offset, int direction)
{
    const char *text=edit->text->text ? edit->text->text : "";
    int cursor=offset, length=(int)SDL_strlen(text), boundary=cursor;
    if ((direction<0 && cursor==0) || (direction>0 && cursor==length)) return cursor;
    if (HasLayout(edit)) {
        boundary=edit->layout->step(edit->layout_userdata,cursor,direction);
    } else {
        TTF_SubString sub;
        if (!TTF_GetTextSubString(edit->text,direction<0?cursor-1:cursor,&sub)) return cursor;
        boundary=direction<0 ? sub.offset : sub.offset+sub.length;
        if (boundary==cursor) {
            const char *next=text+cursor;
            if (direction<0) SDL_StepBackUTF8(text,&next);
            else { size_t remaining=(size_t)(length-cursor); SDL_StepUTF8(&next,&remaining); }
            boundary=(int)(next-text);
        }
    }
    if (boundary<0 || boundary>length || ClampPosition(text,boundary)!=boundary ||
        (direction<0 ? boundary>=cursor : boundary<=cursor)) return cursor;
    return boundary;
}

static bool CrossesLine(const char *text,int a,int b)
{
    for(int i=a;i<b;i++) if(text[i]=='\n' || text[i]=='\r') return true;
    return false;
}
/* Word navigation is deliberately bounded by the current logical paragraph.
 * ASCII words, non-ASCII letters and CJK runs form words; punctuation forms a
 * token; each shaped emoji cluster is one token. No language dictionary is
 * implied by these deterministic editor boundaries. */
static int WordClass(const char *text,int offset)
{
    const char *p=text+offset; Uint32 ch=SDL_StepUTF8(&p,NULL);
    if(ch==' ' || ch=='\t' || ch==0xa0 || ch==0x3000) return 0;
    if((ch>='a' && ch<='z') || (ch>='A' && ch<='Z') || (ch>='0' && ch<='9') || ch=='_') return 1;
    if((ch>=0x1f000 && ch<=0x1faff) || (ch>=0x2600 && ch<=0x27bf)) return 3;
    if(ch<128 || (ch>=0x2000 && ch<=0x206f) || (ch>=0x3000 && ch<=0x303f) ||
       (ch>=0xff00 && ch<=0xff65)) return 2;
    return 1;
}
static int WordBoundary(EditBox *edit,int direction)
{
    const char *text=edit->text->text?edit->text->text:"";
    int length=(int)SDL_strlen(text),pos=edit->cursor;
    /* First pass skips spaces next to the caret, never a paragraph break. */
    for(;;) {
        int next=ClusterBoundary(edit,pos,direction);
        int lo=SDL_min(pos,next),hi=SDL_max(pos,next);
        if(next==pos || CrossesLine(text,lo,hi)) return pos;
        if(WordClass(text,lo)!=0) break;
        pos=next;
    }
    int next=ClusterBoundary(edit,pos,direction);
    int token=WordClass(text,SDL_min(pos,next));
    for(;;) {
        int lo=SDL_min(pos,next),hi=SDL_max(pos,next);
        if(next==pos || CrossesLine(text,lo,hi) || WordClass(text,lo)!=token) return pos;
        pos=next;
        if(token==3 || pos==0 || pos==length) return pos;
        next=ClusterBoundary(edit,pos,direction);
    }
}
static void MoveWord(EditBox *edit,int direction)
{
    NormalizeBounds(edit);
    SetCursorPosition(edit,WordBoundary(edit,direction));
}
static void DeleteModified(EditBox *edit,int direction,bool line)
{
    if(!EditBox_CancelComposition(edit)) return;
    NormalizeBounds(edit);
    int marker,length;
    if(GetHighlightExtents(edit,&marker,&length)) { EditBox_DeleteHighlight(edit); return; }
    int cursor=edit->cursor,boundary;
    if(line) {
        if(direction<0) EditBox_MoveCursorBeginningOfLine(edit);
        else EditBox_MoveCursorEndOfLine(edit);
        boundary=edit->cursor;
        edit->cursor=cursor;
    } else boundary=WordBoundary(edit,direction);
    int start=SDL_min(cursor,boundary),end=SDL_max(cursor,boundary);
    /* Even a malformed/custom layout must not turn a modified deletion into
     * deletion of another paragraph. The unmodified key still joins lines. */
    if(CrossesLine(edit->text->text,start,end)) return;
    if(start<end && ReplaceRange(edit,edit->text->text,start,end,"")) {
        edit->cursor=start; edit->cursor_trailing=false;
        edit->highlight1=edit->highlight2=-1;
    }
}

void EditBox_Backspace(EditBox *edit)
{
    if (!edit || !edit->text->text) {
        return;
    }

    if (!EditBox_CancelComposition(edit)) return;
    NormalizeBounds(edit);
    int marker, selected_length;
    if (GetHighlightExtents(edit, &marker, &selected_length)) {
        EditBox_DeleteHighlight(edit);
        return;
    }

    if (edit->cursor > 0) {
        int start=ClusterBoundary(edit,edit->cursor,-1);
        if (start<edit->cursor && ReplaceRange(edit,edit->text->text,start,edit->cursor,"")) {
            edit->cursor=start;
            edit->cursor_trailing=false;
        }
    }
}

void EditBox_BackspaceToBeginning(EditBox *edit)
{
    if (!edit) {
        return;
    }

    if (!EditBox_CancelComposition(edit)) return;
    NormalizeBounds(edit);
    if (ReplaceRange(edit, edit->text->text ? edit->text->text : "", 0, edit->cursor, "")) {
        edit->cursor = 0;
        edit->highlight1 = edit->highlight2 = -1;
    }
}

void EditBox_DeleteToEnd(EditBox *edit)
{
    if (!edit) {
        return;
    }

    if (!EditBox_CancelComposition(edit)) return;
    NormalizeBounds(edit);
    const char *text = edit->text->text ? edit->text->text : "";
    if (ReplaceRange(edit, text, edit->cursor, (int)SDL_strlen(text), "")) edit->highlight1 = edit->highlight2 = -1;
}

void EditBox_Delete(EditBox *edit)
{
    if (!edit || !edit->text->text) {
        return;
    }

    if (!EditBox_CancelComposition(edit)) return;
    NormalizeBounds(edit);
    int marker, selected_length;
    if (GetHighlightExtents(edit, &marker, &selected_length)) {
        EditBox_DeleteHighlight(edit);
        return;
    }

    int end=ClusterBoundary(edit,edit->cursor,1);
    if (end>edit->cursor && ReplaceRange(edit,edit->text->text,edit->cursor,end,"")) edit->cursor_trailing=false;
}

static bool HandleMouseDown(EditBox *edit, float x, float y)
{
    SDL_FPoint pt = { x, y };
    if (!SDL_PointInRectFloat(&pt, &edit->rect)) {
        if (edit->has_focus) {
            EditBox_SetFocus(edit, false);
            return true;
        }
        return false;
    }

    if (!edit->has_focus) {
        EditBox_SetFocus(edit, true);
    }

    /* Set the cursor position */
    float textX = x - edit->rect.x, textY = y - edit->rect.y;
    if (HasLayout(edit)) {
        int position = edit->layout->hit(edit->layout_userdata,textX,textY);
        SetCursorPosition(edit,position);
        SDL_FRect rect;
        if (edit->layout->caret(edit->layout_userdata,position,false,&rect))
            edit->cursor_trailing = rect.y > textY;
    } else {
        TTF_SubString substring;
        if (!TTF_GetTextSubStringForPoint(edit->text,(int)SDL_roundf(textX),(int)SDL_roundf(textY),&substring)) return false;
        SetCursorPosition(edit,GetCursorTextIndex((int)textX,&substring));
    }
    edit->highlighting = true;
    edit->highlight1 = edit->cursor;
    edit->highlight2 = -1;

    return true;
}

static bool HandleMouseMotion(EditBox *edit, float x, float y)
{
    if (!edit->highlighting) {
        return false;
    }

    /* Set the highlight position */
    float textX = x - edit->rect.x, textY = y - edit->rect.y;
    if (HasLayout(edit)) {
        int position = edit->layout->hit(edit->layout_userdata,textX,textY);
        SetCursorPosition(edit,position);
        SDL_FRect rect;
        if (edit->layout->caret(edit->layout_userdata,position,false,&rect))
            edit->cursor_trailing = rect.y > textY;
    } else {
        TTF_SubString substring;
        if (!TTF_GetTextSubStringForPoint(edit->text,(int)SDL_roundf(textX),(int)SDL_roundf(textY),&substring)) return false;
        SetCursorPosition(edit,GetCursorTextIndex((int)textX,&substring));
    }
    edit->highlight2 = edit->cursor;

    return true;
}

static bool HandleMouseUp(EditBox *edit, float x, float y)
{
    (void)x; (void)y;

    if (!edit->highlighting) {
        return false;
    }

    edit->highlighting = false;
    return true;
}

void EditBox_SelectAll(EditBox *edit)
{
    if (!edit || !edit->text->text) {
        return;
    }

    if (!EditBox_CancelComposition(edit)) return;
    NormalizeBounds(edit);
    edit->highlight1 = 0;
    edit->highlight2 = (int)SDL_strlen(edit->text->text);
    edit->cursor = edit->highlight2;
}

bool EditBox_DeleteHighlight(EditBox *edit)
{
    if (!edit || !edit->text->text) {
        return false;
    }

    if (!EditBox_CancelComposition(edit)) return false;
    int marker, length;
    if (GetHighlightExtents(edit, &marker, &length)) {
        if (!ReplaceRange(edit, edit->text->text, marker, marker + length, "")) return false;
        edit->cursor = marker;
        edit->highlight1 = -1;
        edit->highlight2 = -1;
        return true;
    }
    return false;
}

void EditBox_Copy(EditBox *edit)
{
    if (!edit || !edit->text->text) {
        return;
    }

    if (!EditBox_CancelComposition(edit)) return;
    int marker, length;
    if (GetHighlightExtents(edit, &marker, &length)) {
        char *temp = (char *)SDL_malloc((size_t)length + 1);
        if (temp) {
            SDL_memcpy(temp, &edit->text->text[marker], length);
            temp[length] = '\0';
            SDL_SetClipboardText(temp);
            SDL_free(temp);
        }
    }
}

void EditBox_Cut(EditBox *edit)
{
    if (!edit || !edit->text->text) {
        return;
    }

    if (!EditBox_CancelComposition(edit)) return;
    /* Copy to clipboard and delete text */
    int marker, length;
    if (GetHighlightExtents(edit, &marker, &length)) {
        char *temp = (char *)SDL_malloc((size_t)length + 1);
        if (temp) {
            SDL_memcpy(temp, &edit->text->text[marker], length);
            temp[length] = '\0';
            bool copied = SDL_SetClipboardText(temp);
            SDL_free(temp);
            if (!copied) return;
        } else {
            return;
        }
        if (!ReplaceRange(edit, edit->text->text, marker, marker + length, "")) return;
        edit->cursor = marker;
        edit->highlight1 = -1;
        edit->highlight2 = -1;
    }
}

void EditBox_Paste(EditBox *edit)
{
    if (!edit) {
        return;
    }

    char *text = SDL_GetClipboardText();
    EditBox_Insert(edit, text);
    SDL_free(text);
}

void EditBox_Insert(EditBox *edit, const char *text)
{
    if (!edit || !text || !*text || !ValidText(text)) {
        return;
    }
    NormalizeBounds(edit);
    int marker = edit->cursor, removed = 0;
    const char *base = edit->text->text ? edit->text->text : "";
    if (edit->composition_snapshot) {
        base = edit->composition_snapshot;
        marker = edit->composition_start;
        removed = edit->composition_removed_length;
    } else GetHighlightExtents(edit, &marker, &removed);
    size_t added = SDL_strlen(text);
    if (!ReplaceRange(edit, base, marker, marker + removed, text)) return;
    DiscardComposition(edit);
    edit->cursor = marker + (int)added;
    edit->cursor_trailing = false;
    edit->highlight1 = edit->highlight2 = -1;
    edit->highlighting = false;
}

bool EditBox_HandleEvent(EditBox *edit, SDL_Event *event)
{
    if (!edit || !event) {
        return false;
    }

    switch (event->type) {
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
        return HandleMouseDown(edit, event->button.x, event->button.y);

    case SDL_EVENT_MOUSE_MOTION:
        return HandleMouseMotion(edit, event->motion.x, event->motion.y);

    case SDL_EVENT_MOUSE_BUTTON_UP:
        return HandleMouseUp(edit, event->button.x, event->button.y);

    case SDL_EVENT_KEY_DOWN:
        if (!edit->has_focus) {
            break;
        }
        /* While the IME owns keyboard input, only its TEXT_EDITING/TEXT_INPUT
         * events can alter text. Return/Backspace must not commit preedit. */
        if (edit->composition_snapshot) {
            if (event->key.key == SDLK_ESCAPE) EditBox_CancelComposition(edit);
            return true;
        }

        switch (event->key.key) {
        case SDLK_A:
            if (event->key.mod & SDL_KMOD_CTRL) {
                EditBox_SelectAll(edit);
            }
            break;
        case SDLK_C:
            if (event->key.mod & SDL_KMOD_CTRL) {
                EditBox_Copy(edit);
            }
            break;

        case SDLK_V:
            if (event->key.mod & SDL_KMOD_CTRL) {
                EditBox_Paste(edit);
            }
            break;

        case SDLK_X:
            if (event->key.mod & SDL_KMOD_CTRL) {
                EditBox_Cut(edit);
            }
            break;

        case SDLK_LEFT:
            if (event->key.mod & SDL_KMOD_GUI) {
                EditBox_MoveCursorBeginningOfLine(edit);
            } else if (event->key.mod & (SDL_KMOD_CTRL|SDL_KMOD_ALT)) {
                MoveWord(edit,-1);
            } else {
                EditBox_MoveCursorLeft(edit);
            }
            break;

        case SDLK_RIGHT:
            if (event->key.mod & SDL_KMOD_GUI) {
                EditBox_MoveCursorEndOfLine(edit);
            } else if (event->key.mod & (SDL_KMOD_CTRL|SDL_KMOD_ALT)) {
                MoveWord(edit,1);
            } else {
                EditBox_MoveCursorRight(edit);
            }
            break;

        case SDLK_UP:
            if (event->key.mod & SDL_KMOD_CTRL) {
                EditBox_MoveCursorBeginning(edit);
            } else {
                EditBox_MoveCursorUp(edit);
            }
            break;

        case SDLK_DOWN:
            if (event->key.mod & SDL_KMOD_CTRL) {
                EditBox_MoveCursorEnd(edit);
            } else {
                EditBox_MoveCursorDown(edit);
            }
            break;

        case SDLK_HOME:
            if (event->key.mod & (SDL_KMOD_CTRL|SDL_KMOD_GUI)) EditBox_MoveCursorBeginning(edit);
            else EditBox_MoveCursorBeginningOfLine(edit);
            break;

        case SDLK_END:
            if (event->key.mod & (SDL_KMOD_CTRL|SDL_KMOD_GUI)) EditBox_MoveCursorEnd(edit);
            else EditBox_MoveCursorEndOfLine(edit);
            break;

        case SDLK_BACKSPACE:
            if (event->key.mod & SDL_KMOD_GUI) {
                DeleteModified(edit,-1,true);
            } else if (event->key.mod & (SDL_KMOD_CTRL|SDL_KMOD_ALT)) {
                DeleteModified(edit,-1,false);
            } else {
                EditBox_Backspace(edit);
            }
            break;

        case SDLK_DELETE:
            if (event->key.mod & SDL_KMOD_GUI) {
                DeleteModified(edit,1,true);
            } else if (event->key.mod & (SDL_KMOD_CTRL|SDL_KMOD_ALT)) {
                DeleteModified(edit,1,false);
            } else {
                EditBox_Delete(edit);
            }
            break;

        case SDLK_RETURN:
            EditBox_Insert(edit, "\n");
            break;

        case SDLK_ESCAPE:
            EditBox_SetFocus(edit, false);
            break;

        default:
            break;
        }
        return true;

    case SDL_EVENT_TEXT_INPUT:
        if (!edit->has_focus) break;
        EditBox_Insert(edit, event->text.text);
        return true;

    case SDL_EVENT_TEXT_EDITING:
        if (!edit->has_focus) break;
        HandleComposition(edit, &event->edit);
        break;

    case SDL_EVENT_TEXT_EDITING_CANDIDATES:
        if (!edit->has_focus) break;
        ClearCandidates(edit);
        SaveCandidates(edit, event);
        break;

    default:
        break;
    }
    return false;
}


