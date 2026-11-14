/* Note Lab: shared C application, used by SDL's desktop/mobile/web entry points. */
#define SDL_MAIN_USE_CALLBACKS 1
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#include <SDL3_ttf/SDL_ttf.h>
#include "document.h"
#include "rich_layout.h"
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

typedef enum { PROMPT_NONE, PROMPT_NEW, PROMPT_FIND, PROMPT_LINK, PROMPT_DELETE } Prompt;
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
    NoteStore store;
    NoteList files;
    char *active;
    char *search_keyword;
    char *saving_name, *saving_snapshot;
    bool saving, persistence_error;
    char status[512];
    Prompt prompt_mode;
    size_t selection_start, selection_end;
    SDL_FRect editor_area, sidebar_area;
    int width, height;
    float scroll, sidebar_scroll, sidebar_width;
    int toolbar_page;
    bool resizing_sidebar;
    bool dark, sidebar, typewriter, smoke;
    Uint64 edited_at;
    unsigned frames;
    const char *screenshot;
    Button buttons[31];
} App;

static void reveal_cursor(App *a);

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
    EM_ASM({ Module.editorUnsaved = !!$0; }, ce_document_is_modified(a->doc) || a->persistence_error);
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
    message(a, success ? "已保存到持久存储" : error);
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
    message(a, "已打开 · 自动保存开启");
}
static void get_selection(App *a, size_t *start, size_t *end) {
    if (a->edit->highlight1 >= 0 && a->edit->highlight2 >= 0) {
        *start = (size_t)SDL_min(a->edit->highlight1, a->edit->highlight2);
        *end = (size_t)SDL_max(a->edit->highlight1, a->edit->highlight2);
    } else { *start = *end = (size_t)a->edit->cursor; }
}
static void show_prompt(App *a, Prompt kind) {
    get_selection(a, &a->selection_start, &a->selection_end);
    a->prompt_mode = kind;
    EditBox_SetFocus(a->edit, false);
    /* Cancel uncommitted input when changing input controls. */
    SDL_ClearComposition(a->window);
    sync_view(a);
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
    EditBox_SetFocus(a->edit, a->active != NULL);
}
static void mark_changed(App *a) { a->rich_dirty = true; a->edited_at = SDL_GetTicks(); update_title(a); }
static void accept_prompt(App *a) {
    if (a->prompt->composition_length) { message(a, "请先完成输入法选词"); return; }
    const char *value = a->prompt->text->text ? a->prompt->text->text : "";
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
    if (which == ACT_SIDEBAR) { a->sidebar = !a->sidebar; return; }
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
static void layout(App *a) {
    SDL_GetCurrentRenderOutputSize(a->renderer, &a->width, &a->height);
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
    a->editor_area = (SDL_FRect){left+24,top+30,SDL_max(20,a->width-left-48),SDL_max(40,a->height-top-76)};
    a->edit->rect = a->editor_area;
    a->edit->rect.y -= a->scroll;
    a->edit->rect.h += a->scroll;
    TTF_SetTextWrapWidth(a->edit->text, (int)a->editor_area.w);
    a->prompt->rect = (SDL_FRect){24, a->height/2.0f-8, SDL_max(20,a->width-48), 40};
    TTF_SetTextWrapWidth(a->prompt->text, (int)a->prompt->rect.w);
    int input_target = a->prompt_mode != PROMPT_NONE ? (a->prompt->has_focus ? 2 : 0) : (a->edit->has_focus ? 1 : 0);
    platform_web_input_update(input_target, a->window, input_target == 2 ? &a->prompt->rect : &a->editor_area);
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
    label(a,a->active ? a->active : "跨平台 C 编辑器",170,12,text);
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
        label(a,"在侧边栏打开笔记，或点击“新建”。",a->editor_area.x,a->editor_area.y+30,text);
        label(a,"支持中文输入、自动保存和撤销。",a->editor_area.x,a->editor_area.y+68,text);
    }
    SDL_SetRenderClipRect(a->renderer,NULL);
    if (a->sidebar) {
        fill(a,a->sidebar_area,surface);
        label(a,"笔记",16,a->sidebar_area.y+10,text);
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
    fill(a,(SDL_FRect){0,a->height-30.0f,(float)a->width,30},surface);
    label(a,a->status,12,a->height-26.0f,text);
    if(a->prompt_mode!=PROMPT_NONE) {
        SDL_SetRenderDrawBlendMode(a->renderer,SDL_BLENDMODE_BLEND);
        fill(a,(SDL_FRect){0,0,(float)a->width,(float)a->height},(SDL_Color){0,0,0,110});
        SDL_SetRenderDrawBlendMode(a->renderer,SDL_BLENDMODE_NONE);
        fill(a,(SDL_FRect){12,a->height/2.0f-74,a->width-24.0f,166},bg);
        const char *title=a->prompt_mode==PROMPT_NEW?"新建笔记 · 输入文件名":a->prompt_mode==PROMPT_FIND?"查找文字":a->prompt_mode==PROMPT_LINK?"链接地址（留空移除）":"确认删除当前笔记？";
        label(a,title,24,a->height/2.0f-56,text);
        if(a->prompt_mode!=PROMPT_DELETE) {
            fill(a,a->prompt->rect,surface);
            TTF_SetTextColor(a->prompt->text,text.r,text.g,text.b,255); EditBox_Draw(a->prompt);
        }
        fill(a,(SDL_FRect){24,a->height/2.0f+48,80,32},surface);
        label(a,"确认",38,a->height/2.0f+52,text);
        fill(a,(SDL_FRect){116,a->height/2.0f+48,80,32},surface);
        label(a,"取消",130,a->height/2.0f+52,text);
    }
    if(a->screenshot && a->frames==2) {
        SDL_Surface *shot=SDL_RenderReadPixels(a->renderer,NULL);
        if(shot) { SDL_SaveBMP(shot,a->screenshot); SDL_DestroySurface(shot); }
    }
    SDL_RenderPresent(a->renderer);
}

SDL_AppResult SDL_AppInit(void **state, int argc, char **argv) {
    App *a=calloc(1,sizeof(*a)); if(!a) return SDL_APP_FAILURE; *state=a;
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
    SDL_free(default_path);
    if(result!=NOTE_OK) { SDL_Log("Notes: %s",a->store.error); return SDL_APP_FAILURE; }
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
    return SDL_APP_CONTINUE;
}
SDL_AppResult SDL_AppEvent(void *state, SDL_Event *event) {
    if (platform_web_input_handle_event(event, SDL_AppEvent, state)) return SDL_APP_CONTINUE;
    App *a=state;
    if(event->type==SDL_EVENT_QUIT) {
        if (a->export_job) { message(a, "请先完成或取消文件导出"); return SDL_APP_CONTINUE; }
        return save_document(a)?SDL_APP_SUCCESS:SDL_APP_CONTINUE;
    }
    if(event->type==SDL_EVENT_WILL_ENTER_BACKGROUND) { save_document(a); return SDL_APP_CONTINUE; }
    SDL_ConvertEventToRenderCoordinates(a->renderer,event);
    layout(a);
    if (a->resizing_sidebar) {
        if (event->type == SDL_EVENT_MOUSE_MOTION) a->sidebar_width = SDL_clamp(event->motion.x, 150, SDL_min(600, a->width * 0.72f));
        if (event->type == SDL_EVENT_MOUSE_BUTTON_UP || event->type == SDL_EVENT_WINDOW_FOCUS_LOST) {
            a->resizing_sidebar = false; SDL_CaptureMouse(false);
        }
        if (event->type == SDL_EVENT_MOUSE_MOTION || event->type == SDL_EVENT_MOUSE_BUTTON_UP) return SDL_APP_CONTINUE;
    }
    if(a->prompt_mode!=PROMPT_NONE) {
        if(event->type==SDL_EVENT_KEY_DOWN && event->key.key==SDLK_RETURN && !a->prompt->composition_length) accept_prompt(a);
        else if(event->type==SDL_EVENT_KEY_DOWN && event->key.key==SDLK_ESCAPE) close_prompt(a);
        else if(event->type==SDL_EVENT_MOUSE_BUTTON_DOWN && contains((SDL_FRect){24,a->height/2.0f+48,80,32},event->button.x,event->button.y)) accept_prompt(a);
        else if(event->type==SDL_EVENT_MOUSE_BUTTON_DOWN && contains((SDL_FRect){116,a->height/2.0f+48,80,32},event->button.x,event->button.y)) close_prompt(a);
        else if(a->prompt_mode!=PROMPT_DELETE) {
            if(event->type==SDL_EVENT_KEY_DOWN && (event->key.mod&SDL_KMOD_GUI)) event->key.mod|=SDL_KMOD_CTRL;
            EditBox_HandleEvent(a->prompt,event);
        }
        return SDL_APP_CONTINUE;
    }
    if (a->active && a->edit->composition_length) {
        if (event->type == SDL_EVENT_KEY_DOWN) {
            if (event->key.key == SDLK_ESCAPE) { SDL_ClearComposition(a->window); sync_view(a); }
            return SDL_APP_CONTINUE;
        }
        if (event->type == SDL_EVENT_MOUSE_BUTTON_DOWN) { SDL_ClearComposition(a->window); sync_view(a); }
    }
    if(event->type==SDL_EVENT_MOUSE_BUTTON_DOWN) {
        if (a->sidebar && a->width >= 700 && contains((SDL_FRect){a->sidebar_area.w-4,a->sidebar_area.y,8,a->sidebar_area.h},event->button.x,event->button.y)) {
            a->resizing_sidebar = true; SDL_CaptureMouse(true); return SDL_APP_CONTINUE;
        }
        for(size_t i=0;i<SDL_arraysize(a->buttons);++i) if(contains(a->buttons[i].box,event->button.x,event->button.y)) {
            action(a,a->buttons[i].action); return SDL_APP_CONTINUE;
        }
        if(a->sidebar && contains(a->sidebar_area,event->button.x,event->button.y)) {
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
    free(a->active); free(a->search_keyword);
    free(a->saving_name); free(a->saving_snapshot);
    platform_export_free(a->export_job);
    if(a->engine) TTF_DestroyRendererTextEngine(a->engine);
    if(a->ui_font) TTF_CloseFont(a->ui_font);
    if(a->font) TTF_CloseFont(a->font);
    if(a->emoji_ui_font) TTF_CloseFont(a->emoji_ui_font);
    if(a->emoji_font) TTF_CloseFont(a->emoji_font);
    if(a->renderer) SDL_DestroyRenderer(a->renderer);
    if(a->window) SDL_DestroyWindow(a->window);
    TTF_Quit(); SDL_Quit(); free(a);
}
