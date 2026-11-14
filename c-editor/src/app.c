/* Note Lab: shared C application, used by SDL's desktop/mobile/web entry points. */
#define SDL_MAIN_USE_CALLBACKS 1
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#include <SDL3_ttf/SDL_ttf.h>
#include "document.h"
#include "rich_layout.h"
#include "citations.h"
#include "library_panel.h"
#include "platform_import.h"
#include "cJSON.h"
#include "note_store.h"
#include "platform_persistence.h"
#include "platform_export.h"
#include "platform_web_input.h"
#include "editbox.h"
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#endif

typedef enum { PROMPT_NONE, PROMPT_NEW, PROMPT_FIND, PROMPT_LINK, PROMPT_DELETE,
    PROMPT_LIBRARY_SEARCH, PROMPT_CITATION_PAGE, PROMPT_CITATION_NOTE } Prompt;
typedef enum { LIB_CLOSE, LIB_IMPORT, LIB_ITEMS, LIB_CURRENT, LIB_SEARCH,
    LIB_COLLECTIONS, LIB_ALL, LIB_DESCENDANTS, LIB_PAGE, LIB_NOTE, LIB_INSERT,
    LIB_OPEN_ITEM, LIB_OPEN_PDF, LIB_STYLE, LIB_SYNC, LIB_VIEW, LIB_RETRY, LIB_BUTTON_COUNT } LibraryAction;
typedef enum { ACT_SIDEBAR, ACT_NEW, ACT_SAVE, ACT_UNDO, ACT_REDO, ACT_BOLD,
    ACT_ITALIC, ACT_UNDERLINE, ACT_HIGHLIGHT, ACT_RED, ACT_LINK, ACT_BULLETS,
    ACT_NUMBERS, ACT_HEADING, ACT_FIND, ACT_THEME, ACT_TYPEWRITER, ACT_JSON,
    ACT_DELETE, ACT_REFRESH, ACT_PARAGRAPH, ACT_CLEAR, ACT_LEFT, ACT_CENTER,
    ACT_RIGHT, ACT_JUSTIFY, ACT_FIND_PREVIOUS, ACT_FIND_NEXT, ACT_MOVE_UP,
    ACT_MOVE_DOWN, ACT_MORE } Action;
typedef struct { const char *label; Action action; SDL_FRect box; } Button;
typedef struct {
    SDL_Window *window;
    SDL_Renderer *renderer;
    TTF_Font *font, *ui_font, *emoji_font, *emoji_ui_font;
    TTF_TextEngine *engine;
    EditBox *edit, *prompt;
    CeDocument *doc;
    CeRichLayout *rich;
    char *rich_text;
    float rich_width;
    bool rich_valid, rich_dirty, rich_dark;
    PlatformExport *export_job;
    PlatformImport *import_job;
    PlatformImportKind import_kind;
    char *pending_note_json, *pending_note_stem;
    CeLibraryPanel library;
    bool library_open, library_citation_mode, library_dragging, library_drag_moved;
    float library_drag_y, library_drag_origin, library_outer_scroll, library_content_height;
    size_t library_insert_offset, library_pressed_row;
    CeZoteroApproximateStyle citation_style;
    char *citation_id, *citation_title, *citation_page, *citation_note;
    char *citation_item_uri, *citation_pdf_uri, *insert_page, *insert_note;
    SDL_FRect library_button, library_area, library_rows, library_buttons[LIB_BUTTON_COUNT];
    NoteStore store;
    NoteStore library_store;
    char *notes_root;
    bool library_store_ready, library_cache_saving, library_unsaved, library_restore_failed;
    char library_warning[512];
    NoteList files;
    char *active;
    char *search_keyword;
    char *saving_name, *saving_snapshot;
    bool saving, persistence_error;
    char status[512];
    Prompt prompt_mode;
    size_t selection_start, selection_end;
    SDL_FRect editor_area, sidebar_area, note_import_button, prompt_area, prompt_input_area, prompt_accept, prompt_cancel;
    int width, height;
    float scroll, sidebar_scroll, sidebar_width, prompt_scroll;
    int toolbar_page;
    bool resizing_sidebar;
    bool dark, sidebar, sidebar_manual, typewriter, smoke;
    Uint64 edited_at;
    unsigned frames;
    const char *screenshot;
    Button buttons[31];
} App;

static void reveal_cursor(App *a);
static void library_show(App *a);
static void library_close(App *a);
static void library_layout(App *a);
static void library_draw(App *a);
static void library_event(App *a, SDL_Event *event);
static void library_import_poll(App *a);
static bool library_prompt_accept(App *a, const char *value);
static bool library_cache_save(App *a);
static bool library_cache_load(App *a, bool startup);
static void document_import_begin(App *a);
static bool document_import_prepare(App *a, const PlatformImportFile *file);
static void document_import_finish(App *a);

static char *copy_string(const char *s) {
    size_t len = strlen(s) + 1;
    char *out = malloc(len);
    if (out) memcpy(out, s, len);
    return out;
}
static void message(App *a, const char *s) { SDL_strlcpy(a->status, s, sizeof(a->status)); }
static SDL_Color ink(App *a) { return a->dark ? (SDL_Color){225,229,234,255} : (SDL_Color){35,44,52,255}; }
static SDL_Color paper(App *a) { return a->dark ? (SDL_Color){27,32,38,255} : (SDL_Color){255,255,253,255}; }
static void fill(App *a, SDL_FRect box, SDL_Color color) {
    SDL_SetRenderDrawColor(a->renderer, color.r, color.g, color.b, color.a);
    SDL_RenderFillRect(a->renderer, &box);
}
static bool contains(SDL_FRect r, float x, float y) {
    return x >= r.x && y >= r.y && x < r.x + r.w && y < r.y + r.h;
}
static void label(App *a, const char *str, float x, float y, SDL_Color color) {
    TTF_Text *text = TTF_CreateText(a->engine, a->ui_font, str, 0);
    if (!text) return;
    TTF_SetTextColor(text, color.r, color.g, color.b, color.a);
    TTF_DrawRendererText(text, x, y);
    TTF_DestroyText(text);
}
static void update_title(App *a) {
    char title[512];
    SDL_snprintf(title, sizeof(title), "%s%s — Note Lab", ce_document_is_modified(a->doc) ? "* " : "", a->active ? a->active : "C Editor");
    SDL_SetWindowTitle(a->window, title);
#ifdef __EMSCRIPTEN__
    EM_ASM({ Module.editorUnsaved = !!$0; }, ce_document_is_modified(a->doc) || a->persistence_error || a->library_unsaved);
#endif
}
static bool refresh_files(App *a) {
    NoteList next = {0};
    if (note_store_list(&a->store, &next) != NOTE_OK) { message(a, a->store.error); return false; }
    note_store_list_free(&a->files);
    a->files = next;
    return true;
}
static void sync_view(App *a) {
    a->rich_dirty = true;
    platform_web_input_cancel();
    const char *text = ce_document_text(a->doc);
    EditBox_SetText(a->edit, text);
    if ((size_t)a->edit->cursor > strlen(text)) a->edit->cursor = (int)strlen(text);
    while (a->edit->cursor > 0 && ((unsigned char)text[a->edit->cursor] & 0xc0) == 0x80) --a->edit->cursor;
    a->edit->highlight1 = a->edit->highlight2 = -1;
    a->edit->composition_length = 0;
    a->edit->composition_cursor_length = 0;
    update_title(a);
}
static void persistence_complete(bool success, const char *error, void *userdata) {
    App *a = userdata;
    a->persistence_error = !success;
    message(a, success ? (*a->library_warning ? a->library_warning : "已保存到持久存储") : error);
    if (success && a->saving_snapshot && a->active && a->saving_name && !strcmp(a->active, a->saving_name)) {
        char *current = ce_document_export_json(a->doc);
        if (current && !strcmp(current, a->saving_snapshot)) ce_document_mark_saved(a->doc);
        free(current);
    }
    free(a->saving_name); free(a->saving_snapshot);
    a->saving_name = a->saving_snapshot = NULL;
    a->saving = false;
    update_title(a);
}
static void structure_persisted(bool success, const char *error, void *userdata) {
    App *a = userdata;
    a->persistence_error = !success;
    if (!success) message(a, error);
    update_title(a);
}
static bool save_document(App *a) {
    if (a->saving) { message(a, "正在保存，请稍候"); return false; }
    if (!a->active || !ce_document_is_modified(a->doc)) {
        if (a->persistence_error) platform_persist(structure_persisted, a);
        return !platform_persist_pending() && !a->persistence_error;
    }
    /* Preedit text belongs to the IME, not to the saved document. */
    char *data = SDL_strcasecmp(strrchr(a->active, '.'), ".json") == 0
        ? ce_document_export_json(a->doc) : ce_document_export_html(a->doc);
    if (!data) { message(a, ce_document_error(a->doc)); return false; }
    char *snapshot = ce_document_export_json(a->doc);
    char *name = copy_string(a->active);
    if (!snapshot || !name) { free(data); free(snapshot); free(name); message(a, "内存不足，未保存"); return false; }
    NoteResult result = note_store_save(&a->store, a->active, data, strlen(data));
    free(data);
    if (result != NOTE_OK) { free(snapshot); free(name); message(a, a->store.error); return false; }
    a->saving_name = name; a->saving_snapshot = snapshot; a->saving = true;
    message(a, "正在保存到持久存储…");
    platform_persist(persistence_complete, a);
    return !a->saving && !a->persistence_error;
}
static void open_note(App *a, const char *name) {
    if (!save_document(a)) return;
    EditBox_CancelComposition(a->edit);
    EditBox_SetFocus(a->edit, false);
    char *data = NULL;
    size_t size = 0;
    if (note_store_read(&a->store, name, &data, &size) != NOTE_OK) { message(a, a->store.error); return; }
    if (size > INT_MAX || memchr(data, '\0', size)) {
        message(a, "文件过大或包含二进制内容，未打开。"); note_store_free(data); return;
    }
    char *next_name = copy_string(name);
    if (!next_name) { note_store_free(data); message(a, "内存不足"); return; }
    char error[256];
    bool ok = ce_document_import(a->doc, data, CE_FORMAT_AUTO, error, sizeof(error));
    note_store_free(data);
    if (!ok) { free(next_name); message(a, error); return; }
    free(a->active); a->active = next_name;
    ce_document_mark_saved(a->doc);
    a->scroll = 0;
    a->edit->cursor = 0;
    a->edit->highlight1 = a->edit->highlight2 = -1;
    sync_view(a);
    EditBox_SetFocus(a->edit, true);
    if (a->width < 700) a->sidebar = false;
    message(a, *a->library_warning ? a->library_warning : "已打开 · 自动保存开启");
}
static void get_selection(App *a, size_t *start, size_t *end) {
    if (a->edit->highlight1 >= 0 && a->edit->highlight2 >= 0) {
        *start = (size_t)SDL_min(a->edit->highlight1, a->edit->highlight2);
        *end = (size_t)SDL_max(a->edit->highlight1, a->edit->highlight2);
    } else { *start = *end = (size_t)a->edit->cursor; }
}
static void show_prompt(App *a, Prompt kind) {
    if (kind < PROMPT_LIBRARY_SEARCH) get_selection(a, &a->selection_start, &a->selection_end);
    a->prompt_mode = kind;
    a->prompt_scroll=0;
    EditBox_SetFocus(a->edit, false);
    /* Cancel uncommitted input when changing input controls. */
    SDL_ClearComposition(a->window);
    if (kind < PROMPT_LIBRARY_SEARCH) sync_view(a);
    EditBox_SetText(a->prompt, "");
    a->prompt->cursor = 0;
    a->prompt->highlight1 = a->prompt->highlight2 = -1;
    a->prompt->composition_length = 0;
    EditBox_SetFocus(a->prompt, kind != PROMPT_DELETE);
}
static void close_prompt(App *a) {
    EditBox_SetFocus(a->prompt, false);
    SDL_ClearComposition(a->window);
    a->prompt_mode = PROMPT_NONE;
    EditBox_SetFocus(a->edit, a->active != NULL && !a->library_open);
}
static void mark_changed(App *a) { a->rich_dirty = true; a->edited_at = SDL_GetTicks(); update_title(a); }
static void accept_prompt(App *a) {
    if (a->prompt->composition_length) { message(a, "请先完成输入法选词"); return; }
    const char *value = a->prompt->text->text ? a->prompt->text->text : "";
    if (a->prompt_mode >= PROMPT_LIBRARY_SEARCH) {
        if (library_prompt_accept(a, value)) close_prompt(a);
        return;
    }
    if (a->prompt_mode == PROMPT_NEW) {
        char *name = NULL;
        if (note_store_normalize_name(value, &name) != NOTE_OK) { message(a, "请输入有效的笔记文件名"); return; }
        if (!save_document(a)) { note_store_free(name); return; }
        const char *initial = SDL_strcasecmp(strrchr(name, '.'), ".json") == 0
            ? "{\"type\":\"doc\",\"content\":[{\"type\":\"paragraph\"}]}" : "<p></p>";
        if (note_store_create(&a->store, name, initial, strlen(initial)) != NOTE_OK) {
            message(a, a->store.error); note_store_free(name); return;
        }
        close_prompt(a); refresh_files(a); open_note(a, name); note_store_free(name); platform_persist(structure_persisted,a);
        return;
    }
    if (a->prompt_mode == PROMPT_FIND) {
        char *keyword = copy_string(value);
        if (!keyword) { message(a, "内存不足"); return; }
        free(a->search_keyword); a->search_keyword = keyword;
        size_t start, end;
        if (ce_document_find(a->doc, value, (size_t)a->edit->cursor, false, &start, &end)) {
            a->edit->highlight1 = (int)start; a->edit->highlight2 = (int)end; a->edit->cursor = (int)end;
            message(a, "已选中匹配文本");
        } else message(a, "没有找到匹配文本");
    } else if (a->prompt_mode == PROMPT_LINK) {
        if (!ce_document_style(a->doc, a->selection_start, a->selection_end, CE_MARK_LINK, value, *value != '\0'))
            message(a, ce_document_error(a->doc));
        else mark_changed(a);
    } else if (a->prompt_mode == PROMPT_DELETE && a->active) {
        if (note_store_delete(&a->store, a->active) != NOTE_OK) { message(a, a->store.error); return; }
        free(a->active); a->active = NULL;
        char error[256];
        ce_document_import(a->doc, "", CE_FORMAT_TEXT, error, sizeof(error));
        ce_document_mark_saved(a->doc); sync_view(a); refresh_files(a); platform_persist(structure_persisted,a); message(a, "已删除笔记");
    }
    close_prompt(a);
    reveal_cursor(a);
}
/* Browser clipboard access must use the DOM clipboard event. Return an owned
 * UTF-8 snapshot and delete only if the selection still matches that snapshot. */
static EditBox *clipboard_edit(App *a) {
    EditBox *edit = a->prompt_mode != PROMPT_NONE ? a->prompt : a->edit;
    if (!edit->has_focus || edit->composition_length || edit->composition_snapshot) return NULL;
    return edit;
}
static char *browser_clipboard_read(void *state) {
    App *a = state; EditBox *edit = clipboard_edit(a);
    if (!edit || edit->highlight1 < 0 || edit->highlight2 < 0) return NULL;
    size_t from = (size_t)SDL_min(edit->highlight1, edit->highlight2);
    size_t to = (size_t)SDL_max(edit->highlight1, edit->highlight2);
    const char *text = edit->text->text ? edit->text->text : "";
    if (from >= to || to > strlen(text) || ((unsigned char)text[from] & 0xc0) == 0x80 ||
        ((unsigned char)text[to] & 0xc0) == 0x80) return NULL;
    char *copy = malloc(to - from + 1);
    if (copy) { memcpy(copy, text + from, to - from); copy[to - from] = '\0'; }
    return copy;
}
static bool browser_clipboard_cut(const char *expected, void *state) {
    App *a = state; EditBox *edit = clipboard_edit(a);
    char *current = browser_clipboard_read(state);
    bool matches = current && expected && !strcmp(current, expected);
    free(current);
    if (!matches || !edit) return false;
    if (edit == a->prompt) return EditBox_DeleteHighlight(edit);
    size_t from, to; get_selection(a, &from, &to);
    if (!ce_document_delete(a->doc, from, to)) { message(a, ce_document_error(a->doc)); return false; }
    edit->cursor = (int)from; sync_view(a); mark_changed(a); reveal_cursor(a);
    return true;
}
static void browser_clipboard_error(void *state) {
    message(state, "无法访问剪贴板，原文保持不变");
}
static void format_selection(App *a, CeMark mark, const char *value) {
    size_t start, end; get_selection(a, &start, &end);
    if (start == end) { message(a, "请先选择要设置格式的文字"); return; }
    CeStyle style = ce_document_style_at(a->doc, start);
    if (!ce_document_style(a->doc, start, end, mark, value, !(style.marks & mark)))
        message(a, ce_document_error(a->doc));
    else mark_changed(a);
}
static void action(App *a, Action which) {
    if (which == ACT_MORE) { ++a->toolbar_page; return; }
    if (which == ACT_SIDEBAR) { a->sidebar = !a->sidebar; a->sidebar_manual = true; return; }
    if (which == ACT_NEW) { show_prompt(a, PROMPT_NEW); return; }
    if (which == ACT_REFRESH) { if (refresh_files(a)) message(a, "笔记列表已刷新"); return; }
    if (which == ACT_THEME) { a->dark = !a->dark; return; }
    if (which == ACT_TYPEWRITER) { a->typewriter = !a->typewriter; return; }
    if (!a->active) { message(a, "请先新建或打开笔记"); return; }
    switch (which) {
        case ACT_SAVE: save_document(a); break;
        case ACT_UNDO: if (ce_document_undo(a->doc)) { sync_view(a); mark_changed(a); } break;
        case ACT_REDO: if (ce_document_redo(a->doc)) { sync_view(a); mark_changed(a); } break;
        case ACT_BOLD: format_selection(a, CE_MARK_BOLD, NULL); break;
        case ACT_ITALIC: format_selection(a, CE_MARK_ITALIC, NULL); break;
        case ACT_UNDERLINE: format_selection(a, CE_MARK_UNDERLINE, NULL); break;
        case ACT_HIGHLIGHT: format_selection(a, CE_MARK_HIGHLIGHT, "#fff19a"); break;
        case ACT_RED: format_selection(a, CE_MARK_COLOR, "#c74343"); break;
        case ACT_LINK: show_prompt(a, PROMPT_LINK); break;
        case ACT_FIND: show_prompt(a, PROMPT_FIND); break;
        case ACT_FIND_PREVIOUS: case ACT_FIND_NEXT: {
            if (!a->search_keyword || !*a->search_keyword) { show_prompt(a, PROMPT_FIND); break; }
            size_t start, end, from, selection_end; get_selection(a, &from, &selection_end);
            if (which == ACT_FIND_NEXT) from = selection_end;
            if (ce_document_find(a->doc, a->search_keyword, from, which == ACT_FIND_PREVIOUS, &start, &end)) {
                a->edit->highlight1 = (int)start; a->edit->highlight2 = (int)end; a->edit->cursor = (int)end;
                reveal_cursor(a); message(a, "已选中匹配文本");
            } else message(a, "没有找到匹配文本");
            break;
        }
        case ACT_CLEAR: {
            size_t start, end; get_selection(a, &start, &end);
            if (start == end) { message(a, "请先选择要清除格式的文字"); break; }
            if (ce_document_clear_formatting(a->doc, start, end)) mark_changed(a);
            else message(a, ce_document_error(a->doc));
            break;
        }
        case ACT_LEFT: case ACT_CENTER: case ACT_RIGHT: case ACT_JUSTIFY: {
            const char *alignment = which == ACT_LEFT ? "left" : which == ACT_CENTER ? "center" : which == ACT_RIGHT ? "right" : "justify";
            size_t start, end; get_selection(a, &start, &end);
            if (ce_document_set_alignment(a->doc, start, end, alignment)) mark_changed(a);
            else message(a, ce_document_error(a->doc));
            break;
        }
        case ACT_MOVE_UP: case ACT_MOVE_DOWN: {
            size_t count = ce_document_block_count(a->doc), index = 0;
            CeBlockInfo block;
            for (; index < count; ++index) {
                if (ce_document_block_info(a->doc, index, &block) &&
                    (size_t)a->edit->cursor >= block.start && (size_t)a->edit->cursor <= block.end) break;
            }
            if (index == count || (which == ACT_MOVE_UP && !index) || (which == ACT_MOVE_DOWN && index + 1 == count)) break;
            size_t target = which == ACT_MOVE_UP ? index - 1 : index + 1;
            size_t relative = (size_t)a->edit->cursor-block.start, offset;
            if (ce_document_move_block_ex(a->doc, index, target, &offset)) {
                sync_view(a);
                a->edit->cursor = (int)(offset+relative);
                mark_changed(a); reveal_cursor(a);
            } else message(a, ce_document_error(a->doc));
            break;
        }
        case ACT_DELETE: show_prompt(a, PROMPT_DELETE); break;
        case ACT_BULLETS: case ACT_NUMBERS: case ACT_HEADING: case ACT_PARAGRAPH: {
            size_t start, end; get_selection(a, &start, &end);
            CeBlockKind kind = which == ACT_BULLETS ? CE_BLOCK_BULLET : which == ACT_NUMBERS ? CE_BLOCK_ORDERED : which == ACT_PARAGRAPH ? CE_BLOCK_PARAGRAPH : CE_BLOCK_HEADING;
            if (ce_document_set_block(a->doc, start, end, kind, 2)) { sync_view(a); mark_changed(a); }
            else message(a, ce_document_error(a->doc));
            break;
        }
        case ACT_JSON: {
            if (a->export_job) { message(a, "正在导出，请先完成文件选择"); break; }
            char *json = ce_document_export_json(a->doc);
            if (!json) { message(a, ce_document_error(a->doc)); break; }
            const char *extension = strrchr(a->active, '.');
            size_t stem = extension ? (size_t)(extension - a->active) : strlen(a->active);
            char *filename = malloc(stem + sizeof(".json"));
            if (filename) {
                memcpy(filename, a->active, stem); memcpy(filename + stem, ".json", sizeof(".json"));
                a->export_job = platform_export_json(a->window, filename, json);
            }
            free(filename); free(json);
            message(a, a->export_job ? "正在导出 JSON 文档…" : "内存不足，未导出");
            break;
        }
        default: break;
    }
}

/* Reconcile one committed input event as one undoable replacement. Never copy
 * IME preedit into the persistent model; the EditBox owns that temporary text. */
static void reconcile_input(App *a) {
    if (a->edit->composition_length) return;
    const char *old = ce_document_text(a->doc);
    const char *next = a->edit->text->text ? a->edit->text->text : "";
    size_t old_len = strlen(old), new_len = strlen(next), prefix = 0;
    if (new_len > INT_MAX) { message(a, "文本过大"); sync_view(a); return; }
    while (prefix < old_len && prefix < new_len && old[prefix] == next[prefix]) ++prefix;
    if (prefix == old_len && prefix == new_len) return;
    while (prefix && ((unsigned char)old[prefix] & 0xc0) == 0x80) --prefix;
    size_t old_end = old_len, new_end = new_len;
    while (old_end > prefix && new_end > prefix && old[old_end-1] == next[new_end-1]) { --old_end; --new_end; }
    while (old_end < old_len && ((unsigned char)old[old_end] & 0xc0) == 0x80) { ++old_end; ++new_end; }
    char *insert = malloc(new_end - prefix + 1);
    if (!insert) { message(a, "内存不足"); sync_view(a); return; }
    memcpy(insert, next + prefix, new_end - prefix); insert[new_end-prefix] = '\0';
    bool ok = ce_document_replace(a->doc, prefix, old_end, insert);
    free(insert);
    if (!ok) { message(a, ce_document_error(a->doc)); sync_view(a); }
    else {
        /* Structural editing (for example leaving an empty list item or typing
         * next to a citation) may normalize block separators. Keep the input
         * surface aligned with the model, preserving the caret relative to
         * the unchanged suffix when possible. */
        const char *canonical = ce_document_text(a->doc);
        if (strcmp(canonical, next)) {
            size_t length = strlen(canonical), common = 0, suffix = 0;
            size_t cursor = (size_t)SDL_max(0, a->edit->cursor);
            if (cursor > new_len) cursor = new_len;
            while (common < length && common < new_len && canonical[common] == next[common]) ++common;
            while (suffix < length - common && suffix < new_len - common &&
                   canonical[length - suffix - 1] == next[new_len - suffix - 1]) ++suffix;
            if (cursor > common) {
                if (cursor >= new_len - suffix) cursor = length - (new_len - cursor);
                else cursor = common + SDL_min(cursor - common, length - suffix - common);
            }
            sync_view(a);
            if (cursor > length) cursor = length;
            while (cursor && ((unsigned char)canonical[cursor] & 0xc0) == 0x80) --cursor;
            a->edit->cursor = (int)cursor;
        }
        mark_changed(a);
    }
}
/* The EditBox is the only owner of pending IME text. A preview document is
 * discarded after shaping and never enters autosave, history, or Zotero state.
 * If a structural edit normalizes offsets, use the complete plain layout until
 * commit rather than rendering one text while hit-testing another. */
static bool rich_prepare(void *userdata, EditBox *edit) {
    App *a = userdata;
    const char *input = edit->text->text ? edit->text->text : "";
    float width = edit->rect.w;
    if (!a->rich_dirty && a->rich_text && !strcmp(input,a->rich_text) &&
        a->rich_width == width && a->rich_dark == a->dark) return a->rich_valid;
    char *cache = copy_string(input);
    if (!cache) return false;
    free(a->rich_text); a->rich_text = cache;
    a->rich_width = width; a->rich_dark = a->dark; a->rich_dirty = false;
    const char *committed = ce_document_text(a->doc);
    CeDocument *preview = NULL;
    const CeDocument *model = a->doc;
    if (strcmp(committed,input)) {
        char *json = ce_document_export_json(a->doc), problem[256];
        preview = ce_document_new();
        bool ok = json && preview && ce_document_import(preview,json,CE_FORMAT_JSON,problem,sizeof problem);
        free(json);
        if (ok) {
            size_t old_len = strlen(committed), new_len = strlen(input), start = 0, old_end = old_len, new_end = new_len;
            while (start < old_len && start < new_len && committed[start] == input[start]) start++;
            while (start && ((unsigned char)committed[start] & 0xc0) == 0x80) start--;
            while (old_end > start && new_end > start && committed[old_end-1] == input[new_end-1]) { old_end--; new_end--; }
            while (old_end < old_len && ((unsigned char)committed[old_end] & 0xc0) == 0x80) { old_end++; new_end++; }
            char *insert = malloc(new_end-start+1);
            if (insert) {
                memcpy(insert,input+start,new_end-start); insert[new_end-start] = 0;
                ok = ce_document_replace(preview,start,old_end,insert) && !strcmp(ce_document_text(preview),input);
                free(insert);
            } else ok = false;
        }
        if (!ok) { ce_document_free(preview); a->rich_valid=false; return false; }
        model = preview;
    }
    CeRichTheme theme = ce_rich_theme_default(a->dark);
    a->rich_valid = ce_rich_layout_update(a->rich,model,width,&theme);
    ce_document_free(preview);
    return a->rich_valid;
}
static bool rich_caret(void *userdata,int offset,bool trailing,SDL_FRect *rect) {
    return offset >= 0 && ce_rich_layout_caret_affinity(((App *)userdata)->rich,(size_t)offset,trailing,rect);
}
static int rich_hit(void *userdata,float x,float y) {
    return (int)ce_rich_layout_hit_test(((App *)userdata)->rich,x,y);
}
static int rich_step(void *userdata,int offset,int direction) {
    return (int)ce_rich_layout_step(((App *)userdata)->rich,(size_t)SDL_max(0,offset),direction);
}
static void rich_selection(void *userdata,int start,int end,EditBoxRectCallback callback,void *context) {
    ce_rich_layout_selection_rects(((App *)userdata)->rich,(size_t)SDL_max(0,start),(size_t)SDL_max(0,end),callback,context);
}
static bool rich_draw(void *userdata,float x,float y,int start,int end) {
    App *a=userdata;
    return ce_rich_layout_draw(a->rich,x,y,&a->editor_area,(size_t)SDL_max(0,start),(size_t)SDL_max(0,end));
}
static const EditBoxLayout rich_hooks = {rich_prepare,rich_caret,rich_hit,rich_step,rich_selection,rich_draw};
static const char *json_string(const cJSON *object, const char *key) {
    const cJSON *value=cJSON_GetObjectItemCaseSensitive(object,key);
    return cJSON_IsString(value)?value->valuestring:"";
}
static void citation_context_clear(App *a) {
    free(a->citation_id); free(a->citation_title); free(a->citation_page); free(a->citation_note);
    free(a->citation_item_uri); free(a->citation_pdf_uri);
    a->citation_id=a->citation_title=a->citation_page=a->citation_note=NULL;
    a->citation_item_uri=a->citation_pdf_uri=NULL;
}
static bool citation_context_capture(App *a) {
    cJSON *node=NULL;
    size_t start,end; get_selection(a,&start,&end); (void)end;
    for(size_t i=0;i<ce_document_block_count(a->doc);i++) {
        CeBlockInfo block;
        if(ce_document_block_info(a->doc,i,&block) && block.kind==CE_BLOCK_CITATION && start>=block.start && start<=block.end) {
            char *json=ce_document_block_json(a->doc,i); node=json?cJSON_Parse(json):NULL; free(json); break;
        }
    }
    if(!node) { citation_context_clear(a); return false; }
    cJSON *attrs=cJSON_GetObjectItemCaseSensitive(node,"attrs");
    const char *keys[]={"citationId","title","page","note","itemUri","pdfUri"};
    char *owned[6]={0}; bool ok=true;
    for(size_t i=0;i<SDL_arraysize(owned);i++) if(!(owned[i]=copy_string(json_string(attrs,keys[i])))) ok=false;
    cJSON_Delete(node);
    if(!ok) { for(size_t i=0;i<SDL_arraysize(owned);i++) free(owned[i]); message(a,"内存不足"); return false; }
    citation_context_clear(a);
    a->citation_id=owned[0]; a->citation_title=owned[1]; a->citation_page=owned[2]; a->citation_note=owned[3];
    a->citation_item_uri=owned[4]; a->citation_pdf_uri=owned[5]; return true;
}
static bool citation_position(App *a,const char *id,size_t *end) {
    if(!id || !*id) return false;
    for(size_t i=0;i<ce_document_block_count(a->doc);i++) {
        CeBlockInfo block;
        if(!ce_document_block_info(a->doc,i,&block) || block.kind!=CE_BLOCK_CITATION) continue;
        char *json=ce_document_block_json(a->doc,i); cJSON *node=json?cJSON_Parse(json):NULL; free(json);
        bool match=node && !strcmp(json_string(cJSON_GetObjectItemCaseSensitive(node,"attrs"),"citationId"),id);
        cJSON_Delete(node); if(match) { *end=block.end; return true; }
    }
    return false;
}
static void library_show(App *a) {
    EditBox_CancelComposition(a->edit); SDL_ClearComposition(a->window); platform_web_input_cancel();
    size_t end; get_selection(a,&a->library_insert_offset,&end);
    a->library_citation_mode=a->active && citation_context_capture(a);
    if(!a->active) citation_context_clear(a);
    a->library_open=true; a->library_dragging=false;
    a->library_outer_scroll=0;
    EditBox_SetFocus(a->edit,false); a->rich_dirty=true;
}
static void library_close(App *a) {
    a->library_open=false; a->library_dragging=false;
    EditBox_SetFocus(a->edit,a->active!=NULL);
}
static void library_prompt(App *a,Prompt kind) {
    const char *value="";
    if(kind==PROMPT_LIBRARY_SEARCH) value=a->library.query?a->library.query:"";
    else if(kind==PROMPT_CITATION_PAGE) value=a->library_citation_mode?a->citation_page:a->insert_page;
    else if(kind==PROMPT_CITATION_NOTE) value=a->library_citation_mode?a->citation_note:a->insert_note;
    show_prompt(a,kind); EditBox_SetText(a->prompt,value?value:"");
    a->prompt->cursor=(int)strlen(value?value:"");
    EditBox_SelectAll(a->prompt);
}
static bool library_prompt_accept(App *a,const char *value) {
    if(a->prompt_mode==PROMPT_LIBRARY_SEARCH) {
        if(!ce_library_panel_query(&a->library,value)) { message(a,"内存不足，搜索条件保持不变"); return false; }
    } else {
        char **target=a->prompt_mode==PROMPT_CITATION_PAGE?
            (a->library_citation_mode?&a->citation_page:&a->insert_page):
            (a->library_citation_mode?&a->citation_note:&a->insert_note);
        char *owned=copy_string(value);
        if(!owned) { message(a,"内存不足"); return false; }
        free(*target); *target=owned;
    }
    return true;
}
static const char *approximate_style(App *a) {
    return a->citation_style==CE_ZOTERO_APPROXIMATE_IEEE?"IEEE":a->citation_style==CE_ZOTERO_APPROXIMATE_GBT_7714?"GB/T 7714":"APA";
}
/* Footer synchronization changes offsets if an old footer was in the middle
 * of the document. Non-footer block order is stable, including duplicate-ID
 * citations that receive fresh IDs during synchronization. */
static bool library_anchor(App *a,size_t *ordinal,size_t *relative) {
    size_t count=0;
    for(size_t i=0;i<ce_document_block_count(a->doc);i++) {
        CeBlockInfo block; if(!ce_document_block_info(a->doc,i,&block)) continue;
        if(block.kind==CE_BLOCK_ENDNOTES) continue;
        if((size_t)a->edit->cursor>=block.start && (size_t)a->edit->cursor<=block.end) {
            *ordinal=count; *relative=(size_t)a->edit->cursor-block.start; return true;
        }
        count++;
    }
    return false;
}
static void library_restore_anchor(App *a,size_t ordinal,size_t relative) {
    size_t count=0;
    for(size_t i=0;i<ce_document_block_count(a->doc);i++) {
        CeBlockInfo block; if(!ce_document_block_info(a->doc,i,&block) || block.kind==CE_BLOCK_ENDNOTES) continue;
        if(count++==ordinal) { a->edit->cursor=(int)(block.start+SDL_min(relative,block.end-block.start)); return; }
    }
}
static void library_action(App *a,LibraryAction action) {
    if(action==LIB_CLOSE) { library_close(a); return; }
    if(action==LIB_VIEW) {
        a->library_outer_scroll=a->library_outer_scroll>0?0:SDL_max(0,a->library_content_height-a->library_area.h);
        return;
    }
    if(action==LIB_IMPORT) {
        if(a->import_job || a->pending_note_json || a->library_cache_saving) { message(a,"正在导入或保存，请稍候"); return; }
        a->import_kind=PLATFORM_IMPORT_ZOTERO;
        a->import_job=platform_import_open(a->window,PLATFORM_IMPORT_ZOTERO);
        message(a,a->import_job?"选择 Zotero 的 CSL JSON 或 SQLite 数据库":"无法打开文件选择器"); return;
    }
    if(action==LIB_ITEMS) { a->library_citation_mode=false; return; }
    if(action==LIB_CURRENT) {
        a->library_citation_mode=true;
        if(!a->citation_title) message(a,"请先关闭文献面板，在正文中点击引用块，再打开文献面板");
        return;
    }
    if(action==LIB_SEARCH) { library_prompt(a,PROMPT_LIBRARY_SEARCH); return; }
    if(action==LIB_COLLECTIONS) { if(!ce_library_panel_collections(&a->library,!a->library.collections_view)) message(a,"无法更新分类列表"); return; }
    if(action==LIB_ALL) { if(!ce_library_panel_collection(&a->library,SIZE_MAX)) message(a,"无法更新条目列表"); return; }
    if(action==LIB_DESCENDANTS) { if(!ce_library_panel_descendants(&a->library,!a->library.include_descendants)) message(a,"无法更新子分类"); return; }
    if(action==LIB_PAGE || action==LIB_NOTE) { library_prompt(a,action==LIB_PAGE?PROMPT_CITATION_PAGE:PROMPT_CITATION_NOTE); return; }
    if(action==LIB_STYLE) { a->citation_style=(CeZoteroApproximateStyle)((a->citation_style+1)%3); return; }
    if(action==LIB_RETRY) {
        if(a->library_cache_saving) message(a,"正在保存文献库，请稍候");
        else if(a->library_restore_failed) library_cache_load(a,false);
        else library_cache_save(a);
        return;
    }
    if(action==LIB_OPEN_ITEM || action==LIB_OPEN_PDF) {
        const CeZoteroItem *item=ce_library_panel_selected(&a->library);
        const char *uri=a->library_citation_mode?(action==LIB_OPEN_ITEM?a->citation_item_uri:a->citation_pdf_uri):
            item?(action==LIB_OPEN_ITEM?item->item_uri:item->pdf_uri):NULL;
        if(!ce_library_uri_allowed(uri)) { message(a,"此条目没有可打开的 Zotero 或 http(s) 链接"); return; }
        if(!SDL_OpenURL(uri)) message(a,SDL_GetError()); else message(a,"已请求打开条目链接");
        return;
    }
    if(!a->active) { message(a,"请先新建或打开笔记，再插入引用或同步尾注"); return; }
    char error[256];
    if(action==LIB_SYNC) {
        size_t ordinal=0,relative=0; bool anchored=library_anchor(a,&ordinal,&relative);
        CeCitationSyncResult result;
        if(!ce_citations_sync_approximate(a->doc,a->citation_style,&result,error,sizeof error)) { message(a,error); return; }
        sync_view(a); mark_changed(a);
        if(anchored) library_restore_anchor(a,ordinal,relative);
        a->library_insert_offset=(size_t)a->edit->cursor;
        citation_context_capture(a);
        char status[256]; SDL_snprintf(status,sizeof status,"已同步 %zu 条引用 / %zu 条文献 · %s 近似格式",result.citation_count,result.reference_count,approximate_style(a));
        message(a,status); return;
    }
    if(action==LIB_INSERT) {
        if(a->library_citation_mode) {
            if(!a->citation_id || !*a->citation_id) { message(a,"未选中有效引用；缺少 ID 的旧引用可先同步尾注修复"); return; }
            if(!ce_citation_update(a->doc,a->citation_id,a->citation_page,a->citation_note,error,sizeof error)) { message(a,error); return; }
            sync_view(a); mark_changed(a); library_close(a); reveal_cursor(a); message(a,"引用页码和摘录已更新，可撤销");
        } else {
            const CeZoteroItem *item=ce_library_panel_selected(&a->library);
            if(!item) { message(a,"请先在文献列表中选择一条文献"); return; }
            char *id=NULL;
            if(!ce_citation_insert(a->doc,a->library_insert_offset,item,a->insert_page,a->insert_note,&id,error,sizeof error)) { message(a,error); return; }
            sync_view(a); size_t end;
            if(citation_position(a,id,&end)) a->edit->cursor=(int)end;
            free(id); mark_changed(a); library_close(a); reveal_cursor(a);
            message(a,"已插入引用，可撤销 · 在文献面板同步近似格式尾注");
        }
    }
}
static void document_import_clear(App *a) {
    free(a->pending_note_json); free(a->pending_note_stem);
    a->pending_note_json=a->pending_note_stem=NULL;
}
static void document_import_begin(App *a) {
    if(a->import_job || a->pending_note_json) { message(a,"正在导入，请先完成当前文件选择"); return; }
    EditBox_CancelComposition(a->edit); SDL_ClearComposition(a->window); platform_web_input_cancel(); a->rich_dirty=true;
    a->import_kind=PLATFORM_IMPORT_DOCUMENT;
    a->import_job=platform_import_open(a->window,PLATFORM_IMPORT_DOCUMENT);
    message(a,a->import_job?"选择旧笔记 HTML、JSON 或文本文件，将创建新副本":"无法打开文件选择器");
}
static char *document_import_stem(const char *filename) {
    const char *base=filename && *filename?filename:"导入笔记";
    for(const char *p=base;*p;p++) if(*p=='/' || *p=='\\') base=p+1;
    const char *dot=strrchr(base,'.');
    size_t length=dot && dot>base?(size_t)(dot-base):strlen(base);
    if(length>200) { length=200; while(length && ((unsigned char)base[length]&0xc0)==0x80) length--; }
    char *stem=malloc(length+1); if(!stem) return NULL;
    memcpy(stem,base,length); stem[length]=0;
    char candidate[256]; SDL_snprintf(candidate,sizeof candidate,"%s.json",stem);
    if(!length || *stem=='.' || !note_store_valid_name(candidate)) { free(stem); stem=copy_string("导入笔记"); }
    return stem;
}
static bool document_import_prepare(App *a,const PlatformImportFile *file) {
    if(!file->data || file->size>INT_MAX || memchr(file->data,0,file->size)) {
        message(a,"旧笔记包含二进制内容或过大，未导入"); return false;
    }
    const char *extension=file->filename?strrchr(file->filename,'.'):NULL;
    CeFormat format=CE_FORMAT_AUTO;
    if(extension && !SDL_strcasecmp(extension,".txt")) format=CE_FORMAT_TEXT;
    else if(extension && !SDL_strcasecmp(extension,".json")) format=CE_FORMAT_JSON;
    else if(extension && (!SDL_strcasecmp(extension,".html") || !SDL_strcasecmp(extension,".htm"))) format=CE_FORMAT_HTML;
    CeDocument *candidate=ce_document_new(); char error[256]="内存不足";
    bool ok=candidate && ce_document_import(candidate,(const char *)file->data,format,error,sizeof error);
    char *json=ok?ce_document_export_json(candidate):NULL;
    char *stem=ok?document_import_stem(file->filename):NULL;
    ce_document_free(candidate);
    if(!ok || !json || !stem) { free(json); free(stem); message(a,ok?"内存不足，未导入旧笔记":error); return false; }
    document_import_clear(a); a->pending_note_json=json; a->pending_note_stem=stem;
    message(a,"旧笔记已验证，正在保存当前笔记后创建新副本…"); return true;
}
static void document_import_saved(bool success,const char *error,void *userdata) {
    App *a=userdata; structure_persisted(success,error,a);
    if(success) message(a,*a->library_warning?a->library_warning:"已导入并持久保存新笔记副本 · 原文件未改动");
}
static void document_import_finish(App *a) {
    if(!a->pending_note_json || a->saving || platform_persist_pending() || a->library_open || a->prompt_mode!=PROMPT_NONE) return;
    if(a->persistence_error) {
        document_import_clear(a); message(a,"当前笔记尚未持久保存，旧笔记未导入；请先重试保存"); return;
    }
    if(!save_document(a)) {
        if(a->saving || platform_persist_pending()) return;
        document_import_clear(a); return;
    }
    char name[256]; NoteResult result=NOTE_EXISTS;
    for(unsigned number=1;number<=10000;number++) {
        if(number==1) SDL_snprintf(name,sizeof name,"%s.json",a->pending_note_stem);
        else SDL_snprintf(name,sizeof name,"%s (%u).json",a->pending_note_stem,number);
        result=note_store_create(&a->store,name,a->pending_note_json,strlen(a->pending_note_json));
        if(result!=NOTE_EXISTS) break;
    }
    document_import_clear(a);
    if(result!=NOTE_OK) { message(a,a->store.error); return; }
    refresh_files(a); open_note(a,name);
    message(a,"已创建新笔记副本，正在保存到持久存储…");
    platform_persist(document_import_saved,a);
}

static void library_cache_problem(App *a,bool restoring,const char *detail) {
    a->library_restore_failed=restoring;
    SDL_snprintf(a->library_warning,sizeof a->library_warning,"%s：%s",restoring?"文献库缓存未恢复":"文献库尚未持久保存",detail && *detail?detail:"请重试");
    message(a,a->library_warning); update_title(a);
}
static bool library_cache_store_open(App *a) {
    if(a->library_store_ready) return true;
    char *path=NULL;
    if(!a->notes_root || SDL_asprintf(&path,"%s/.notelab-library",a->notes_root)<0) {
        SDL_strlcpy(a->library_store.error,"内存不足",sizeof a->library_store.error); return false;
    }
    NoteResult result=note_store_init(&a->library_store,path); SDL_free(path);
    a->library_store_ready=result==NOTE_OK; return a->library_store_ready;
}
static void library_cache_saved(bool success,const char *error,void *userdata) {
    App *a=userdata; a->library_cache_saving=false;
    if(success) {
        a->library_unsaved=false; a->library_warning[0]=0; a->library_restore_failed=false;
        char summary[256]; SDL_snprintf(summary,sizeof summary,"文献库已持久保存 · %zu 条文献、%zu 个分类",a->library.library.item_count,a->library.library.collection_count);
        message(a,summary); update_title(a);
    } else library_cache_problem(a,false,error);
}
static bool library_cache_save(App *a) {
    if(a->library_cache_saving) return false;
    if(!library_cache_store_open(a)) { library_cache_problem(a,false,a->library_store.error); return false; }
    char error[256]; char *json=ce_zotero_cache_export(&a->library.library,error,sizeof error);
    if(!json) { library_cache_problem(a,false,error); return false; }
    NoteResult result=note_store_save(&a->library_store,"library.json",json,strlen(json));
    if(result==NOTE_NOT_FOUND) result=note_store_create(&a->library_store,"library.json",json,strlen(json));
    free(json);
    if(result!=NOTE_OK) { library_cache_problem(a,false,a->library_store.error); return false; }
    a->library_cache_saving=true;
    message(a,"文献库已导入，正在保存到持久存储…");
    platform_persist(library_cache_saved,a);
    return !a->library_cache_saving && !a->library_unsaved;
}
static bool library_cache_load(App *a,bool startup) {
    if(!library_cache_store_open(a)) { library_cache_problem(a,true,a->library_store.error); return false; }
    char *json=NULL; size_t length=0;
    NoteResult result=note_store_read(&a->library_store,"library.json",&json,&length);
    if(startup && result==NOTE_NOT_FOUND) return true;
    if(result!=NOTE_OK) { library_cache_problem(a,true,a->library_store.error); return false; }
    CeZoteroLibrary restored={0}; char error[256]="缓存包含二进制内容";
    bool ok=!memchr(json,0,length) && ce_zotero_cache_import(json,&restored,error,sizeof error);
    note_store_free(json);
    if(ok && !ce_library_panel_replace(&a->library,&restored)) { SDL_strlcpy(error,"内存不足，文献库保持不变",sizeof error); ok=false; }
    ce_zotero_library_free(&restored);
    if(!ok) { library_cache_problem(a,true,error); return false; }
    a->library_unsaved=false; a->library_warning[0]=0; a->library_restore_failed=false;
    char summary[256]; SDL_snprintf(summary,sizeof summary,"已恢复文献库 · %zu 条文献、%zu 个分类",a->library.library.item_count,a->library.library.collection_count);
    message(a,summary); update_title(a); return true;
}

static void library_import_poll(App *a) {
    if(!a->import_job) return;
    const char *status=NULL;
    PlatformImportStatus result=platform_import_status(a->import_job,&status);
    if(result==PLATFORM_IMPORT_PENDING) return;
    if(result!=PLATFORM_IMPORT_DONE) message(a,status?status:"导入已取消");
    else {
        PlatformImportFile file={0}; CeZoteroLibrary imported={0}; char error[256]="无法读取所选文件";
        bool ok=platform_import_take_result(a->import_job,&file);
        if(ok && a->import_kind==PLATFORM_IMPORT_DOCUMENT) {
            document_import_prepare(a,&file); platform_import_file_free(&file);
            platform_import_free(a->import_job); a->import_job=NULL; return;
        }
        if(ok && file.kind==PLATFORM_IMPORT_SQLITE) {
            ok=file.native_path?ce_zotero_load_sqlite(file.native_path,&imported,error,sizeof error):
                ce_zotero_load_sqlite_bytes(file.data,file.size,&imported,error,sizeof error);
        } else if(ok) {
            if(!file.data || memchr(file.data,0,file.size)) { SDL_strlcpy(error,"CSL JSON 包含二进制内容，未导入",sizeof error); ok=false; }
            else ok=ce_zotero_import_csl((const char *)file.data,&imported,error,sizeof error);
        }
        if(ok && !ce_library_panel_replace(&a->library,&imported)) { SDL_strlcpy(error,"内存不足，原文献库保持不变",sizeof error); ok=false; }
        if(ok) {
            a->library_citation_mode=false;
            a->library_dragging=false;
            a->library_unsaved=true; a->library_restore_failed=false; a->library_warning[0]=0;
            update_title(a); library_cache_save(a);
        } else message(a,error);
        ce_zotero_library_free(&imported); platform_import_file_free(&file);
    }
    platform_import_free(a->import_job); a->import_job=NULL;
}
static void library_layout(App *a) {
    a->library_button=(SDL_FRect){a->width-94.0f,3,82,42};
    float width=SDL_min((float)a->width,480.0f),left=a->width-width,height=a->height-30.0f;
    a->library_area=(SDL_FRect){left,0,width,height};
    a->library_content_height=SDL_max(height,600);
    a->library_outer_scroll=SDL_clamp(a->library_outer_scroll,0,a->library_content_height-height);
    for(size_t i=0;i<LIB_BUTTON_COUNT;i++) a->library_buttons[i]=(SDL_FRect){0};
    float x=left+12,w=width-24;
    a->library_buttons[LIB_CLOSE]=(SDL_FRect){left+width-68,8,56,40};
    if(height<600) a->library_buttons[LIB_VIEW]=(SDL_FRect){left+width-168,8,92,40};
    a->library_buttons[LIB_IMPORT]=(SDL_FRect){x,58,w/3-4,42};
    a->library_buttons[LIB_ITEMS]=(SDL_FRect){x+w/3+2,58,w/3-4,42};
    a->library_buttons[LIB_CURRENT]=(SDL_FRect){x+2*w/3+4,58,w/3-4,42};
    if(!a->library_citation_mode) {
        a->library_buttons[LIB_SEARCH]=(SDL_FRect){x,110,w,42};
        a->library_buttons[LIB_COLLECTIONS]=(SDL_FRect){x,160,w/3-4,42};
        a->library_buttons[LIB_ALL]=(SDL_FRect){x+w/3+2,160,w/3-4,42};
        a->library_buttons[LIB_DESCENDANTS]=(SDL_FRect){x+2*w/3+4,160,w/3-4,42};
    }
    float bottom=a->library_content_height-220;
    a->library_rows=(SDL_FRect){x,232,w,SDL_max(32,bottom-244)};
    a->library_buttons[LIB_PAGE]=(SDL_FRect){x,bottom+45,w/2-3,42};
    a->library_buttons[LIB_NOTE]=(SDL_FRect){x+w/2+3,bottom+45,w/2-3,42};
    a->library_buttons[LIB_INSERT]=(SDL_FRect){x,bottom+96,w/3-4,42};
    a->library_buttons[LIB_OPEN_ITEM]=(SDL_FRect){x+w/3+2,bottom+96,w/3-4,42};
    a->library_buttons[LIB_OPEN_PDF]=(SDL_FRect){x+2*w/3+4,bottom+96,w/3-4,42};
    a->library_buttons[LIB_STYLE]=(SDL_FRect){x,bottom+166,w*.46f-3,42};
    a->library_buttons[LIB_SYNC]=(SDL_FRect){x+w*.46f+3,bottom+166,w*.54f-3,42};
    if(*a->library_warning) a->library_buttons[LIB_RETRY]=(SDL_FRect){x+w-106,bottom+2,106,38};
    a->library_rows.y-=a->library_outer_scroll;
    for(size_t i=0;i<LIB_BUTTON_COUNT;i++)
        if(i!=LIB_CLOSE && i!=LIB_VIEW) a->library_buttons[i].y-=a->library_outer_scroll;
    float maximum=SDL_max(0,(float)a->library.row_count*62-a->library_rows.h);
    a->library.scroll=SDL_clamp(a->library.scroll,0,maximum);
}
static void clipped_label(App *a,const char *text,SDL_FRect box,SDL_Color color) {
    SDL_Rect clip={(int)box.x,(int)box.y,(int)box.w,(int)box.h};
    bool had_clip=SDL_RenderClipEnabled(a->renderer); SDL_Rect previous;
    if(had_clip) {
        SDL_GetRenderClipRect(a->renderer,&previous);
        if(!SDL_GetRectIntersection(&previous,&clip,&clip)) return;
    }
    SDL_SetRenderClipRect(a->renderer,&clip); label(a,text,box.x,box.y,color);
    SDL_SetRenderClipRect(a->renderer,had_clip?&previous:NULL);
}
static void library_draw(App *a) {
    SDL_SetRenderDrawBlendMode(a->renderer,SDL_BLENDMODE_BLEND);
    fill(a,(SDL_FRect){0,0,(float)a->width,a->height-30.0f},(SDL_Color){0,0,0,85});
    SDL_SetRenderDrawBlendMode(a->renderer,SDL_BLENDMODE_NONE);
    SDL_Color bg=paper(a),text=ink(a),surface=a->dark?(SDL_Color){42,49,57,255}:(SDL_Color){234,240,236,255};
    fill(a,a->library_area,bg);
    const char *names[LIB_BUTTON_COUNT]={"关闭","导入文件","文献库","当前引用","搜索","分类列表","全部条目","含子分类","页码","摘录","插入引用","原条目","PDF","APA · 近似","同步近似尾注","操作 ↓","重试保存"};
    names[LIB_VIEW]=a->library_outer_scroll>0?"列表 ↑":"操作 ↓";
    names[LIB_RETRY]=a->library_restore_failed?"重试恢复":"重试保存";
    char search[512],page[256],note[256],style[64];
    SDL_snprintf(search,sizeof search,"搜索：%s",a->library.query && *a->library.query?a->library.query:"标题、作者或引用键"); names[LIB_SEARCH]=search;
    names[LIB_COLLECTIONS]=a->library.collections_view?"返回条目":"分类列表";
    names[LIB_DESCENDANTS]=a->library.include_descendants?"含子分类 ✓":"仅本分类";
    const char *page_value=a->library_citation_mode?a->citation_page:a->insert_page;
    const char *note_value=a->library_citation_mode?a->citation_note:a->insert_note;
    SDL_snprintf(page,sizeof page,"页码：%s",page_value?page_value:""); names[LIB_PAGE]=page;
    SDL_snprintf(note,sizeof note,"摘录：%s",note_value?note_value:""); names[LIB_NOTE]=note;
    names[LIB_INSERT]=a->library_citation_mode?"保存引用":"插入引用";
    SDL_snprintf(style,sizeof style,"%s · 近似",approximate_style(a)); names[LIB_STYLE]=style;
    for(size_t i=0;i<LIB_BUTTON_COUNT;i++) if(a->library_buttons[i].w && i!=LIB_CLOSE && i!=LIB_VIEW) {
        SDL_FRect button=a->library_buttons[i];
        fill(a,button,surface); clipped_label(a,names[i],(SDL_FRect){button.x+8,button.y+10,button.w-16,24},text);
    }
    float offset=a->library_outer_scroll;
    float x=a->library_area.x+12,w=a->library_area.w-24,bottom=a->library_content_height-220-offset;
    if(a->library_citation_mode) {
        clipped_label(a,a->citation_title && *a->citation_title?a->citation_title:"当前未选中引用块",(SDL_FRect){x,120-offset,w,30},text);
        label(a,"在正文中点击引用块，再打开此面板。",x,166-offset,text);
        label(a,"页码和摘录按“保存引用”后写入，可撤销。",x,198-offset,text);
    } else {
        const char *collection=a->library.collection<a->library.library.collection_count?a->library.library.collections[a->library.collection].name:"全部文献";
        char summary[512]; SDL_snprintf(summary,sizeof summary,"%s · %zu %s",collection?collection:"分类",a->library.row_count,a->library.collections_view?"个分类":"条结果");
        clipped_label(a,summary,(SDL_FRect){x,208-offset,w,24},text);
        SDL_Rect clip={(int)a->library_rows.x,(int)a->library_rows.y,(int)a->library_rows.w,(int)a->library_rows.h};
        SDL_SetRenderClipRect(a->renderer,&clip);
        for(size_t row=0;row<a->library.row_count;row++) {
            float y=a->library_rows.y+(float)row*62-a->library.scroll;
            if(y+60<a->library_rows.y || y>a->library_rows.y+a->library_rows.h) continue;
            size_t index=a->library.rows[row];
            if(a->library.collections_view) {
                const CeZoteroCollection *c=&a->library.library.collections[index];
                float indent=(float)SDL_min(ce_library_panel_collection_depth(&a->library,index),6)*14;
                clipped_label(a,c->name?c->name:"未命名分类",(SDL_FRect){x+8+indent,y+8,w-16-indent,24},text);
                char detail[96]; SDL_snprintf(detail,sizeof detail,"%zu 条直接文献 · %zu 个子分类",c->item_count,c->child_count);
                clipped_label(a,detail,(SDL_FRect){x+8+indent,y+34,w-16-indent,24},text);
            } else {
                const CeZoteroItem *item=&a->library.library.items[index];
                if(a->library.selected_item==index) fill(a,(SDL_FRect){x,y,w,60},surface);
                clipped_label(a,item->title && *item->title?item->title:"未命名文献",(SDL_FRect){x+8,y+7,w-16,24},text);
                char detail[512]; SDL_snprintf(detail,sizeof detail,"%s · %s",item->author?item->author:"",item->year?item->year:"");
                clipped_label(a,detail,(SDL_FRect){x+8,y+33,w-16,24},text);
            }
        }
        if(!a->library.row_count) {
            label(a,a->library.library.item_count?"没有符合条件的结果":"点击“导入文件”选择 CSL JSON 或 SQLite",x+8,a->library_rows.y+12,text);
        }
        SDL_SetRenderClipRect(a->renderer,NULL);
    }
    const CeZoteroItem *selected=ce_library_panel_selected(&a->library);
    const char *title=a->library_citation_mode?a->citation_title:selected?selected->title:NULL;
    if(*a->library_warning) clipped_label(a,a->library_warning,(SDL_FRect){x,bottom+8,w-114,30},(SDL_Color){190,55,45,255});
    else clipped_label(a,title && *title?title:"选择文献后插入到正文光标处",(SDL_FRect){x,bottom+8,w,30},text);
    clipped_label(a,"尾注采用近似格式，尚未使用完整 CSL 引擎。",(SDL_FRect){x,bottom+142,w,22},text);
    fill(a,(SDL_FRect){a->library_area.x,0,a->library_area.w,56},bg);
    clipped_label(a,"Zotero 文献",(SDL_FRect){x+4,16,a->library_buttons[LIB_VIEW].w?140:w-72,30},text);
    const LibraryAction fixed[]={LIB_CLOSE,LIB_VIEW};
    for(size_t i=0;i<SDL_arraysize(fixed);i++) {
        SDL_FRect box=a->library_buttons[fixed[i]]; if(!box.w) continue;
        fill(a,box,surface); label(a,names[fixed[i]],box.x+8,box.y+10,text);
    }
}
static void library_event(App *a,SDL_Event *event) {
    if(event->type==SDL_EVENT_KEY_DOWN) {
        if(event->key.key==SDLK_ESCAPE) library_close(a);
        else if(event->key.key==SDLK_F && (event->key.mod&(SDL_KMOD_CTRL|SDL_KMOD_GUI))) { a->library_citation_mode=false; library_prompt(a,PROMPT_LIBRARY_SEARCH); }
        else if(event->key.key==SDLK_RETURN && !a->library.collections_view) library_action(a,LIB_INSERT);
        else if(!a->library_citation_mode && !a->library.collections_view && (event->key.key==SDLK_UP || event->key.key==SDLK_DOWN) && a->library.row_count) {
            size_t row=0;
            for(size_t i=0;i<a->library.row_count;i++) if(a->library.rows[i]==a->library.selected_item) row=i;
            if(a->library.selected_item==SIZE_MAX) row=0;
            else if(event->key.key==SDLK_UP && row) row--;
            else if(event->key.key==SDLK_DOWN && row+1<a->library.row_count) row++;
            a->library.selected_item=a->library.rows[row];
            if(row*62<a->library.scroll) a->library.scroll=(float)row*62;
            if((row+1)*62>a->library.scroll+a->library_rows.h) a->library.scroll=(float)(row+1)*62-a->library_rows.h;
        }
    } else if(event->type==SDL_EVENT_MOUSE_WHEEL && !a->library_citation_mode) {
        a->library.scroll=SDL_max(0,a->library.scroll-event->wheel.y*62);
    } else if(event->type==SDL_EVENT_MOUSE_BUTTON_DOWN) {
        if(event->button.y>=a->library_area.h) return;
        if(event->button.y<56) {
            if(contains(a->library_buttons[LIB_CLOSE],event->button.x,event->button.y)) library_action(a,LIB_CLOSE);
            else if(contains(a->library_buttons[LIB_VIEW],event->button.x,event->button.y)) library_action(a,LIB_VIEW);
            return;
        }
        for(size_t i=0;i<LIB_BUTTON_COUNT;i++) if(contains(a->library_buttons[i],event->button.x,event->button.y)) {
            library_action(a,(LibraryAction)i); return;
        }
        if(!a->library_citation_mode && contains(a->library_rows,event->button.x,event->button.y)) {
            a->library_pressed_row=(size_t)((event->button.y-a->library_rows.y+a->library.scroll)/62);
            a->library_dragging=true; a->library_drag_moved=false;
            a->library_drag_y=a->library_drag_origin=event->button.y;
        }
    } else if(event->type==SDL_EVENT_MOUSE_MOTION && a->library_dragging) {
        if(SDL_fabsf(event->motion.y-a->library_drag_origin)>5) a->library_drag_moved=true;
        a->library.scroll=SDL_max(0,a->library.scroll+a->library_drag_y-event->motion.y); a->library_drag_y=event->motion.y;
    } else if(event->type==SDL_EVENT_MOUSE_BUTTON_UP) {
        if(a->library_dragging && !a->library_drag_moved && a->library_pressed_row<a->library.row_count &&
           contains(a->library_rows,event->button.x,event->button.y)) {
            size_t index=a->library.rows[a->library_pressed_row];
            if(a->library.collections_view) {
                /* A collection-name search should not hide all its items. */
                if(!ce_library_panel_query(&a->library,"") || !ce_library_panel_collection(&a->library,index)) message(a,"无法打开分类");
            } else a->library.selected_item=index;
        }
        a->library_dragging=false;
    } else if(event->type==SDL_EVENT_WINDOW_FOCUS_LOST) a->library_dragging=false;
}

static void layout(App *a) {
    /* Layout uses window points. The renderer scales these coordinates to the
     * Retina/mobile framebuffer; text and hit testing share that same space. */
    int window_width=1,window_height=1;
    SDL_GetWindowSize(a->window,&window_width,&window_height);
    window_width=SDL_max(1,window_width); window_height=SDL_max(1,window_height);
    int logical_width=0,logical_height=0; SDL_RendererLogicalPresentation mode;
    SDL_GetRenderLogicalPresentation(a->renderer,&logical_width,&logical_height,&mode);
    if(logical_width!=window_width || logical_height!=window_height || mode!=SDL_LOGICAL_PRESENTATION_STRETCH)
        SDL_SetRenderLogicalPresentation(a->renderer,window_width,window_height,SDL_LOGICAL_PRESENTATION_STRETCH);
    SDL_Rect bounds={0,0,window_width,window_height},safe=bounds,intersection;
    if(!SDL_GetWindowSafeArea(a->window,&safe) || !SDL_GetRectIntersection(&bounds,&safe,&intersection)) safe=bounds;
    else safe=intersection;
    /* Let SDL translate both drawing and pointer coordinates by the safe-area
     * origin, keeping the notch/status bar and home indicator outside controls. */
    SDL_SetRenderViewport(a->renderer,&safe);
    a->width=safe.w; a->height=safe.h;
    /* Fresh narrow windows should expose the editor. Once the user chooses
     * the sidebar state, keep that choice through subsequent layout passes. */
    if (!a->sidebar_manual) a->sidebar = a->width >= 700;
    library_layout(a);
    int columns = SDL_max(1, (a->width - 26) / 78);
    int per_page = SDL_max(1, columns * 2 - 1);
    int actions = (int)SDL_arraysize(a->buttons) - 1;
    int pages = (actions + per_page - 1) / per_page;
    a->toolbar_page %= pages;
    int first = a->toolbar_page * per_page, shown = SDL_min(per_page, actions - first);
    for (size_t i = 0; i < SDL_arraysize(a->buttons); ++i) a->buttons[i].box = (SDL_FRect){0};
    for (int i = 0; i < shown; ++i) {
        a->buttons[first+i].box = (SDL_FRect){16.0f+(i%columns)*78,50.0f+(i/columns)*40,72,34};
    }
    if (pages > 1) a->buttons[actions].box = (SDL_FRect){16.0f+(per_page%columns)*78,50.0f+(per_page/columns)*40,72,34};
    float top = 138;
    float sidebar_width = a->sidebar ? SDL_min(a->sidebar_width, a->width * 0.72f) : 0;
    bool overlay = a->width < 700;
    float left = (!overlay && a->sidebar) ? sidebar_width : 0;
    a->sidebar_area = (SDL_FRect){0,top,sidebar_width,SDL_max(40,a->height-top-32)};
    a->note_import_button=a->sidebar?(SDL_FRect){sidebar_width-98,top+4,88,36}:(SDL_FRect){0};
    a->editor_area = (SDL_FRect){left+24,top+30,SDL_max(20,a->width-left-48),SDL_max(40,a->height-top-76)};
    a->edit->rect = a->editor_area;
    a->edit->rect.y -= a->scroll;
    a->edit->rect.h += a->scroll;
    TTF_SetTextWrapWidth(a->edit->text, (int)a->editor_area.w);
    bool multiline=a->prompt_mode==PROMPT_CITATION_NOTE;
    float dialog_height=multiline?SDL_min(310.0f,a->height-32.0f):166;
    a->prompt_area=(SDL_FRect){12,a->height/2.0f-dialog_height/2.0f+9,a->width-24.0f,dialog_height};
    a->prompt_input_area=(SDL_FRect){24,a->prompt_area.y+66,SDL_max(20,a->width-48),SDL_max(40,dialog_height-126)};
    a->prompt->rect=a->prompt_input_area;
    a->prompt_accept=(SDL_FRect){24,a->prompt_area.y+dialog_height-44,80,32};
    a->prompt_cancel=(SDL_FRect){116,a->prompt_area.y+dialog_height-44,80,32};
    TTF_SetTextWrapWidth(a->prompt->text, (int)a->prompt->rect.w);
    if(a->prompt_mode!=PROMPT_NONE && a->prompt_mode!=PROMPT_DELETE) {
        SDL_FRect caret;
        int cursor=a->prompt->composition_length?a->prompt->composition_start+a->prompt->composition_cursor:a->prompt->cursor;
        if(EditBox_CaretRect(a->prompt,cursor,&caret)) {
            if(caret.y<a->prompt_scroll) a->prompt_scroll=caret.y;
            else if(caret.y+caret.h>a->prompt_scroll+a->prompt_input_area.h) a->prompt_scroll=caret.y+caret.h-a->prompt_input_area.h;
        }
        a->prompt->rect.y-=a->prompt_scroll; a->prompt->rect.h+=a->prompt_scroll;
    }
    int input_target = a->prompt_mode != PROMPT_NONE ? (a->prompt->has_focus ? 2 : 0) : (a->edit->has_focus ? 1 : 0);
    SDL_FRect input_area=input_target==2?a->prompt_input_area:a->editor_area;
#ifdef __EMSCRIPTEN__
    /* The DOM bridge maps framebuffer pixels to CSS pixels. Convert from the
     * renderer's logical coordinates, including any viewport offset, first. */
    float min_x,min_y,max_x,max_y; int pixel_width,pixel_height;
    if(SDL_RenderCoordinatesToWindow(a->renderer,input_area.x,input_area.y,&min_x,&min_y) &&
       SDL_RenderCoordinatesToWindow(a->renderer,input_area.x+input_area.w,input_area.y+input_area.h,&max_x,&max_y) &&
       SDL_GetWindowSizeInPixels(a->window,&pixel_width,&pixel_height)) {
        float sx=(float)pixel_width/window_width,sy=(float)pixel_height/window_height;
        input_area=(SDL_FRect){min_x*sx,min_y*sy,(max_x-min_x)*sx,(max_y-min_y)*sy};
    }
#endif
    platform_web_input_update(input_target, a->window, &input_area);
}
static void reveal_cursor(App *a) {
    SDL_FRect caret;
    int position=a->edit->composition_length ? a->edit->composition_start+a->edit->composition_cursor : a->edit->cursor;
    if (!EditBox_CaretRect(a->edit,position,&caret)) return;
    float pos = caret.y;
    if (a->typewriter) a->scroll = SDL_max(0, pos - a->editor_area.h/2);
    else if (pos < a->scroll) a->scroll = pos;
    else if (pos + caret.h > a->scroll + a->editor_area.h) a->scroll = pos + caret.h - a->editor_area.h;
    a->edit->rect.y = a->editor_area.y-a->scroll;
    a->edit->rect.h = a->editor_area.h+a->scroll;
}
static void draw(App *a) {
    layout(a);
    SDL_Color text = ink(a), bg = paper(a);
    SDL_Color surface = a->dark ? (SDL_Color){36,42,49,255} : (SDL_Color){239,242,239,255};
    SDL_SetRenderDrawColor(a->renderer,bg.r,bg.g,bg.b,255); SDL_RenderClear(a->renderer);
    label(a,"NOTE LAB",18,12,text);
    clipped_label(a,a->active ? a->active : "跨平台 C 编辑器",(SDL_FRect){170,12,SDL_max(0,a->width-274),30},text);
    fill(a,a->library_button,surface);
    label(a,*a->library_warning?"文献 !":"文献",a->library_button.x+16,a->library_button.y+10,
          *a->library_warning?(SDL_Color){190,55,45,255}:text);
    for (size_t i=0;i<SDL_arraysize(a->buttons);++i) {
        if (!a->buttons[i].box.w) continue;
        fill(a,a->buttons[i].box,surface);
        label(a,a->buttons[i].label,a->buttons[i].box.x+10,a->buttons[i].box.y+6,text);
    }
    SDL_Rect clip={(int)a->editor_area.x,(int)a->editor_area.y,(int)a->editor_area.w,(int)a->editor_area.h};
    SDL_SetRenderClipRect(a->renderer,&clip);
    if (a->active) {
        TTF_SetTextColor(a->edit->text,text.r,text.g,text.b,255);
        EditBox_Draw(a->edit);
    } else {
        label(a,"点击“新建”开始写作。",a->editor_area.x,a->editor_area.y+30,text);
        label(a,"点击“侧栏”打开或导入笔记。",a->editor_area.x,a->editor_area.y+68,text);
    }
    SDL_SetRenderClipRect(a->renderer,NULL);
    if (a->sidebar) {
        fill(a,a->sidebar_area,surface);
        label(a,"笔记",16,a->sidebar_area.y+10,text);
        fill(a,a->note_import_button,a->dark?(SDL_Color){48,57,65,255}:(SDL_Color){218,229,220,255});
        label(a,"导入",a->note_import_button.x+24,a->note_import_button.y+7,text);
        SDL_Rect side={(int)a->sidebar_area.x,(int)a->sidebar_area.y+40,(int)a->sidebar_area.w,(int)a->sidebar_area.h-40};
        SDL_SetRenderClipRect(a->renderer,&side);
        for(size_t i=0;i<a->files.count;++i) {
            float y=a->sidebar_area.y+44+(float)i*40-a->sidebar_scroll;
            if(y+36<side.y || y>side.y+side.h) continue;
            if(a->active && !strcmp(a->active,a->files.names[i])) fill(a,(SDL_FRect){8,y,a->sidebar_area.w-16,36},a->dark?(SDL_Color){43,74,68,255}:(SDL_Color){206,226,215,255});
            label(a,a->files.names[i],16,y+6,text);
        }
        SDL_SetRenderClipRect(a->renderer,NULL);
    }
    if(a->library_open) library_draw(a);
    fill(a,(SDL_FRect){0,a->height-30.0f,(float)a->width,30},surface);
    label(a,a->status,12,a->height-26.0f,text);
    if(a->prompt_mode!=PROMPT_NONE) {
        SDL_SetRenderDrawBlendMode(a->renderer,SDL_BLENDMODE_BLEND);
        fill(a,(SDL_FRect){0,0,(float)a->width,(float)a->height},(SDL_Color){0,0,0,110});
        SDL_SetRenderDrawBlendMode(a->renderer,SDL_BLENDMODE_NONE);
        fill(a,a->prompt_area,bg);
        const char *title=a->prompt_mode==PROMPT_NEW?"新建笔记 · 输入文件名":a->prompt_mode==PROMPT_FIND?"查找文字":a->prompt_mode==PROMPT_LINK?"链接地址（留空移除）":a->prompt_mode==PROMPT_LIBRARY_SEARCH?"搜索文献或分类（留空显示全部）":a->prompt_mode==PROMPT_CITATION_PAGE?"引用页码（留空清除）":a->prompt_mode==PROMPT_CITATION_NOTE?"引用摘录（留空清除）":"确认删除当前笔记？";
        label(a,title,24,a->prompt_area.y+18,text);
        if(a->prompt_mode!=PROMPT_DELETE) {
            fill(a,a->prompt_input_area,surface);
            TTF_SetTextColor(a->prompt->text,text.r,text.g,text.b,255);
            SDL_Rect prompt_clip={(int)a->prompt_input_area.x,(int)a->prompt_input_area.y,(int)a->prompt_input_area.w,(int)a->prompt_input_area.h};
            SDL_SetRenderClipRect(a->renderer,&prompt_clip); EditBox_Draw(a->prompt); SDL_SetRenderClipRect(a->renderer,NULL);
        }
        fill(a,a->prompt_accept,surface);
        label(a,"确认",a->prompt_accept.x+14,a->prompt_accept.y+4,text);
        fill(a,a->prompt_cancel,surface);
        label(a,"取消",a->prompt_cancel.x+14,a->prompt_cancel.y+4,text);
    }
    if(a->screenshot && a->frames==2) {
        SDL_Rect viewport;
        if(SDL_GetRenderViewport(a->renderer,&viewport) && SDL_SetRenderViewport(a->renderer,NULL)) {
            /* Include the device insets in startup evidence, not just the
             * current safe-area viewport. Restore it before normal drawing. */
            SDL_Surface *shot=SDL_RenderReadPixels(a->renderer,NULL);
            if(!shot) SDL_Log("Screenshot read failed: %s",SDL_GetError());
            if(!SDL_SetRenderViewport(a->renderer,&viewport))
                SDL_Log("Screenshot viewport restore failed: %s",SDL_GetError());
            if(shot) {
                if(!SDL_SaveBMP(shot,a->screenshot)) SDL_Log("Screenshot save failed: %s",SDL_GetError());
                SDL_DestroySurface(shot);
            }
        } else SDL_Log("Screenshot viewport setup failed: %s",SDL_GetError());
    }
    SDL_RenderPresent(a->renderer);
}

SDL_AppResult SDL_AppInit(void **state, int argc, char **argv) {
    App *a=calloc(1,sizeof(*a)); if(!a) return SDL_APP_FAILURE; *state=a;
    ce_library_panel_init(&a->library);
    const char *notes=NULL, *font=NULL;
    for(int i=1;i<argc;++i) {
        if(!strcmp(argv[i],"--notes") && i+1<argc) notes=argv[++i];
        else if(!strcmp(argv[i],"--font") && i+1<argc) font=argv[++i];
        else if(!strcmp(argv[i],"--smoke-test")) a->smoke=true;
        else if(!strcmp(argv[i],"--screenshot") && i+1<argc) a->screenshot=argv[++i];
    }
    if(a->smoke && !notes) { SDL_Log("--smoke-test requires an explicit --notes test directory"); return SDL_APP_FAILURE; }
    if(!SDL_Init(SDL_INIT_VIDEO) || !TTF_Init()) { SDL_Log("Initialization: %s",SDL_GetError()); return SDL_APP_FAILURE; }
    a->window=SDL_CreateWindow("Note Lab",1100,760,SDL_WINDOW_RESIZABLE|SDL_WINDOW_HIGH_PIXEL_DENSITY);
    if(a->window) a->renderer=SDL_CreateRenderer(a->window,NULL);
    if(!a->window || !a->renderer) { SDL_Log("Window: %s",SDL_GetError()); return SDL_APP_FAILURE; }
    SDL_SetRenderVSync(a->renderer,1);
#ifdef __EMSCRIPTEN__
    if(!font) font="/assets/font.otf";
    if(!notes) notes="/notes";
#endif
    char *font_path=NULL;
    if(!font) {
#ifdef __ANDROID__
        font="font.otf";
#else
        const char *base=SDL_GetBasePath();
        if(base) SDL_asprintf(&font_path,"%s%s",base,EDITOR_FONT_PATH);
        font=font_path?font_path:EDITOR_FONT_PATH;
#endif
    }
    a->font=TTF_OpenFont(font,20); a->ui_font=TTF_OpenFont(font,16);
    SDL_free(font_path);
    char *emoji_path=NULL;
#ifdef __EMSCRIPTEN__
    const char *emoji="/assets/emoji.ttf";
#elif defined(__ANDROID__)
    const char *emoji="emoji.ttf";
#else
    const char *base=SDL_GetBasePath();
    if(base) SDL_asprintf(&emoji_path,"%sassets/emoji.ttf",base);
    const char *emoji=emoji_path?emoji_path:"assets/emoji.ttf";
#endif
    a->emoji_font=TTF_OpenFont(emoji,20); a->emoji_ui_font=TTF_OpenFont(emoji,16);
    SDL_free(emoji_path);
    if(!a->font || !a->ui_font || !a->emoji_font || !a->emoji_ui_font ||
       !TTF_AddFallbackFont(a->font,a->emoji_font) || !TTF_AddFallbackFont(a->ui_font,a->emoji_ui_font)) {
        SDL_Log("Required text/emoji font: %s",SDL_GetError()); return SDL_APP_FAILURE;
    }
    a->engine=TTF_CreateRendererTextEngine(a->renderer);
    a->doc=ce_document_new();
    if(!a->font || !a->ui_font || !a->engine || !a->doc) { SDL_Log("Font/model: %s",SDL_GetError()); return SDL_APP_FAILURE; }
    SDL_FRect box={260,160,800,540};
    a->edit=EditBox_Create(a->window,a->renderer,a->engine,a->font,&box);
    a->prompt=EditBox_Create(a->window,a->renderer,a->engine,a->font,&box);
    if(!a->edit || !a->prompt) return SDL_APP_FAILURE;
    a->rich=ce_rich_layout_create(a->renderer,a->font);
    if(!a->rich) return SDL_APP_FAILURE;
    ce_rich_layout_set_fallback(a->rich,a->emoji_font);
    a->rich_dirty=true;
    EditBox_SetLayout(a->edit,&rich_hooks,a);
    char *default_path=notes?NULL:SDL_GetPrefPath("NoteLab","notes");
    NoteResult result=note_store_init(&a->store,notes?notes:default_path);
    if(result==NOTE_OK) a->notes_root=copy_string(notes?notes:default_path);
    SDL_free(default_path);
    if(result!=NOTE_OK) { SDL_Log("Notes: %s",a->store.error); return SDL_APP_FAILURE; }
    if(!a->notes_root) return SDL_APP_FAILURE;
    Button buttons[]={
        {"侧栏",ACT_SIDEBAR,{0}}, {"新建",ACT_NEW,{0}}, {"保存",ACT_SAVE,{0}},
        {"撤销",ACT_UNDO,{0}}, {"重做",ACT_REDO,{0}}, {"粗体",ACT_BOLD,{0}},
        {"斜体",ACT_ITALIC,{0}}, {"下划线",ACT_UNDERLINE,{0}}, {"高亮",ACT_HIGHLIGHT,{0}},
        {"文字色",ACT_RED,{0}}, {"链接",ACT_LINK,{0}}, {"无序",ACT_BULLETS,{0}},
        {"有序",ACT_NUMBERS,{0}}, {"标题",ACT_HEADING,{0}}, {"查找",ACT_FIND,{0}},
        {"主题",ACT_THEME,{0}}, {"打字机",ACT_TYPEWRITER,{0}}, {"JSON",ACT_JSON,{0}},
        {"删除",ACT_DELETE,{0}}, {"刷新",ACT_REFRESH,{0}}, {"段落",ACT_PARAGRAPH,{0}},
        {"清格式",ACT_CLEAR,{0}}, {"左对齐",ACT_LEFT,{0}}, {"居中",ACT_CENTER,{0}},
        {"右对齐",ACT_RIGHT,{0}}, {"两端",ACT_JUSTIFY,{0}}, {"上一个",ACT_FIND_PREVIOUS,{0}},
        {"下一个",ACT_FIND_NEXT,{0}}, {"段上移",ACT_MOVE_UP,{0}}, {"段下移",ACT_MOVE_DOWN,{0}},
        {"更多 ›",ACT_MORE,{0}}
    };
    SDL_COMPILE_TIME_ASSERT(button_count, SDL_arraysize(buttons) == SDL_arraysize(a->buttons));
    memcpy(a->buttons,buttons,sizeof(buttons)); a->sidebar=true; a->sidebar_width=240;
    platform_web_input_init();
    platform_web_input_clipboard(browser_clipboard_read, browser_clipboard_cut, browser_clipboard_error, SDL_AppEvent, a);
    refresh_files(a); layout(a);
    message(a,"就绪 · Ctrl/Cmd+N 新建 · Ctrl/Cmd+S 保存");
    if(a->files.count) open_note(a,a->files.names[0]);
    library_cache_load(a,true);
    return SDL_APP_CONTINUE;
}
SDL_AppResult SDL_AppEvent(void *state, SDL_Event *event) {
    if (platform_web_input_handle_event(event, SDL_AppEvent, state)) return SDL_APP_CONTINUE;
    App *a=state;
    if(event->type==SDL_EVENT_QUIT) {
        if (a->export_job || a->import_job || a->pending_note_json) { message(a, "请先完成或取消文件导入导出"); return SDL_APP_CONTINUE; }
        if(a->library_unsaved && !library_cache_save(a)) return SDL_APP_CONTINUE;
        return save_document(a)?SDL_APP_SUCCESS:SDL_APP_CONTINUE;
    }
    if(event->type==SDL_EVENT_WILL_ENTER_BACKGROUND) {
        save_document(a); if(a->library_unsaved && !a->library_cache_saving) library_cache_save(a);
        return SDL_APP_CONTINUE;
    }
    layout(a);
    SDL_ConvertEventToRenderCoordinates(a->renderer,event);
    if (a->resizing_sidebar) {
        if (event->type == SDL_EVENT_MOUSE_MOTION) a->sidebar_width = SDL_clamp(event->motion.x, 150, SDL_min(600, a->width * 0.72f));
        if (event->type == SDL_EVENT_MOUSE_BUTTON_UP || event->type == SDL_EVENT_WINDOW_FOCUS_LOST) {
            a->resizing_sidebar = false; SDL_CaptureMouse(false);
        }
        if (event->type == SDL_EVENT_MOUSE_MOTION || event->type == SDL_EVENT_MOUSE_BUTTON_UP) return SDL_APP_CONTINUE;
    }
    if(a->prompt_mode!=PROMPT_NONE) {
        if(event->type==SDL_EVENT_KEY_DOWN && event->key.key==SDLK_RETURN && !a->prompt->composition_length &&
           (a->prompt_mode!=PROMPT_CITATION_NOTE || (event->key.mod&(SDL_KMOD_CTRL|SDL_KMOD_GUI)))) accept_prompt(a);
        else if(event->type==SDL_EVENT_KEY_DOWN && event->key.key==SDLK_ESCAPE) close_prompt(a);
        else if(event->type==SDL_EVENT_MOUSE_BUTTON_DOWN && contains(a->prompt_accept,event->button.x,event->button.y)) accept_prompt(a);
        else if(event->type==SDL_EVENT_MOUSE_BUTTON_DOWN && contains(a->prompt_cancel,event->button.x,event->button.y)) close_prompt(a);
        else if(a->prompt_mode!=PROMPT_DELETE) {
            if(event->type==SDL_EVENT_KEY_DOWN && (event->key.mod&SDL_KMOD_GUI)) event->key.mod|=SDL_KMOD_CTRL;
            EditBox_HandleEvent(a->prompt,event);
        }
        return SDL_APP_CONTINUE;
    }
    if(a->library_open) { library_event(a,event); return SDL_APP_CONTINUE; }
    if (a->active && a->edit->composition_length) {
        if (event->type == SDL_EVENT_KEY_DOWN) {
            if (event->key.key == SDLK_ESCAPE) { SDL_ClearComposition(a->window); sync_view(a); }
            return SDL_APP_CONTINUE;
        }
        if (event->type == SDL_EVENT_MOUSE_BUTTON_DOWN) { SDL_ClearComposition(a->window); sync_view(a); }
    }
    if(event->type==SDL_EVENT_MOUSE_BUTTON_DOWN) {
        if(contains(a->library_button,event->button.x,event->button.y)) { library_show(a); return SDL_APP_CONTINUE; }
        if (a->sidebar && a->width >= 700 && contains((SDL_FRect){a->sidebar_area.w-4,a->sidebar_area.y,8,a->sidebar_area.h},event->button.x,event->button.y)) {
            a->resizing_sidebar = true; SDL_CaptureMouse(true); return SDL_APP_CONTINUE;
        }
        for(size_t i=0;i<SDL_arraysize(a->buttons);++i) if(contains(a->buttons[i].box,event->button.x,event->button.y)) {
            action(a,a->buttons[i].action); return SDL_APP_CONTINUE;
        }
        if(a->sidebar && contains(a->sidebar_area,event->button.x,event->button.y)) {
            if(contains(a->note_import_button,event->button.x,event->button.y)) { document_import_begin(a); return SDL_APP_CONTINUE; }
            float pos=event->button.y-a->sidebar_area.y-44+a->sidebar_scroll;
            if(pos>=0 && (size_t)(pos/40)<a->files.count) open_note(a,a->files.names[(size_t)(pos/40)]);
            return SDL_APP_CONTINUE;
        }
        if(!contains(a->editor_area,event->button.x,event->button.y)) return SDL_APP_CONTINUE;
    }
    if(event->type==SDL_EVENT_MOUSE_WHEEL) {
        if(a->sidebar && contains(a->sidebar_area,event->wheel.mouse_x,event->wheel.mouse_y))
            a->sidebar_scroll=SDL_max(0,a->sidebar_scroll-event->wheel.y*40);
        else {
            float height;
            if(rich_prepare(a,a->edit)) height=ce_rich_layout_height(a->rich);
            else { int w,h; TTF_GetTextSize(a->edit->text,&w,&h); height=(float)h; }
            a->scroll=SDL_clamp(a->scroll-event->wheel.y*48,0,SDL_max(0,height-a->editor_area.h+24));
        }
        return SDL_APP_CONTINUE;
    }
    if(event->type==SDL_EVENT_KEY_DOWN && (event->key.mod&(SDL_KMOD_CTRL|SDL_KMOD_GUI)) && !a->edit->composition_length) {
        Action cmd; bool handled=true;
        switch(event->key.key) {
            case SDLK_N:cmd=ACT_NEW;break; case SDLK_S:cmd=ACT_SAVE;break;
            case SDLK_Z:cmd=(event->key.mod&SDL_KMOD_SHIFT)?ACT_REDO:ACT_UNDO;break;
            case SDLK_Y:cmd=ACT_REDO;break;case SDLK_B:cmd=ACT_BOLD;break;
            case SDLK_I:cmd=ACT_ITALIC;break;case SDLK_U:cmd=ACT_UNDERLINE;break;
            case SDLK_F:cmd=ACT_FIND;break; default:handled=false;cmd=ACT_SAVE;break;
        }
        if(handled) { action(a,cmd); reveal_cursor(a); return SDL_APP_CONTINUE; }
        /* The upstream sample uses Ctrl; map Command on macOS for clipboard. */
        if(event->key.mod&SDL_KMOD_GUI) event->key.mod|=SDL_KMOD_CTRL;
    }
    if(a->active) {
        int cursor=a->edit->cursor;
        bool navigation=event->type==SDL_EVENT_KEY_DOWN && (event->key.key==SDLK_LEFT || event->key.key==SDLK_RIGHT || event->key.key==SDLK_UP || event->key.key==SDLK_DOWN || event->key.key==SDLK_HOME || event->key.key==SDLK_END);
        EditBox_HandleEvent(a->edit,event);
        if(navigation) {
            if(event->key.mod&SDL_KMOD_SHIFT) {
                if(a->edit->highlight1<0) a->edit->highlight1=cursor;
                a->edit->highlight2=a->edit->cursor;
            } else a->edit->highlight1=a->edit->highlight2=-1;
        }
        if(event->type==SDL_EVENT_TEXT_INPUT || event->type==SDL_EVENT_KEY_DOWN) { reconcile_input(a); reveal_cursor(a); }
        else if(event->type==SDL_EVENT_TEXT_EDITING) reveal_cursor(a);
    }
    return SDL_APP_CONTINUE;
}
SDL_AppResult SDL_AppIterate(void *state) {
    App *a=state;
    library_import_poll(a);
    document_import_finish(a);
    if (a->export_job) {
        const char *result = NULL;
        if (platform_export_status(a->export_job, &result) != PLATFORM_EXPORT_PENDING) {
            message(a, result); platform_export_free(a->export_job); a->export_job = NULL;
        }
    }
    if(a->active && !a->saving && ce_document_is_modified(a->doc) && !a->edit->composition_length && SDL_GetTicks()-a->edited_at>=1000) {
        if(!save_document(a)) a->edited_at=SDL_GetTicks();
    }
    draw(a); ++a->frames;
    if(a->smoke && a->frames>=3) { SDL_Log("NoteLab smoke test passed"); return SDL_APP_SUCCESS; }
    return SDL_APP_CONTINUE;
}
void SDL_AppQuit(void *state, SDL_AppResult result) {
    (void)result; App *a=state; if(!a) return;
    platform_web_input_destroy();
    EditBox_Destroy(a->prompt); EditBox_Destroy(a->edit);
    ce_rich_layout_destroy(a->rich); free(a->rich_text);
    ce_document_free(a->doc); note_store_list_free(&a->files); note_store_destroy(&a->store);
    note_store_destroy(&a->library_store); free(a->notes_root);
    free(a->active); free(a->search_keyword);
    free(a->saving_name); free(a->saving_snapshot);
    platform_export_free(a->export_job);
    platform_import_free(a->import_job);
    document_import_clear(a);
    ce_library_panel_destroy(&a->library); citation_context_clear(a);
    free(a->insert_page); free(a->insert_note);
    if(a->engine) TTF_DestroyRendererTextEngine(a->engine);
    if(a->ui_font) TTF_CloseFont(a->ui_font);
    if(a->font) TTF_CloseFont(a->font);
    if(a->emoji_ui_font) TTF_CloseFont(a->emoji_ui_font);
    if(a->emoji_font) TTF_CloseFont(a->emoji_font);
    if(a->renderer) SDL_DestroyRenderer(a->renderer);
    if(a->window) SDL_DestroyWindow(a->window);
    TTF_Quit(); SDL_Quit(); free(a);
}
