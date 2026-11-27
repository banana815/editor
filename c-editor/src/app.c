/* Note Lab: shared C application, used by SDL's desktop/mobile/web entry points. */
#define SDL_MAIN_USE_CALLBACKS 1
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#include <SDL3_ttf/SDL_ttf.h>
#include "document.h"
#include "writing_stats.h"
#include "writing_store.h"
#include "document_search.h"
#include "document_publish.h"
#include <time.h>
#include "rich_layout.h"
#include "citations.h"
#include "citation_session.h"
#include "bibliography_archive.h"
#include "bibliography_selection.h"
#include "library_panel.h"
#include "platform_import.h"
#include "platform_library_source.h"
#include "cJSON.h"
#include "csl_json.h"
#include "note_store.h"
#include "platform_persistence.h"
#include "platform_export.h"
#include "platform_web_input.h"
#include "touch_gesture.h"
#include "editor_clipboard.h"
#include "document_slice.h"
#include "editbox.h"
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#endif

typedef enum { PROMPT_NONE, PROMPT_NEW, PROMPT_FIND, PROMPT_LINK, PROMPT_DELETE,
    PROMPT_REPLACE_QUERY, PROMPT_REPLACE_VALUE, PROMPT_SEARCH_ALL,
    PROMPT_FOLDER, PROMPT_TAGS, PROMPT_GOAL, PROMPT_NEW_FOLDER, PROMPT_TAG_FILTER, PROMPT_RESTORE_NAME,
    PROMPT_LIBRARY_SEARCH, PROMPT_CITATION_PAGE, PROMPT_CITATION_NOTE, PROMPT_LIBRARY_SOURCE } Prompt;
typedef enum { LIB_CLOSE, LIB_IMPORT, LIB_ITEMS, LIB_CURRENT, LIB_SEARCH,
    LIB_COLLECTIONS, LIB_ALL, LIB_DESCENDANTS, LIB_PAGE, LIB_NOTE, LIB_INSERT,
    LIB_OPEN_ITEM, LIB_OPEN_PDF, LIB_STYLE, LIB_SYNC, LIB_VIEW, LIB_RETRY,
    LIB_CHECK_ALL, LIB_CHECK_NONE, LIB_BIBLIOGRAPHY, LIB_SOURCE, LIB_SOURCE_REFRESH, LIB_SOURCE_CANCEL, LIB_BUTTON_COUNT } LibraryAction;
typedef enum { ACT_SIDEBAR, ACT_NEW, ACT_SAVE, ACT_UNDO, ACT_REDO, ACT_BOLD,
    ACT_ITALIC, ACT_UNDERLINE, ACT_HIGHLIGHT, ACT_RED, ACT_LINK, ACT_BULLETS,
    ACT_NUMBERS, ACT_HEADING, ACT_FIND, ACT_THEME, ACT_TYPEWRITER, ACT_JSON,
    ACT_DELETE, ACT_REFRESH, ACT_PARAGRAPH, ACT_CLEAR, ACT_LEFT, ACT_CENTER,
    ACT_RIGHT, ACT_JUSTIFY, ACT_FIND_PREVIOUS, ACT_FIND_NEXT, ACT_MOVE_UP,
    ACT_MOVE_DOWN, ACT_INDENT, ACT_OUTDENT, ACT_MORE, ACT_FOCUS, ACT_OUTLINE, ACT_TOOLS,
    ACT_MANAGE, ACT_REPLACE, ACT_REPLACE_ALL, ACT_SEARCH_ALL, ACT_COPY_MD, ACT_COPY_HTML, ACT_EXPORT_MD, ACT_EXPORT_HTML } Action;
typedef struct { const char *label; Action action; SDL_FRect box; } Button;
typedef struct EditorClipboardRequest EditorClipboardRequest;
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
    PlatformLibrarySource *source_job;
    char *source_pending_config;
    PlatformImportKind import_kind;
    char *pending_note_json, *pending_note_stem;
    CeLibraryPanel library;
    bool library_open, library_citation_mode, library_dragging, library_drag_moved, library_pressed_check;
    float library_drag_y, library_drag_origin, library_drag_origin_x, library_outer_scroll, library_content_height;
    size_t library_insert_offset, library_pressed_row;
    CeCitationProfile citation_style;
    CeCitationSession *citation_session;
    uint64_t citation_revision;
    bool citation_normalized_pending;
    char citation_warning[512], edit_error[512];
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
    uint64_t prompt_generation;
    size_t selection_start, selection_end;
    EditorClipboardRequest *clipboard_event;
    unsigned typing_mask, typing_enabled;
    size_t typing_anchor;
    uint64_t typing_revision;
    char *typing_color, *typing_highlight, *typing_link;
    SDL_FRect editor_area, sidebar_area, note_import_button, prompt_area, prompt_input_area, prompt_accept, prompt_cancel;
    int width, height;
    float scroll, sidebar_scroll, sidebar_width, prompt_scroll;
    CeTouchGesture touch;
    uint64_t touch_revision, touch_last_tick;
    int touch_select_anchor, touch_menu_pressed;
    bool touch_ui_active, touch_menu_visible;
    SDL_TouchID touch_ui_device;
    SDL_FingerID touch_ui_finger;
    SDL_FRect touch_menu_buttons[4];
    int toolbar_page;
    bool resizing_sidebar;
    bool dark, sidebar, sidebar_manual, typewriter, smoke;
    bool focus, outline, tools;
    SDL_FRect focus_button, outline_button, tools_button;
    CeWritingStats writing_stats;
    uint64_t stats_revision;
    size_t stats_start, stats_end;
    bool stats_valid;
    CeWritingStore writing_store;
    CeArticleInfo article_info;
    bool manager_open, paragraph_focus;
    int manager_mode, manager_selected, status_filter;
    SDL_FRect manager_area, manager_buttons[14], manager_rows, manager_preview, manager_close;
    float manager_scroll, manager_preview_scroll, manager_preview_height;
    CeArchiveList manager_archives;
    char *manager_preview_text, *manager_archive_data, *manager_archive_name;
    char folder_filter[256], tag_filter[512];
    size_t *visible_files, visible_count;
    Uint64 history_at;
    uint64_t goal_revision;
    size_t goal_characters;
    bool goal_valid;
    bool manager_pointer,manager_moved,manager_preview_drag;
    float manager_pointer_x,manager_pointer_y,manager_pointer_last_y;
    char *replace_query,*replace_text,*all_search_query;
    bool replace_all,search_open;
    size_t search_count,search_skipped;
    SearchHit *search_hits;
    SDL_FRect search_area,search_close,search_again;
    float search_scroll;
    bool search_dragging,search_drag_moved;
    float search_drag_x,search_drag_y,search_drag_scroll;
    size_t search_pressed_row;
    Uint64 undo_typed_at;
    uint64_t undo_revision;
    size_t undo_caret;
    bool undo_typing;
    bool link_click_pending;
    int link_click_offset;
    float link_click_x,link_click_y;
    Uint64 link_click_ready_at;
    uint64_t link_click_revision;
    Uint64 edited_at;
    unsigned frames;
    const char *screenshot;
    Button buttons[41];
} App;

static void reveal_cursor(App *a);
static void editing_break(App *a);
static void layout(App *a);
static size_t outline_count(App *a);
static bool manager_ready(App *a);
static bool manager_filter_files(App *a);
static void manager_load_article(App *a);
static bool manager_checkpoint(App *a,bool force);
static void manager_show(App *a,int mode);
static bool manager_prompt_accept(App *a,const char *value);
static void clipped_label(App *a,const char *text,SDL_FRect rect,SDL_Color color);
static void editor_clipboard_action(App *a, CeEditorClipboardAction action);
static bool typing_set(App *a,size_t offset,CeMark mark,const char *value,bool enabled);
static void library_show(App *a);
static void library_close(App *a);
static void library_layout(App *a);
static void library_draw(App *a);
static void library_event(App *a, SDL_Event *event);
static void library_import_poll(App *a);
static bool library_prompt_accept(App *a, const char *value);
static bool library_cache_save(App *a);
static bool library_cache_load(App *a, bool startup);
static bool library_source_begin(App *a, const char *kind, const char *location);
static void library_source_refresh(App *a);
static void library_source_poll(App *a);
static char *library_source_config(const char *kind,const char *location);
static cJSON *library_source_read(App *a);
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
static void citation_diagnostics(App *a) {
    const CeCitationProcessStats *stats=ce_citation_prepared_stats(ce_citation_session_prepared(a->citation_session));
    if(!stats) return;
    const char *warning=stats->missing_term?"引用样式缺少所需术语，请检查格式结果":
        stats->malformed_markup?"文献字段含未闭合格式标记，已按原文保留，请检查":
        stats->literal_markup?"文献字段含不支持的格式标记，已按原文保留，请检查":"";
    SDL_strlcpy(a->citation_warning,warning,sizeof a->citation_warning);
}
static CeDocument *edit_candidate(App *a) {
    char *json=ce_document_export_json(a->doc);
    CeDocument *candidate=ce_document_new();
    SDL_strlcpy(a->edit_error,"内存不足，原文保持不变",sizeof a->edit_error);
    bool ok=json && candidate && ce_document_import(candidate,json,CE_FORMAT_JSON,a->edit_error,sizeof a->edit_error);
    free(json);
    if(!ok) { ce_document_free(candidate); return NULL; }
    return candidate;
}
static bool edit_commit(App *a,CeDocument *candidate,bool changed,
    const CeCitationProfile *override,bool force) {
    if(!candidate) return false;
    if(!changed) SDL_strlcpy(a->edit_error,ce_document_error(candidate),sizeof a->edit_error);
    bool ok=changed && ce_citation_session_apply(&a->citation_session,a->doc,candidate,
        &a->library.library,override,force,a->edit_error,sizeof a->edit_error);
    ce_document_free(candidate);
    if(ok) {
        ++a->citation_revision;
        if(a->citation_session) {
            a->citation_style=ce_citation_session_profile(a->citation_session);
            citation_diagnostics(a);
        }
    }
    return ok;
}
static bool edit_replace(App *a,size_t start,size_t end,const char *text) {
    CeDocument *candidate=edit_candidate(a);
    bool pending=a->typing_mask && a->typing_revision==a->citation_revision &&
        start==end && start==a->typing_anchor && *text;
    bool changed=candidate && ce_document_replace(candidate,start,end,text);
    if(changed && pending) {
        const char *before=ce_document_text(a->doc),*after=ce_document_text(candidate);
        size_t old_size=strlen(before),new_size=strlen(after),prefix=0,suffix=0;
        while(prefix<start && prefix<new_size && before[prefix]==after[prefix]) ++prefix;
        while(suffix<old_size-end && suffix<new_size-prefix &&
            before[old_size-suffix-1]==after[new_size-suffix-1]) ++suffix;
        for(unsigned mark=CE_MARK_BOLD;changed && mark<=CE_MARK_LINK;mark<<=1) {
            if(!(a->typing_mask&mark)) continue;
            const char *value=mark==CE_MARK_COLOR?a->typing_color:mark==CE_MARK_HIGHLIGHT?a->typing_highlight:
                mark==CE_MARK_LINK?a->typing_link:NULL;
            changed=ce_document_style(candidate,prefix,new_size-suffix,(CeMark)mark,value,(a->typing_enabled&mark)!=0);
        }
    }
    bool ok=edit_commit(a,candidate,changed,NULL,false);
    if(ok && pending) { a->typing_revision=a->citation_revision; a->typing_anchor=start+strlen(text); }
    return ok;
}
static bool edit_style(App *a,size_t start,size_t end,CeMark mark,const char *value,bool enabled) {
    CeDocument *candidate=edit_candidate(a);
    return edit_commit(a,candidate,candidate && ce_document_style(candidate,start,end,mark,value,enabled),NULL,false);
}
static bool edit_clear(App *a,size_t start,size_t end) {
    CeDocument *candidate=edit_candidate(a);
    return edit_commit(a,candidate,candidate && ce_document_clear_block_formatting(candidate,start,end),NULL,false);
}
static bool edit_alignment(App *a,size_t start,size_t end,const char *alignment) {
    CeDocument *candidate=edit_candidate(a);
    return edit_commit(a,candidate,candidate && ce_document_set_alignment(candidate,start,end,alignment),NULL,false);
}
static bool edit_block(App *a,size_t start,size_t end,CeBlockKind kind,unsigned level) {
    CeDocument *candidate=edit_candidate(a);
    return edit_commit(a,candidate,candidate && ce_document_set_block(candidate,start,end,kind,level),NULL,false);
}
static bool edit_move(App *a,size_t from,size_t to,size_t *offset) {
    CeDocument *candidate=edit_candidate(a); size_t position=0;
    bool ok=candidate && ce_document_move_block_ex(candidate,from,to,&position);
    size_t ordinal=0; bool anchored=false;
    for(size_t i=0;ok && i<ce_document_block_count(candidate);i++) {
        CeBlockInfo block;
        if(!ce_document_block_info(candidate,i,&block) || block.kind==CE_BLOCK_ENDNOTES) continue;
        if(block.start==position) { anchored=true; break; }
        ++ordinal;
    }
    ok=edit_commit(a,candidate,ok,NULL,false);
    if(ok && anchored) {
        size_t current=0;
        for(size_t i=0;i<ce_document_block_count(a->doc);i++) {
            CeBlockInfo block;
            if(!ce_document_block_info(a->doc,i,&block) || block.kind==CE_BLOCK_ENDNOTES) continue;
            if(current++==ordinal) { position=block.start; break; }
        }
    }
    if(ok && offset) *offset=position;
    return ok;
}
static void citation_refresh(App *a) {
    CeCitationSession *next=NULL;
    bool ok=ce_citation_session_build(a->doc,&a->library.library,NULL,&next,
        a->citation_warning,sizeof a->citation_warning);
    ce_citation_session_free(a->citation_session); a->citation_session=next;
    if(ok) { a->citation_style=ce_citation_session_profile(next); citation_diagnostics(a); }
    ++a->citation_revision;
    if(!ok) message(a,a->citation_warning);
}
/* Only freshly evaluated C output is a display authority. Imported JSON cache
 * fields remain document data until the whole source revision is processed. */
static CeRichCitationStatus citation_resolve(void *context,const cJSON *atom,
    const CeCitationCache **cache,char *error,size_t capacity) {
    App *a=context; (void)error; (void)capacity;
    if(ce_bibliography_archive_eligible(atom)) {
        *cache=NULL; return CE_RICH_CITATION_ARCHIVED;
    }
    const CeCitationPrepared *prepared=ce_citation_session_prepared(a->citation_session);
    if(!prepared) return CE_RICH_CITATION_UNSUPPORTED;
    const cJSON *type=cJSON_GetObjectItemCaseSensitive(atom,"type");
    if(cJSON_IsString(type) && !strcmp(type->valuestring,"zoteroEndnoteSection"))
        *cache=ce_citation_prepared_bibliography(prepared);
    else {
        const cJSON *attrs=cJSON_GetObjectItemCaseSensitive(atom,"attrs");
        const cJSON *id=cJSON_GetObjectItemCaseSensitive(attrs,"citationId");
        *cache=cJSON_IsString(id)?ce_citation_prepared_citation(prepared,id->valuestring):NULL;
    }
    return *cache?CE_RICH_CITATION_VALID:CE_RICH_CITATION_ABSENT;
}
static char *citation_export(App *a,bool html) {
    a->edit_error[0]='\0';
    if(!a->citation_session)
        return html?ce_document_export_html(a->doc):ce_document_export_json(a->doc);
    CeCitationPrepared *snapshot=NULL;
    if(!ce_citation_session_snapshot(a->citation_session,a->doc,&a->library.library,
        &snapshot,a->edit_error,sizeof a->edit_error)) return NULL;
    char *result=NULL;
    if(!html) result=copy_string(ce_citation_prepared_json(snapshot));
    else {
        CeDocument *temporary=ce_document_new();
        if(temporary && ce_document_import(temporary,ce_citation_prepared_json(snapshot),CE_FORMAT_JSON,
            a->edit_error,sizeof a->edit_error)) result=ce_document_export_html(temporary);
        if(temporary && !result) SDL_strlcpy(a->edit_error,ce_document_error(temporary),sizeof a->edit_error);
        ce_document_free(temporary);
    }
    ce_citation_prepared_free(snapshot);
    if(!result && !*a->edit_error) SDL_strlcpy(a->edit_error,"内存不足，未导出",sizeof a->edit_error);
    return result;
}
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
    SDL_snprintf(title, sizeof(title), "%s%s — Note Lab", (ce_document_is_modified(a->doc) || a->citation_normalized_pending) ? "* " : "", a->active ? a->active : "C Editor");
    SDL_SetWindowTitle(a->window, title);
#ifdef __EMSCRIPTEN__
    EM_ASM({ Module.editorUnsaved = !!$0; }, ce_document_is_modified(a->doc) || a->citation_normalized_pending || a->persistence_error || a->library_unsaved);
#endif
}
static bool refresh_files(App *a) {
    NoteList next = {0};
    if (note_store_list(&a->store, &next) != NOTE_OK) { message(a, a->store.error); return false; }
    NoteList previous=a->files;
    a->files = next;
    if(!manager_filter_files(a)) {
        note_store_list_free(&a->files); a->files=previous; return false;
    }
    note_store_list_free(&previous);
    return true;
}
static void sync_view(App *a) {
    a->rich_dirty = true;
    a->stats_valid = false;
    a->goal_valid = false;
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
        if (current && !strcmp(current, a->saving_snapshot)) {
            ce_document_mark_saved(a->doc); a->citation_normalized_pending=false;
        }
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
    a->undo_typing=false;
    if (a->saving) { message(a, "正在保存，请稍候"); return false; }
    if (!a->active || (!ce_document_is_modified(a->doc) && !a->citation_normalized_pending)) {
        if (a->persistence_error) platform_persist(structure_persisted, a);
        return !platform_persist_pending() && !a->persistence_error;
    }
    /* Preedit text belongs to the IME, not to the saved document. */
    char *data = citation_export(a,SDL_strcasecmp(strrchr(a->active, '.'), ".json") != 0);
    if (!data) { message(a, *a->edit_error?a->edit_error:ce_document_error(a->doc)); return false; }
    char *snapshot = ce_document_export_json(a->doc);
    char *name = copy_string(a->active);
    if (!snapshot || !name) { free(data); free(snapshot); free(name); message(a, "内存不足，未保存"); return false; }
    if(!manager_checkpoint(a,false)) { free(data); free(snapshot); free(name); return false; }
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
    char error[512]="内存不足";
    CeDocument *candidate=ce_document_new();
    bool ok = candidate && ce_document_import(candidate, data, CE_FORMAT_AUTO, error, sizeof(error));
    note_store_free(data);
    char *original=ok?ce_document_export_json(candidate):NULL;
    if(ok && !original) SDL_strlcpy(error,"内存不足，未打开笔记",sizeof error);
    CeCitationSession *next_session=NULL;
    char citation_warning[512]="";
    bool processed=ok && original && ce_citation_session_build(candidate,&a->library.library,NULL,
        &next_session,citation_warning,sizeof citation_warning);
    const char *replacement=processed?ce_citation_prepared_json(ce_citation_session_prepared(next_session)):original;
    ok=ok && original && replacement && ce_document_import(a->doc,replacement,CE_FORMAT_JSON,error,sizeof error);
    bool normalized=ok && processed && strcmp(original,replacement)!=0;
    free(original); ce_document_free(candidate);
    if (!ok) { ce_citation_session_free(next_session); free(next_name); message(a, error); return; }
    ce_citation_session_free(a->citation_session); a->citation_session=next_session;
    a->citation_normalized_pending=normalized;
    SDL_strlcpy(a->citation_warning,citation_warning,sizeof a->citation_warning);
    a->citation_style=ce_citation_session_profile(next_session); ++a->citation_revision;
    if(processed) citation_diagnostics(a);
    free(a->active); a->active = next_name;
    manager_load_article(a);
    ce_document_mark_saved(a->doc);
    a->scroll = 0;
    a->edit->cursor = 0;
    a->edit->highlight1 = a->edit->highlight2 = -1;
    sync_view(a);
    EditBox_SetFocus(a->edit, true);
    if (a->width < 700) a->sidebar = false;
    a->edited_at=SDL_GetTicks();
    message(a, *a->citation_warning?a->citation_warning:*a->library_warning ? a->library_warning : "已打开 · 自动保存开启");
}
static void get_selection(App *a, size_t *start, size_t *end) {
    if (a->edit->highlight1 >= 0 && a->edit->highlight2 >= 0) {
        *start = (size_t)SDL_min(a->edit->highlight1, a->edit->highlight2);
        *end = (size_t)SDL_max(a->edit->highlight1, a->edit->highlight2);
    } else { *start = *end = (size_t)a->edit->cursor; }
}
#include "app_editing.inc"
static void show_prompt(App *a, Prompt kind) {
    editing_break(a);
    ++a->prompt_generation;
    if (kind < PROMPT_LIBRARY_SEARCH) get_selection(a, &a->selection_start, &a->selection_end);
    const char *link=kind==PROMPT_LINK?editing_link_range(a,&a->selection_start,&a->selection_end):NULL;
    a->prompt_mode = kind;
    a->prompt_scroll=0;
    EditBox_SetFocus(a->edit, false);
    /* Cancel uncommitted input when changing input controls. */
    SDL_ClearComposition(a->window);
    if (kind < PROMPT_LIBRARY_SEARCH) sync_view(a);
    EditBox_SetText(a->prompt,link?link:"");
    a->prompt->cursor = 0;
    a->prompt->highlight1 = a->prompt->highlight2 = -1;
    if(link) { a->prompt->highlight1=0; a->prompt->highlight2=a->prompt->cursor=(int)strlen(link); }
    a->prompt->composition_length = 0;
    EditBox_SetFocus(a->prompt, kind != PROMPT_DELETE);
}
static void close_prompt(App *a) {
    ++a->prompt_generation;
    EditBox_SetFocus(a->prompt, false);
    SDL_ClearComposition(a->window);
    a->prompt_mode = PROMPT_NONE;
    EditBox_SetFocus(a->edit, a->active != NULL && !a->library_open && !a->manager_open && !a->search_open);
}
static void mark_changed(App *a) { a->rich_dirty = true; a->edited_at = SDL_GetTicks(); update_title(a); }
#include "app_publish.inc"
#include "app_search.inc"
static void accept_prompt(App *a) {
    if (a->prompt->composition_length) { message(a, "请先完成输入法选词"); return; }
    const char *value = a->prompt->text->text ? a->prompt->text->text : "";
    if(search_prompt_accept(a,value) || manager_prompt_accept(a,value)) return;
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
        bool ok=a->selection_start==a->selection_end?
            typing_set(a,a->selection_start,CE_MARK_LINK,value,*value!='\0'):
            edit_style(a,a->selection_start,a->selection_end,CE_MARK_LINK,value,*value!='\0');
        if (!ok)
            message(a, a->edit_error);
        else if(a->selection_start!=a->selection_end) mark_changed(a);
    } else if (a->prompt_mode == PROMPT_DELETE && a->active) {
        if(!save_document(a) || !manager_ready(a)) return;
        if(!ce_writing_trash_note(&a->writing_store,&a->store,a->active,(int64_t)time(NULL))) { message(a,a->writing_store.error); return; }
        free(a->active); a->active = NULL;
        ce_citation_session_free(a->citation_session); a->citation_session=NULL;
        a->citation_normalized_pending=false; a->citation_warning[0]='\0'; ++a->citation_revision;
        char error[256];
        ce_document_import(a->doc, "", CE_FORMAT_TEXT, error, sizeof(error));
        ce_document_mark_saved(a->doc); sync_view(a); refresh_files(a); manager_load_article(a); platform_persist(structure_persisted,a); message(a, "文章已移入回收站，可从文章管理恢复");
    }
    close_prompt(a);
    reveal_cursor(a);
}
/* Browser clipboard access must use the DOM clipboard event. Return an owned
 * UTF-8 snapshot and delete only if the selection still matches that snapshot. */
static EditBox *clipboard_edit(App *a) {
    EditBox *edit = a->prompt_mode != PROMPT_NONE ? a->prompt : a->edit;
    bool touch_selection=edit==a->edit && a->touch_menu_visible && a->active;
    if ((!edit->has_focus && !touch_selection) || edit->composition_length || edit->composition_snapshot) return NULL;
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
    if (!edit_replace(a, from, to, "")) { message(a, a->edit_error); return false; }
    edit->cursor = (int)from; sync_view(a); mark_changed(a); reveal_cursor(a);
    return true;
}
static void browser_clipboard_error(void *state) {
    message(state, "无法访问剪贴板，原文保持不变");
}
#include "app_clipboard.inc"
static bool typing_current(App *a,size_t offset) {
    return a->typing_mask && a->typing_revision==a->citation_revision && a->typing_anchor==offset;
}
static CeStyle typing_style(App *a,size_t offset) {
    CeStyle style=ce_document_style_at(a->doc,offset);
    /* Match the model's left-biased insertion inheritance at text boundaries. */
    for(size_t i=0;i<ce_document_span_count(a->doc);++i) {
        CeStyleSpan span;
        if(ce_document_span(a->doc,i,&span) && offset>=span.start && offset<=span.end) { style=span.style; break; }
    }
    if(typing_current(a,offset)) {
        style.marks=(style.marks&~a->typing_mask)|a->typing_enabled;
        if(a->typing_mask&CE_MARK_COLOR) style.color=a->typing_color;
        if(a->typing_mask&CE_MARK_HIGHLIGHT) style.highlight=a->typing_highlight;
        if(a->typing_mask&CE_MARK_LINK) style.link=a->typing_link;
    }
    return style;
}
static bool typing_set(App *a,size_t offset,CeMark mark,const char *value,bool enabled) {
    char **slot=mark==CE_MARK_COLOR?&a->typing_color:mark==CE_MARK_HIGHLIGHT?&a->typing_highlight:
        mark==CE_MARK_LINK?&a->typing_link:NULL;
    char *owned=slot && enabled && value?copy_string(value):NULL;
    if(slot && enabled && value && !owned) { SDL_strlcpy(a->edit_error,"内存不足，输入格式保持不变",sizeof a->edit_error); return false; }
    if(!typing_current(a,offset)) { a->typing_mask=a->typing_enabled=0; }
    if(slot) { free(*slot); *slot=owned; }
    a->typing_mask|=mark;
    if(enabled) a->typing_enabled|=mark; else a->typing_enabled&=~(unsigned)mark;
    a->typing_anchor=offset; a->typing_revision=a->citation_revision;
    message(a,"已设置接下来输入文字的格式"); return true;
}
static void format_selection(App *a, CeMark mark, const char *value) {
    size_t start, end; get_selection(a, &start, &end);
    if (start == end) {
        CeStyle pending=typing_style(a,start);
        if(!typing_set(a,start,mark,value,!(pending.marks&mark))) message(a,a->edit_error);
        return;
    }
    CeStyle style = ce_document_style_at(a->doc, start);
    if (!edit_style(a, start, end, mark, value, !(style.marks & mark)))
        message(a, a->edit_error);
    else mark_changed(a);
}
#include "app_writing_commands.inc"
static void action(App *a, Action which) {
    editing_break(a);
    if (which == ACT_FOCUS) {
        a->focus = !a->focus; a->touch_menu_visible=false;
        layout(a); reveal_cursor(a);
        message(a,a->focus?"专注写作 · Esc 退出 · Ctrl/Cmd+Shift+F 切换":"已退出专注写作"); return;
    }
    if (which == ACT_TOOLS) { a->tools = !a->tools; a->toolbar_page=0; layout(a); reveal_cursor(a); return; }
    if (which == ACT_OUTLINE) {
        a->outline = !a->outline; a->sidebar=true; a->sidebar_manual=true; a->sidebar_scroll=0;
        layout(a); reveal_cursor(a); return;
    }
    if (which == ACT_MORE) { ++a->toolbar_page; return; }
    if (which == ACT_SIDEBAR) { a->sidebar = !a->sidebar; a->sidebar_manual = true; return; }
    if (which == ACT_NEW) { show_prompt(a, PROMPT_NEW); return; }
    if (which == ACT_MANAGE) { manager_show(a,0); return; }
    if (which == ACT_SEARCH_ALL) { search_begin_all(a); return; }
    if (which == ACT_REFRESH) { if (refresh_files(a)) message(a, "笔记列表已刷新"); return; }
    if (which == ACT_THEME) { a->dark = !a->dark; return; }
    if (which == ACT_TYPEWRITER) { a->typewriter = !a->typewriter; return; }
    if (!a->active) { message(a, "请先新建或打开笔记"); return; }
    switch (which) {
        case ACT_REPLACE: search_begin_replace(a,false); break;
        case ACT_REPLACE_ALL: search_begin_replace(a,true); break;
        case ACT_COPY_MD: publish_action(a,CE_PUBLISH_MARKDOWN,true); break;
        case ACT_COPY_HTML: publish_action(a,CE_PUBLISH_HTML,true); break;
        case ACT_EXPORT_MD: publish_action(a,CE_PUBLISH_MARKDOWN,false); break;
        case ACT_EXPORT_HTML: publish_action(a,CE_PUBLISH_HTML,false); break;
        case ACT_SAVE: save_document(a); break;
        case ACT_UNDO: if (ce_document_undo(a->doc)) { citation_refresh(a); sync_view(a); mark_changed(a); } break;
        case ACT_REDO: if (ce_document_redo(a->doc)) { citation_refresh(a); sync_view(a); mark_changed(a); } break;
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
            if(!edit_clear(a,start,end)) { message(a,a->edit_error); break; }
            mark_changed(a);
            if (start == end) {
                a->typing_mask=CE_MARK_BOLD|CE_MARK_ITALIC|CE_MARK_UNDERLINE|CE_MARK_COLOR|CE_MARK_HIGHLIGHT|CE_MARK_LINK;
                a->typing_enabled=0; a->typing_anchor=start; a->typing_revision=a->citation_revision;
                message(a,"已清除段落格式，接下来输入的文字不带格式"); break;
            }
            break;
        }
        case ACT_INDENT: case ACT_OUTDENT: {
            size_t start,end; get_selection(a,&start,&end); bool changed=false;
            int cursor=a->edit->cursor,selection1=a->edit->highlight1,selection2=a->edit->highlight2;
            CeDocument *candidate=edit_candidate(a);
            if(!candidate) { message(a,a->edit_error); break; }
            if(!ce_document_adjust_list_depth(candidate,start,end,which==ACT_INDENT?1:-1,&changed)) {
                message(a,ce_document_error(candidate)); ce_document_free(candidate); break;
            }
            if(!changed) { ce_document_free(candidate); message(a,"当前位置没有可调整层级的列表项"); break; }
            if(!edit_commit(a,candidate,true,NULL,false)) { message(a,a->edit_error); break; }
            sync_view(a); a->edit->cursor=cursor; a->edit->highlight1=selection1; a->edit->highlight2=selection2;
            mark_changed(a); reveal_cursor(a); break;
        }
        case ACT_LEFT: case ACT_CENTER: case ACT_RIGHT: case ACT_JUSTIFY: {
            const char *alignment = which == ACT_LEFT ? "left" : which == ACT_CENTER ? "center" : which == ACT_RIGHT ? "right" : "justify";
            size_t start, end; get_selection(a, &start, &end);
            if (edit_alignment(a, start, end, alignment)) mark_changed(a);
            else message(a, a->edit_error);
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
            if (edit_move(a, index, target, &offset)) {
                sync_view(a);
                a->edit->cursor = (int)(offset+relative);
                mark_changed(a); reveal_cursor(a);
            } else message(a, a->edit_error);
            break;
        }
        case ACT_DELETE: show_prompt(a, PROMPT_DELETE); break;
        case ACT_BULLETS: case ACT_NUMBERS: case ACT_HEADING: case ACT_PARAGRAPH: {
            if (which==ACT_BULLETS || which==ACT_NUMBERS)
                writing_toggle_list(a,which==ACT_BULLETS ? CE_BLOCK_BULLET : CE_BLOCK_ORDERED);
            else writing_set_block(a,which==ACT_PARAGRAPH ? CE_BLOCK_PARAGRAPH : CE_BLOCK_HEADING,2);
            break;
        }
        case ACT_JSON: {
            if (a->export_job) { message(a, "正在导出，请先完成文件选择"); break; }
            char *json = citation_export(a,false);
            if (!json) { message(a, a->edit_error); break; }
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
static void reconcile_input(App *a,size_t prior_start,size_t prior_end,bool deleting,bool grouping) {
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
    /* An insertion among identical characters has multiple equivalent text
     * diffs. Prefer the actual old selection so its text marks stay attached to
     * the user's caret instead of drifting to the last matching character. */
    if(prior_start<=prior_end && prior_end<=old_len && prior_start<=new_len &&
       old_len-prior_end<=new_len-prior_start && !memcmp(old,next,prior_start) &&
       !memcmp(old+prior_end,next+new_len-(old_len-prior_end),old_len-prior_end)) {
        prefix=prior_start; old_end=prior_end; new_end=new_len-(old_len-prior_end);
    }
    if(deleting && prior_start==prior_end && new_len<old_len) {
        size_t caret=(size_t)SDL_max(0,a->edit->cursor),removed=old_len-new_len;
        if(caret<=prior_start && caret<=new_len && removed<=old_len-caret &&
           !memcmp(old,next,caret) && !memcmp(old+caret+removed,next+caret,new_len-caret)) {
            prefix=new_end=caret; old_end=caret+removed;
        }
    }
    char *insert = malloc(new_end - prefix + 1);
    if (!insert) { editing_break(a); message(a, "内存不足"); sync_view(a); return; }
    memcpy(insert, next + prefix, new_end - prefix); insert[new_end-prefix] = '\0';
    grouping=grouping && !deleting && prior_start==prior_end && prefix==prior_start &&
        old_end==prior_start && *insert && !strchr(insert,'\n') && !strchr(insert,'\r');
    uint64_t before=ce_document_revision(a->doc);
    bool ok = edit_replace(a, prefix, old_end, insert);
    free(insert);
    if (!ok) { editing_break(a); message(a, a->edit_error); sync_view(a); }
    else {
        /* Structural editing (for example leaving an empty list item or typing
         * next to a citation) may normalize block separators. Keep the input
         * surface aligned with the model, preserving the caret relative to
         * the unchanged suffix when possible. */
        const char *canonical = ce_document_text(a->doc);
        if (strcmp(canonical, next)) {
            grouping=false;
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
        editing_record_input(a,before,prior_start,grouping);
        if(a->typing_mask && a->typing_revision==a->citation_revision)
            a->typing_anchor=(size_t)SDL_max(0,a->edit->cursor);
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
    CeRichCitationSource citations={a->citation_revision,a,citation_resolve};
    a->rich_valid = ce_rich_layout_update_citations(a->rich,model,width,&theme,&citations);
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
    bool ok=ce_rich_layout_draw(a->rich,x,y,&a->editor_area,(size_t)SDL_max(0,start),(size_t)SDL_max(0,end));
    if(ok && a->paragraph_focus && a->active && !a->edit->composition_length && start==end) {
        CeBlockInfo block; SDL_FRect first,last;
        if(ce_document_block_at(a->doc,(size_t)SDL_max(0,a->edit->cursor),&block) &&
           ce_rich_layout_caret(a->rich,block.start,&first) && ce_rich_layout_caret_affinity(a->rich,block.end,true,&last)) {
            SDL_Color dim=paper(a); dim.a=175;
            float top=SDL_clamp(y+first.y,a->editor_area.y,a->editor_area.y+a->editor_area.h);
            float bottom=SDL_clamp(y+last.y+last.h,a->editor_area.y,a->editor_area.y+a->editor_area.h);
            SDL_SetRenderDrawBlendMode(a->renderer,SDL_BLENDMODE_BLEND);
            fill(a,(SDL_FRect){a->editor_area.x,a->editor_area.y,a->editor_area.w,top-a->editor_area.y},dim);
            fill(a,(SDL_FRect){a->editor_area.x,bottom,a->editor_area.w,a->editor_area.y+a->editor_area.h-bottom},dim);
            SDL_SetRenderDrawBlendMode(a->renderer,SDL_BLENDMODE_NONE);
        }
    }
    return ok;
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
    if(a->active && !a->library_citation_mode && end>a->library_insert_offset) {
        CeDocumentSlice *slice=NULL; char *plain=NULL;
        if(ce_document_slice_capture(a->doc,a->library_insert_offset,end,&slice,NULL,a->edit_error,sizeof a->edit_error) &&
           ce_document_slice_plain(slice,clipboard_atom_text,a,&plain,a->edit_error,sizeof a->edit_error)) {
            if(plain && *plain) { free(a->insert_note); a->insert_note=plain; plain=NULL; }
        } else message(a,"未能读取选中文字，已有摘录保持不变");
        free(plain); ce_document_slice_free(slice);
    }
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
    cJSON *source=NULL;
    if(kind==PROMPT_LIBRARY_SOURCE) {
        source=library_source_read(a);
        value=!strcmp(json_string(source,"kind"),"http")?json_string(source,"location"):
            "http://127.0.0.1:23119/better-bibtex/export/library?/translator=csljson";
    }
    show_prompt(a,kind); EditBox_SetText(a->prompt,value?value:"");
    a->prompt->cursor=(int)strlen(value?value:"");
    EditBox_SelectAll(a->prompt);
    cJSON_Delete(source);
}
static bool library_prompt_accept(App *a,const char *value) {
    if(a->prompt_mode==PROMPT_LIBRARY_SOURCE) return library_source_begin(a,"http",value);
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
static const char *citation_style_label(App *a) {
    return a->citation_style==CE_CITATION_PROFILE_IEEE?"IEEE 2023":a->citation_style==CE_CITATION_PROFILE_GBT?"GB/T 7714-2015":"APA 7";
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
        if(a->import_job || a->source_job || a->pending_note_json || a->library_cache_saving) { message(a,"正在读取或保存，请稍候"); return; }
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
    if(action==LIB_CHECK_ALL) {
        if(!ce_library_panel_check_visible(&a->library,true)) message(a,"请先返回文献条目列表");
        return;
    }
    if(action==LIB_CHECK_NONE) { ce_library_panel_uncheck_all(&a->library); return; }
    if(action==LIB_SOURCE) { library_prompt(a,PROMPT_LIBRARY_SOURCE); return; }
    if(action==LIB_SOURCE_REFRESH) { library_source_refresh(a); return; }
    if(action==LIB_SOURCE_CANCEL) {
        if(a->source_job) {
            platform_library_source_cancel(a->source_job); platform_library_source_free(a->source_job); a->source_job=NULL;
            free(a->source_pending_config); a->source_pending_config=NULL;
            message(a,"已取消来源读取，原文献库保留");
        }
        else message(a,"当前没有正在读取的来源");
        return;
    }
    if(action==LIB_PAGE || action==LIB_NOTE) { library_prompt(a,action==LIB_PAGE?PROMPT_CITATION_PAGE:PROMPT_CITATION_NOTE); return; }
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
    if(action==LIB_BIBLIOGRAPHY) {
        size_t count=a->library.checked_count, at=0;
        const CeZoteroItem **items=count?calloc(count,sizeof *items):NULL;
        if(count && !items) { message(a,"内存不足，参考文献保持不变"); return; }
        for(size_t i=0;i<a->library.library.item_count;i++) if(a->library.checked && a->library.checked[i]) {
            if(at<count) items[at++]=&a->library.library.items[i];
        }
        if(at!=count) { free(items); message(a,"文献选择已改变，请重新选择"); return; }
        size_t ordinal=0,relative=0; bool anchored=library_anchor(a,&ordinal,&relative);
        CeDocument *candidate=edit_candidate(a);
        bool ok=candidate && ce_bibliography_select(candidate,items,count,error,sizeof error);
        free(items);
        if(!ok) { message(a,candidate?error:a->edit_error); ce_document_free(candidate); return; }
        if(!edit_commit(a,candidate,true,NULL,true)) { message(a,a->edit_error); return; }
        sync_view(a); mark_changed(a);
        if(anchored) library_restore_anchor(a,ordinal,relative);
        a->library_insert_offset=(size_t)a->edit->cursor;
        library_close(a); reveal_cursor(a);
        message(a,count?"已按勾选条目更新独立书目，正文引用保留，可撤销":"已清空独立书目选择，正文引用保留，可撤销");
        return;
    }
    if(action==LIB_STYLE) {
        CeCitationProfile profile=(CeCitationProfile)((a->citation_style+1)%3);
        CeDocument *candidate=edit_candidate(a);
        if(!edit_commit(a,candidate,true,&profile,true)) { message(a,a->edit_error); return; }
        sync_view(a); mark_changed(a); message(a,"引用格式已更新，可撤销"); return;
    }
    if(action==LIB_SYNC) {
        size_t ordinal=0,relative=0; bool anchored=library_anchor(a,&ordinal,&relative);
        CeDocument *candidate=edit_candidate(a);
        if(!edit_commit(a,candidate,true,NULL,true)) { message(a,a->edit_error); return; }
        const CeCitationProcessStats *result=ce_citation_prepared_stats(ce_citation_session_prepared(a->citation_session));
        sync_view(a); mark_changed(a);
        if(anchored) library_restore_anchor(a,ordinal,relative);
        a->library_insert_offset=(size_t)a->edit->cursor;
        citation_context_capture(a);
        char status[256]; SDL_snprintf(status,sizeof status,"已更新 %zu 条引用 / %zu 条文献 · %s",result->citation_count,result->reference_count,citation_style_label(a));
        message(a,status); return;
    }
    if(action==LIB_INSERT) {
        if(a->library_citation_mode) {
            if(!a->citation_id || !*a->citation_id) { message(a,"未选中有效引用；缺少 ID 的旧引用可先同步尾注修复"); return; }
            CeDocument *candidate=edit_candidate(a);
            if(!candidate) { message(a,a->edit_error); return; }
            if(!ce_citation_update(candidate,a->citation_id,a->citation_page,a->citation_note,error,sizeof error)) {
                ce_document_free(candidate); message(a,error); return;
            }
            if(!edit_commit(a,candidate,true,NULL,true)) { message(a,a->edit_error); return; }
            sync_view(a); mark_changed(a); library_close(a); reveal_cursor(a); message(a,"引用页码和摘录已更新，可撤销");
        } else {
            const CeZoteroItem *item=ce_library_panel_selected(&a->library);
            if(!item) { message(a,"请先在文献列表中选择一条文献"); return; }
            char *id=NULL;
            CeDocument *candidate=edit_candidate(a);
            if(!candidate) { message(a,a->edit_error); return; }
            if(!ce_citation_insert(candidate,a->library_insert_offset,item,a->insert_page,a->insert_note,&id,error,sizeof error)) {
                ce_document_free(candidate); message(a,error); return;
            }
            if(!edit_commit(a,candidate,true,NULL,true)) { free(id); message(a,a->edit_error); return; }
            sync_view(a); size_t end;
            if(citation_position(a,id,&end)) a->edit->cursor=(int)end;
            free(id); mark_changed(a); library_close(a); reveal_cursor(a);
            message(a,"已插入引用并更新参考文献，可撤销");
        }
    }
}
static void document_import_clear(App *a) {
    free(a->pending_note_json); free(a->pending_note_stem);
    a->pending_note_json=a->pending_note_stem=NULL;
}
static void document_import_begin(App *a) {
    if(a->import_job || a->source_job || a->pending_note_json) { message(a,"正在读取，请先完成当前操作"); return; }
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

#include "app_library_source.inc"
static void library_import_poll(App *a) {
    if(!a->import_job) return;
    const char *status=NULL;
    PlatformImportStatus result=platform_import_status(a->import_job,&status);
    if(result==PLATFORM_IMPORT_PENDING) return;
    if(result==PLATFORM_IMPORT_DONE && (a->library_cache_saving || a->prompt_mode==PROMPT_CITATION_PAGE ||
       a->prompt_mode==PROMPT_CITATION_NOTE)) return;
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
        if(ok) {
            imported.client_state_json=library_source_config(file.kind==PLATFORM_IMPORT_SQLITE && file.native_path?"sqlite":"file",
                file.kind==PLATFORM_IMPORT_SQLITE && file.native_path?file.native_path:file.filename?file.filename:"");
            if(!imported.client_state_json) { SDL_strlcpy(error,"内存不足或文件名过长，原文献库保持不变",sizeof error); ok=false; }
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
    float base_height=SDL_max(height,600);
    a->library_content_height=base_height+150;
    a->library_outer_scroll=SDL_clamp(a->library_outer_scroll,0,a->library_content_height-height);
    for(size_t i=0;i<LIB_BUTTON_COUNT;i++) a->library_buttons[i]=(SDL_FRect){0};
    float x=left+12,w=width-24;
    a->library_buttons[LIB_CLOSE]=(SDL_FRect){left+width-68,8,56,40};
    if(height<a->library_content_height) a->library_buttons[LIB_VIEW]=(SDL_FRect){left+width-168,8,92,40};
    a->library_buttons[LIB_IMPORT]=(SDL_FRect){x,58,w/3-4,42};
    a->library_buttons[LIB_ITEMS]=(SDL_FRect){x+w/3+2,58,w/3-4,42};
    a->library_buttons[LIB_CURRENT]=(SDL_FRect){x+2*w/3+4,58,w/3-4,42};
    if(!a->library_citation_mode) {
        a->library_buttons[LIB_SEARCH]=(SDL_FRect){x,110,w,42};
        a->library_buttons[LIB_COLLECTIONS]=(SDL_FRect){x,160,w/3-4,42};
        a->library_buttons[LIB_ALL]=(SDL_FRect){x+w/3+2,160,w/3-4,42};
        a->library_buttons[LIB_DESCENDANTS]=(SDL_FRect){x+2*w/3+4,160,w/3-4,42};
    }
    a->library_buttons[LIB_SOURCE]=(SDL_FRect){x,base_height+52,w/3-4,42};
    a->library_buttons[LIB_SOURCE_REFRESH]=(SDL_FRect){x+w/3+2,base_height+52,w/3-4,42};
    a->library_buttons[LIB_SOURCE_CANCEL]=(SDL_FRect){x+2*w/3+4,base_height+52,w/3-4,42};
    float bottom=base_height-220;
    a->library_rows=(SDL_FRect){x,232,w,SDL_max(32,bottom-244)};
    a->library_buttons[LIB_PAGE]=(SDL_FRect){x,bottom+45,w/2-3,42};
    a->library_buttons[LIB_NOTE]=(SDL_FRect){x+w/2+3,bottom+45,w/2-3,42};
    a->library_buttons[LIB_INSERT]=(SDL_FRect){x,bottom+96,w/3-4,42};
    a->library_buttons[LIB_OPEN_ITEM]=(SDL_FRect){x+w/3+2,bottom+96,w/3-4,42};
    a->library_buttons[LIB_OPEN_PDF]=(SDL_FRect){x+2*w/3+4,bottom+96,w/3-4,42};
    a->library_buttons[LIB_STYLE]=(SDL_FRect){x,bottom+166,w*.46f-3,42};
    a->library_buttons[LIB_SYNC]=(SDL_FRect){x+w*.46f+3,bottom+166,w*.54f-3,42};
    if(*a->library_warning) a->library_buttons[LIB_RETRY]=(SDL_FRect){x+w-106,bottom+2,106,38};
    if(!a->library_citation_mode) {
        a->library_buttons[LIB_CHECK_ALL]=(SDL_FRect){x,base_height+2,w/3-4,42};
        a->library_buttons[LIB_CHECK_NONE]=(SDL_FRect){x+w/3+2,base_height+2,w/3-4,42};
        a->library_buttons[LIB_BIBLIOGRAPHY]=(SDL_FRect){x+2*w/3+4,base_height+2,w/3-4,42};
    }
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
    const char *names[LIB_BUTTON_COUNT]={"关闭","导入文件","文献库","当前引用","搜索","分类列表","全部条目","含子分类","页码","摘录","插入引用","原条目","PDF","APA 7","更新参考文献","操作 ↓","重试保存","勾选结果","清空勾选","设为书目","连接来源","刷新来源","取消读取"};
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
    SDL_snprintf(style,sizeof style,"%s",citation_style_label(a)); names[LIB_STYLE]=style;
    for(size_t i=0;i<LIB_BUTTON_COUNT;i++) if(a->library_buttons[i].w && i!=LIB_CLOSE && i!=LIB_VIEW) {
        SDL_FRect button=a->library_buttons[i];
        fill(a,button,surface); clipped_label(a,names[i],(SDL_FRect){button.x+8,button.y+10,button.w-16,24},text);
    }
    float offset=a->library_outer_scroll;
    float x=a->library_area.x+12,w=a->library_area.w-24,bottom=SDL_max(a->library_area.h,600)-220-offset;
    if(a->library_citation_mode) {
        clipped_label(a,a->citation_title && *a->citation_title?a->citation_title:"当前未选中引用块",(SDL_FRect){x,120-offset,w,30},text);
        label(a,"在正文中点击引用块，再打开此面板。",x,166-offset,text);
        label(a,"页码和摘录按“保存引用”后写入，可撤销。",x,198-offset,text);
    } else {
        const char *collection=a->library.collection<a->library.library.collection_count?a->library.library.collections[a->library.collection].name:"全部文献";
        char summary[512]; SDL_snprintf(summary,sizeof summary,"%s · %zu %s · 已勾选 %zu",collection?collection:"分类",a->library.row_count,a->library.collections_view?"个分类":"条结果",a->library.checked_count);
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
                label(a,a->library.checked && a->library.checked[index]?"☑":"☐",x+8,y+18,text);
                clipped_label(a,item->title && *item->title?item->title:"未命名文献",(SDL_FRect){x+40,y+7,w-48,24},text);
                char detail[512]; SDL_snprintf(detail,sizeof detail,"%s · %s",item->author?item->author:"",item->year?item->year:"");
                clipped_label(a,detail,(SDL_FRect){x+40,y+33,w-48,24},text);
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
    clipped_label(a,*a->citation_warning?a->citation_warning:"引用和参考文献随编辑自动更新。",(SDL_FRect){x,bottom+142,w,22},text);
    const char *source_hint=a->source_job?"正在读取来源，当前文献库仍可使用…":
#ifdef __EMSCRIPTEN__
        "浏览器无法直连 Better BibTeX，可导入其导出文件。";
#else
        "连接读取 CSL JSON；来源仅在点击刷新时重新读取。";
#endif
    clipped_label(a,source_hint,(SDL_FRect){x,SDL_max(a->library_area.h,600)+102-offset,w,40},text);
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
        else if(!a->library_citation_mode && !a->library.collections_view && event->key.key==SDLK_A && (event->key.mod&(SDL_KMOD_CTRL|SDL_KMOD_GUI))) library_action(a,LIB_CHECK_ALL);
        else if(!a->library_citation_mode && !a->library.collections_view && event->key.key==SDLK_SPACE && a->library.selected_item<a->library.library.item_count)
            ce_library_panel_check(&a->library,a->library.selected_item,!a->library.checked[a->library.selected_item]);
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
    } else if(event->type==SDL_EVENT_MOUSE_BUTTON_DOWN && event->button.button==SDL_BUTTON_LEFT) {
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
            a->library_pressed_check=event->button.x<a->library_rows.x+36;
            a->library_dragging=true; a->library_drag_moved=false;
            a->library_drag_y=a->library_drag_origin=event->button.y;
            a->library_drag_origin_x=event->button.x;
        }
    } else if(event->type==SDL_EVENT_MOUSE_MOTION && a->library_dragging) {
        if(SDL_fabsf(event->motion.y-a->library_drag_origin)>5 || SDL_fabsf(event->motion.x-a->library_drag_origin_x)>5) a->library_drag_moved=true;
        a->library.scroll=SDL_max(0,a->library.scroll+a->library_drag_y-event->motion.y); a->library_drag_y=event->motion.y;
    } else if(event->type==SDL_EVENT_MOUSE_BUTTON_UP && event->button.button==SDL_BUTTON_LEFT) {
        if(a->library_dragging && !a->library_drag_moved && a->library_pressed_row<a->library.row_count &&
           contains(a->library_rows,event->button.x,event->button.y)) {
            size_t index=a->library.rows[a->library_pressed_row];
            if(a->library.collections_view) {
                /* A collection-name search should not hide all its items. */
                if(!ce_library_panel_query(&a->library,"") || !ce_library_panel_collection(&a->library,index)) message(a,"无法打开分类");
            } else {
                a->library.selected_item=index;
                if(a->library_pressed_check && event->button.x<a->library_rows.x+36)
                    ce_library_panel_check(&a->library,index,!a->library.checked[index]);
            }
        }
        a->library_dragging=false;
    } else if(event->type==SDL_EVENT_WINDOW_FOCUS_LOST) a->library_dragging=false;
}

#include "app_manager.inc"

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
    a->focus_button=(SDL_FRect){a->width-340.0f,3,76,42};
    a->outline_button=(SDL_FRect){a->width-258.0f,3,76,42};
    a->tools_button=(SDL_FRect){a->width-176.0f,3,76,42};
    if(a->width<360) {
        float slot=SDL_max(1.0f,(a->width-16.0f)/4);
        a->focus_button=(SDL_FRect){8,3,slot-4,42};
        a->outline_button=(SDL_FRect){8+slot,3,slot-4,42};
        a->tools_button=(SDL_FRect){8+slot*2,3,slot-4,42};
        a->library_button=(SDL_FRect){8+slot*3,3,slot-4,42};
    }
    if(a->focus) {
        a->focus_button=(SDL_FRect){a->width-104.0f,3,92,42};
        a->outline_button=a->tools_button=a->library_button=(SDL_FRect){0};
    }
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
    if(!a->tools || a->focus) {
        /* Keep the everyday writing actions in one row; advanced formatting
         * remains available through the explicit Tools button. */
        const Action primary[]={ACT_SIDEBAR,ACT_NEW,ACT_SAVE,ACT_UNDO,ACT_REDO,ACT_BOLD,ACT_HEADING,ACT_FIND,ACT_MANAGE,ACT_SEARCH_ALL};
        for(size_t i=0;i<SDL_arraysize(a->buttons);i++) a->buttons[i].box=(SDL_FRect){0};
        for(size_t slot=0;!a->focus && slot<SDL_arraysize(primary) && slot<(size_t)columns;slot++)
            for(size_t i=0;i<SDL_arraysize(a->buttons);i++) if(a->buttons[i].action==primary[slot])
                a->buttons[i].box=(SDL_FRect){16.0f+slot*78,50,72,34};
    }
    float top = a->focus?52:a->tools?138:98;
    float sidebar_width = a->sidebar && !a->focus ? SDL_min(a->sidebar_width, a->width * 0.72f) : 0;
    bool overlay = a->width < 700;
    float left = (!overlay && a->sidebar) ? sidebar_width : 0;
    a->sidebar_area = (SDL_FRect){0,top,sidebar_width,SDL_max(40,a->height-top-58)};
    a->sidebar_scroll=SDL_clamp(a->sidebar_scroll,0,
        SDL_max(0,(float)(a->outline?outline_count(a):a->visible_count)*40-a->sidebar_area.h+44));
    a->note_import_button=sidebar_width && !a->outline?(SDL_FRect){sidebar_width-98,top+4,88,36}:(SDL_FRect){0};
    float available=SDL_max(20,a->width-left-48),column=SDL_min(760.0f,available);
    a->editor_area = (SDL_FRect){left+24+(available-column)/2,top+30,column,SDL_max(40,a->height-top-102)};
    float touch_button=SDL_min(90.0f,SDL_max(1.0f,(a->width-32.0f)/4));
    for(size_t i=0;i<SDL_arraysize(a->touch_menu_buttons);i++)
        a->touch_menu_buttons[i]=(SDL_FRect){(a->width-touch_button*4)/2+(float)i*touch_button,
            SDL_max(top,a->height-78.0f),touch_button-3,40};
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
static void touch_cancel(App *a) {
    ce_touch_cancel(&a->touch);
    a->touch_ui_active=false; a->touch_menu_visible=false; a->touch_menu_pressed=-1;
    a->library_dragging=false; a->resizing_sidebar=false;
    if(a->edit) a->edit->highlighting=false;
    if(a->prompt) a->prompt->highlighting=false;
}
static void touch_scroll(App *a,float delta) {
    float height;
    if(rich_prepare(a,a->edit)) height=ce_rich_layout_height(a->rich);
    else { int w,h; if(!TTF_GetTextSize(a->edit->text,&w,&h)) return; height=(float)h; }
    a->scroll=SDL_clamp(a->scroll+delta,0,SDL_max(0,height-a->editor_area.h+24));
    a->edit->rect.y=a->editor_area.y-a->scroll;
    a->edit->rect.h=a->editor_area.h+a->scroll;
}
static int touch_hit(App *a,float x,float y) {
    if(!rich_prepare(a,a->edit)) return a->edit->cursor;
    return rich_hit(a,x-a->edit->rect.x,y-a->edit->rect.y);
}
static void touch_selection_move(App *a,float x,float y) {
    int end=touch_hit(a,x,y);
    if(end==a->touch_select_anchor && rich_prepare(a,a->edit)) end=rich_step(a,end,1);
    a->edit->highlight1=a->touch_select_anchor; a->edit->highlight2=end;
    a->edit->cursor=end; a->edit->cursor_trailing=false;
}
static void touch_apply(App *a,CeTouchResult r) {
    if(r.actions&CE_TOUCH_CANCEL) {
        a->touch_ui_active=false; a->touch_menu_visible=false; a->touch_menu_pressed=-1;
        a->library_dragging=false; a->resizing_sidebar=false;
        a->edit->highlighting=false; a->prompt->highlighting=false;
        return;
    }
    if(r.actions&CE_TOUCH_SCROLL) { a->touch_menu_visible=false; touch_scroll(a,r.scroll_dy); }
    if(r.actions&CE_TOUCH_TAP) {
        if(!EditBox_CancelComposition(a->edit)) return;
        SDL_ClearComposition(a->window); a->rich_dirty=true;
        SDL_Event click={0}; click.type=SDL_EVENT_MOUSE_BUTTON_DOWN;
        click.button.windowID=SDL_GetWindowID(a->window); click.button.button=SDL_BUTTON_LEFT;
        click.button.x=r.x; click.button.y=r.y;
        EditBox_HandleEvent(a->edit,&click);
        click.type=SDL_EVENT_MOUSE_BUTTON_UP; EditBox_HandleEvent(a->edit,&click);
        /* Publish the C-confirmed target before the same trusted pointerup
         * returns; a later frame is too late for mobile keyboard activation. */
        layout(a); platform_web_input_request_focus(); a->touch_menu_visible=true;
    }
    if(r.actions&CE_TOUCH_SELECT_BEGIN) {
        if(!EditBox_CancelComposition(a->edit)) { touch_cancel(a); return; }
        SDL_ClearComposition(a->window); a->rich_dirty=true;
        int start=touch_hit(a,r.anchor_x,r.anchor_y);
        if(rich_prepare(a,a->edit) && (size_t)start>=ce_document_length(a->doc)) start=rich_step(a,start,-1);
        a->touch_select_anchor=start;
        touch_selection_move(a,r.x,r.y);
    }
    if(r.actions&(CE_TOUCH_SELECT_UPDATE|CE_TOUCH_SELECT_END)) touch_selection_move(a,r.x,r.y);
    if(r.actions&CE_TOUCH_SELECT_END) a->touch_menu_visible=true;
}
/* Touch owns one route until release. Real mouse/pen events retain their
 * existing path. Only converted logical coordinates enter this function. */
static bool touch_route(App *a,SDL_Event *event) {
    if(event->type!=SDL_EVENT_FINGER_DOWN && event->type!=SDL_EVENT_FINGER_MOTION &&
       event->type!=SDL_EVENT_FINGER_UP && event->type!=SDL_EVENT_FINGER_CANCELED) return false;
    SDL_TouchFingerEvent finger=event->tfinger;
    if(finger.touchID==SDL_MOUSE_TOUCHID || finger.touchID==SDL_PEN_TOUCHID ||
       finger.windowID!=SDL_GetWindowID(a->window)) return true;
    if(a->touch.contact_count && a->touch_revision!=a->citation_revision) touch_cancel(a);
    bool down=event->type==SDL_EVENT_FINGER_DOWN,up=event->type==SDL_EVENT_FINGER_UP;
    bool eligible=a->active && !a->library_open && !a->manager_open && !a->search_open && a->prompt_mode==PROMPT_NONE &&
        contains(a->editor_area,finger.x,finger.y) &&
        !(a->sidebar && !a->focus && contains(a->sidebar_area,finger.x,finger.y));
    int menu=-1;
    if(a->touch_menu_visible && !a->library_open && a->prompt_mode==PROMPT_NONE)
        for(size_t i=0;i<SDL_arraysize(a->touch_menu_buttons);i++)
            if(contains(a->touch_menu_buttons[i],finger.x,finger.y)) { menu=(int)i; eligible=false; break; }
    if(down && !a->touch.contact_count) {
        a->touch_revision=a->citation_revision;
        if(eligible) a->typing_mask=0;
    }
    CeTouchEvent input={down?CE_TOUCH_DOWN:up?CE_TOUCH_UP:
        event->type==SDL_EVENT_FINGER_CANCELED?CE_TOUCH_CANCELED:CE_TOUCH_MOVE,
        finger.touchID,finger.fingerID,finger.timestamp?finger.timestamp/1000000:SDL_GetTicks(),
        finger.x,finger.y,eligible};
    CeTouchResult result=ce_touch_event(&a->touch,&input); touch_apply(a,result);
    if(result.handled) return true;
    if(down) {
        a->touch_ui_active=true; a->touch_ui_device=finger.touchID; a->touch_ui_finger=finger.fingerID;
        a->touch_menu_pressed=menu;
        if(menu<0) a->touch_menu_visible=false;
    }
    if(!a->touch_ui_active || a->touch_ui_device!=finger.touchID || a->touch_ui_finger!=finger.fingerID) return true;
    if(a->touch_menu_pressed!=-1) {
        int pressed=a->touch_menu_pressed;
        if(!up && (SDL_fabsf(finger.x-a->touch.down_x)>=CE_TOUCH_SLOP ||
                   SDL_fabsf(finger.y-a->touch.down_y)>=CE_TOUCH_SLOP)) a->touch_menu_pressed=-2;
        if(up) {
            a->touch_ui_active=false; a->touch_menu_pressed=-1;
            if(pressed>=0 && menu==pressed) editor_clipboard_action(a,(CeEditorClipboardAction)pressed);
        }
        return true;
    }
    if(up) {
        a->touch_ui_active=false;
        if(a->prompt_mode!=PROMPT_NONE && a->prompt->has_focus) {
            layout(a); platform_web_input_request_focus();
        }
    }
    SDL_zero(*event);
    if(down || up) {
        event->type=down?SDL_EVENT_MOUSE_BUTTON_DOWN:SDL_EVENT_MOUSE_BUTTON_UP;
        event->button.windowID=finger.windowID; event->button.button=SDL_BUTTON_LEFT;
        event->button.down=down; event->button.x=finger.x; event->button.y=finger.y;
    } else {
        event->type=SDL_EVENT_MOUSE_MOTION; event->motion.windowID=finger.windowID;
        event->motion.state=SDL_BUTTON_LMASK; event->motion.x=finger.x; event->motion.y=finger.y;
    }
    return false;
}
static void touch_tick(App *a) {
    uint64_t now=SDL_GetTicks(),elapsed=a->touch_last_tick?now-a->touch_last_tick:0;
    a->touch_last_tick=now;
    if((a->touch.contact_count || a->touch_menu_visible) &&
        (a->touch_revision!=a->citation_revision ||
         ((a->touch_menu_visible || a->touch.phase==CE_TOUCH_PENDING ||
           a->touch.phase==CE_TOUCH_SCROLLING || a->touch.phase==CE_TOUCH_SELECTING) &&
          (a->prompt_mode!=PROMPT_NONE || a->library_open || a->manager_open || a->search_open)))) touch_cancel(a);
    touch_apply(a,ce_touch_tick(&a->touch,now));
    if(a->touch.phase==CE_TOUCH_SELECTING) {
        float y=a->touch.last_y,delta=0;
        if(y<a->editor_area.y+24) delta=-1;
        else if(y>a->editor_area.y+a->editor_area.h-24) delta=1;
        if(delta) { touch_scroll(a,delta*(float)SDL_min(elapsed,50)*.35f); touch_selection_move(a,a->touch.last_x,y); }
    }
}
/* Outline rows are derived from the current model, never stored byte offsets
 * that could become stale after an edit or an undo. */
static size_t outline_count(App *a) {
    size_t count=0; CeBlockInfo block;
    for(size_t i=0;i<ce_document_block_count(a->doc);i++)
        if(ce_document_block_info(a->doc,i,&block) && block.kind==CE_BLOCK_HEADING) ++count;
    return count;
}
static void outline_jump(App *a,size_t row) {
    CeBlockInfo block;
    for(size_t i=0;i<ce_document_block_count(a->doc);i++) {
        if(!ce_document_block_info(a->doc,i,&block) || block.kind!=CE_BLOCK_HEADING) continue;
        if(row--) continue;
        a->edit->cursor=(int)block.start; a->edit->highlight1=a->edit->highlight2=-1;
        a->typing_mask=0; EditBox_SetFocus(a->edit,true);
        if(a->width<700) { a->sidebar=false; layout(a); }
        reveal_cursor(a); return;
    }
}
static void writing_summary(App *a,char *out,size_t capacity) {
    size_t start=0,end=ce_document_length(a->doc),selected_start,selected_end;
    get_selection(a,&selected_start,&selected_end);
    selected_start=SDL_min(selected_start,end); selected_end=SDL_min(selected_end,end);
    bool selected=!a->edit->composition_length && !a->edit->composition_snapshot && selected_end>selected_start;
    if(selected) { start=selected_start; end=selected_end; }
    if(!a->stats_valid || a->stats_revision!=a->citation_revision || a->stats_start!=start || a->stats_end!=end) {
        a->writing_stats=ce_writing_stats(ce_document_text(a->doc)+start,end-start);
        a->stats_revision=a->citation_revision; a->stats_start=start; a->stats_end=end; a->stats_valid=true;
    }
    SDL_snprintf(out,capacity,"%s%zu 字符 · %zu 词 · %zu 段 · 约 %zu 分钟阅读%s",
        selected?"选中 · ":"",a->writing_stats.characters,a->writing_stats.words,
        a->writing_stats.paragraphs,a->writing_stats.reading_minutes,a->typewriter?" · 打字机":"");
    if(a->article_info.goal) {
        if(!a->goal_valid || a->goal_revision!=a->citation_revision) {
            a->goal_characters=ce_writing_stats(ce_document_text(a->doc),ce_document_length(a->doc)).characters;
            a->goal_revision=a->citation_revision; a->goal_valid=true;
        }
        char statistics[512]; SDL_strlcpy(statistics,out,sizeof statistics);
        SDL_snprintf(out,capacity,"目标 %zu/%zu 字符 · %s",a->goal_characters,a->article_info.goal,statistics);
    }
}
static void draw(App *a) {
    layout(a);
    SDL_Color text = ink(a), bg = paper(a);
    SDL_Color surface = a->dark ? (SDL_Color){36,42,49,255} : (SDL_Color){239,242,239,255};
    SDL_SetRenderDrawColor(a->renderer,bg.r,bg.g,bg.b,255); SDL_RenderClear(a->renderer);
    if(a->width>=700 || a->focus) {
        label(a,"NOTE LAB",18,12,text);
        clipped_label(a,a->active ? a->active : "留一页空间，专心写作",(SDL_FRect){150,12,SDL_max(0,a->width-(a->focus?268:510)),30},text);
    }
    const SDL_FRect controls[]={a->focus_button,a->outline_button,a->tools_button,a->library_button};
    const char *control_labels[]={a->focus?"退出专注":"专注",a->outline?"文章库":"大纲",a->tools?"收起":"格式",*a->library_warning?"文献 !":"文献"};
    for(size_t i=0;i<SDL_arraysize(controls);i++) if(controls[i].w) {
        fill(a,controls[i],surface); label(a,control_labels[i],controls[i].x+10,controls[i].y+10,text);
    }
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
    if (a->sidebar && !a->focus) {
        fill(a,a->sidebar_area,surface);
        clipped_label(a,a->outline?"文章大纲":*a->folder_filter?a->folder_filter:
            (*a->tag_filter || a->status_filter)?"文章库 · 筛选":"文章库 · 管理",
            (SDL_FRect){16,a->sidebar_area.y+10,a->sidebar_area.w-120,30},text);
        if(!a->outline) {
        fill(a,a->note_import_button,a->dark?(SDL_Color){48,57,65,255}:(SDL_Color){218,229,220,255});
        label(a,"导入",a->note_import_button.x+24,a->note_import_button.y+7,text);
        }
        SDL_Rect side={(int)a->sidebar_area.x,(int)a->sidebar_area.y+40,(int)a->sidebar_area.w,(int)a->sidebar_area.h-40};
        SDL_SetRenderClipRect(a->renderer,&side);
        if(a->outline) {
            size_t row=0; CeBlockInfo block;
            for(size_t i=0;i<ce_document_block_count(a->doc);i++) {
                if(!ce_document_block_info(a->doc,i,&block) || block.kind!=CE_BLOCK_HEADING) continue;
                float y=a->sidebar_area.y+44+(float)row++*40-a->sidebar_scroll;
                if(y+36<side.y || y>side.y+side.h) continue;
                char title[256]; size_t n=SDL_min(block.end-block.start,sizeof title-1);
                const char *source=ce_document_text(a->doc)+block.start;
                while(n && ((unsigned char)source[n]&0xc0)==0x80) --n;
                memcpy(title,source,n); title[n]=0;
                float indent=16+(SDL_clamp(block.level,1,6)-1)*12;
                clipped_label(a,*title?title:"无标题",(SDL_FRect){indent,y+6,a->sidebar_area.w-indent-12,30},text);
            }
            if(!row) {
                clipped_label(a,"用标题组织文章",(SDL_FRect){16,side.y+12,side.w-24,30},text);
                clipped_label(a,"输入 # 空格创建标题",(SDL_FRect){16,side.y+48,side.w-24,30},text);
            }
        } else for(size_t row=0;row<a->visible_count;++row) {
            size_t i=a->visible_files[row];
            float y=a->sidebar_area.y+44+(float)row*40-a->sidebar_scroll;
            if(y+36<side.y || y>side.y+side.h) continue;
            if(a->active && !strcmp(a->active,a->files.names[i])) fill(a,(SDL_FRect){8,y,a->sidebar_area.w-16,36},a->dark?(SDL_Color){43,74,68,255}:(SDL_Color){206,226,215,255});
            clipped_label(a,a->files.names[i],(SDL_FRect){16,y+6,a->sidebar_area.w-28,30},text);
        }
        SDL_SetRenderClipRect(a->renderer,NULL);
    }
    if(a->library_open) library_draw(a);
    if(a->touch_menu_visible && a->active && !a->library_open && !a->manager_open && !a->search_open && a->prompt_mode==PROMPT_NONE) {
        const char *names[]={"复制","剪切","粘贴","全选"};
        for(size_t i=0;i<SDL_arraysize(names);i++) {
            fill(a,a->touch_menu_buttons[i],surface);
            label(a,names[i],a->touch_menu_buttons[i].x+8,a->touch_menu_buttons[i].y+9,text);
        }
    }
    bool compact_footer=a->touch_menu_visible || a->library_open;
    float footer=compact_footer?30.0f:58.0f;
    fill(a,(SDL_FRect){0,a->height-footer,(float)a->width,footer},surface);
    char summary[256]; writing_summary(a,summary,sizeof summary);
    if(!compact_footer) clipped_label(a,a->active?summary:"从一个想法开始 · 新建文章后自动保存",(SDL_FRect){12,a->height-55.0f,a->width-24.0f,28},text);
    if(!compact_footer && a->active && a->article_info.goal) fill(a,(SDL_FRect){0,a->height-footer,(float)a->width*SDL_min(1.0f,(float)a->goal_characters/a->article_info.goal),3},(SDL_Color){72,146,116,255});
    clipped_label(a,a->status,(SDL_FRect){12,a->height-27.0f,a->width-24.0f,28},text);
    manager_draw(a); search_draw(a);
    if(a->prompt_mode!=PROMPT_NONE) {
        SDL_SetRenderDrawBlendMode(a->renderer,SDL_BLENDMODE_BLEND);
        fill(a,(SDL_FRect){0,0,(float)a->width,(float)a->height},(SDL_Color){0,0,0,110});
        SDL_SetRenderDrawBlendMode(a->renderer,SDL_BLENDMODE_NONE);
        fill(a,a->prompt_area,bg);
        const char *title=a->prompt_mode==PROMPT_NEW?"新建笔记 · 输入文件名":a->prompt_mode==PROMPT_FIND?"查找文字":a->prompt_mode==PROMPT_LINK?"链接地址（留空移除）":a->prompt_mode==PROMPT_LIBRARY_SEARCH?"搜索文献或分类（留空显示全部）":a->prompt_mode==PROMPT_CITATION_PAGE?"引用页码（留空清除）":a->prompt_mode==PROMPT_CITATION_NOTE?"引用摘录（留空清除）":a->prompt_mode==PROMPT_LIBRARY_SOURCE?"文献来源地址 · 连接并读取":"确认删除当前笔记？";
        if(a->prompt_mode==PROMPT_DELETE) title="将文章移入回收站？";
        else if(a->prompt_mode==PROMPT_REPLACE_QUERY) title=a->replace_all?"全部替换 · 查找文字":"替换下一处 · 查找文字";
        else if(a->prompt_mode==PROMPT_REPLACE_VALUE) title="替换为（留空删除匹配文字）";
        else if(a->prompt_mode==PROMPT_SEARCH_ALL) title="搜索全部文章正文";
        else if(a->prompt_mode==PROMPT_FOLDER) title="文章文件夹（留空回到未分类）";
        else if(a->prompt_mode==PROMPT_TAGS) title="文章标签（用逗号分隔）";
        else if(a->prompt_mode==PROMPT_GOAL) title="目标非空白字符数（0 关闭）";
        else if(a->prompt_mode==PROMPT_NEW_FOLDER) title="新建文件夹";
        else if(a->prompt_mode==PROMPT_TAG_FILTER) title="按标签筛选文章（留空清除）";
        else if(a->prompt_mode==PROMPT_RESTORE_NAME) title="恢复文章 · 文件名（不会覆盖同名文件）";
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
    SDL_SetHint(SDL_HINT_TOUCH_MOUSE_EVENTS,"0");
    SDL_SetHint(SDL_HINT_MOUSE_TOUCH_EVENTS,"0");
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
        {"增缩进",ACT_INDENT,{0}}, {"减缩进",ACT_OUTDENT,{0}},
        {"管理",ACT_MANAGE,{0}}, {"替换",ACT_REPLACE,{0}}, {"全替换",ACT_REPLACE_ALL,{0}},
        {"搜文章",ACT_SEARCH_ALL,{0}}, {"复制MD",ACT_COPY_MD,{0}}, {"复制HTML",ACT_COPY_HTML,{0}},
        {"导出MD",ACT_EXPORT_MD,{0}}, {"导出HTML",ACT_EXPORT_HTML,{0}},
        {"更多 ›",ACT_MORE,{0}}
    };
    SDL_COMPILE_TIME_ASSERT(button_count, SDL_arraysize(buttons) == SDL_arraysize(a->buttons));
    memcpy(a->buttons,buttons,sizeof(buttons)); a->sidebar=true; a->sidebar_width=240;
    platform_web_input_init();
    platform_web_input_clipboard(browser_clipboard_read, browser_clipboard_cut, browser_clipboard_error, SDL_AppEvent, a);
    platform_web_input_clipboard_rich(browser_clipboard_prepare, browser_clipboard_finish, browser_clipboard_paste, a);
    refresh_files(a); layout(a);
    message(a,"就绪 · Ctrl/Cmd+N 新建 · Ctrl/Cmd+S 保存");
    library_cache_load(a,true);
    if(a->files.count) open_note(a,a->files.names[0]);
    return SDL_APP_CONTINUE;
}
SDL_AppResult SDL_AppEvent(void *state, SDL_Event *event) {
    if (platform_web_input_handle_event(event, SDL_AppEvent, state)) return SDL_APP_CONTINUE;
    App *a=state;
    editing_event(a,event);
    if((event->type==SDL_EVENT_MOUSE_BUTTON_DOWN || event->type==SDL_EVENT_MOUSE_BUTTON_UP) && event->button.which==SDL_TOUCH_MOUSEID) return SDL_APP_CONTINUE;
    if(event->type==SDL_EVENT_MOUSE_MOTION && event->motion.which==SDL_TOUCH_MOUSEID) return SDL_APP_CONTINUE;
    if(event->type==SDL_EVENT_WINDOW_FOCUS_LOST || event->type==SDL_EVENT_WINDOW_RESIZED ||
       event->type==SDL_EVENT_WILL_ENTER_BACKGROUND || event->type==SDL_EVENT_QUIT) touch_cancel(a);
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
    if(touch_route(a,event)) return SDL_APP_CONTINUE;
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
    if(manager_event(a,event) || search_event(a,event)) return SDL_APP_CONTINUE;
    if(a->library_open) { library_event(a,event); return SDL_APP_CONTINUE; }
    if(event->type==SDL_EVENT_KEY_DOWN && !a->edit->composition_length) {
        if(a->focus && event->key.key==SDLK_ESCAPE) { action(a,ACT_FOCUS); return SDL_APP_CONTINUE; }
        if((event->key.mod&(SDL_KMOD_CTRL|SDL_KMOD_GUI)) && (event->key.mod&SDL_KMOD_SHIFT)) {
            if(event->key.key==SDLK_F) { action(a,ACT_FOCUS); return SDL_APP_CONTINUE; }
            if(event->key.key==SDLK_O) { action(a,ACT_OUTLINE); return SDL_APP_CONTINUE; }
            if(event->key.key==SDLK_T) { action(a,ACT_TOOLS); return SDL_APP_CONTINUE; }
            if(event->key.key==SDLK_H) { action(a,ACT_REPLACE_ALL); return SDL_APP_CONTINUE; }
            if(event->key.key==SDLK_P) { action(a,ACT_SEARCH_ALL); return SDL_APP_CONTINUE; }
            if(event->key.key==SDLK_M) { action(a,ACT_MANAGE); return SDL_APP_CONTINUE; }
        }
    }
    if (a->active && a->edit->composition_length) {
        if (event->type == SDL_EVENT_KEY_DOWN) {
            if (event->key.key == SDLK_ESCAPE) { SDL_ClearComposition(a->window); sync_view(a); }
            return SDL_APP_CONTINUE;
        }
        if (event->type == SDL_EVENT_MOUSE_BUTTON_DOWN) { SDL_ClearComposition(a->window); sync_view(a); }
    }
    if(event->type==SDL_EVENT_MOUSE_BUTTON_DOWN) {
        if(contains(a->focus_button,event->button.x,event->button.y)) { action(a,ACT_FOCUS); return SDL_APP_CONTINUE; }
        if(contains(a->outline_button,event->button.x,event->button.y)) { action(a,ACT_OUTLINE); return SDL_APP_CONTINUE; }
        if(contains(a->tools_button,event->button.x,event->button.y)) { action(a,ACT_TOOLS); return SDL_APP_CONTINUE; }
        if(contains(a->library_button,event->button.x,event->button.y)) { library_show(a); return SDL_APP_CONTINUE; }
        if (a->sidebar && !a->focus && a->width >= 700 && contains((SDL_FRect){a->sidebar_area.w-4,a->sidebar_area.y,8,a->sidebar_area.h},event->button.x,event->button.y)) {
            a->resizing_sidebar = true; SDL_CaptureMouse(true); return SDL_APP_CONTINUE;
        }
        for(size_t i=0;i<SDL_arraysize(a->buttons);++i) if(contains(a->buttons[i].box,event->button.x,event->button.y)) {
            action(a,a->buttons[i].action); return SDL_APP_CONTINUE;
        }
        if(a->sidebar && !a->focus && contains(a->sidebar_area,event->button.x,event->button.y)) {
            if(event->button.y<a->sidebar_area.y+40 && event->button.x<a->note_import_button.x && !a->outline) { manager_show(a,0); return SDL_APP_CONTINUE; }
            if(contains(a->note_import_button,event->button.x,event->button.y)) { document_import_begin(a); return SDL_APP_CONTINUE; }
            float pos=event->button.y-a->sidebar_area.y-44+a->sidebar_scroll;
            if(pos>=0) {
                if(a->outline) outline_jump(a,(size_t)(pos/40));
                else if((size_t)(pos/40)<a->visible_count) open_note(a,a->files.names[a->visible_files[(size_t)(pos/40)]]);
            }
            return SDL_APP_CONTINUE;
        }
        if(!contains(a->editor_area,event->button.x,event->button.y)) return SDL_APP_CONTINUE;
    }
    if(event->type==SDL_EVENT_MOUSE_WHEEL) {
        if(a->sidebar && !a->focus && contains(a->sidebar_area,event->wheel.mouse_x,event->wheel.mouse_y))
            a->sidebar_scroll=SDL_clamp(a->sidebar_scroll-event->wheel.y*40,0,
                SDL_max(0,(float)(a->outline?outline_count(a):a->visible_count)*40-a->sidebar_area.h+44));
        else {
            float height;
            if(rich_prepare(a,a->edit)) height=ce_rich_layout_height(a->rich);
            else { int w,h; TTF_GetTextSize(a->edit->text,&w,&h); height=(float)h; }
            a->scroll=SDL_clamp(a->scroll-event->wheel.y*48,0,SDL_max(0,height-a->editor_area.h+24));
        }
        return SDL_APP_CONTINUE;
    }
    if(writing_heading_shortcut(a,event)) return SDL_APP_CONTINUE;
    if(event->type==SDL_EVENT_KEY_DOWN && (event->key.mod&(SDL_KMOD_CTRL|SDL_KMOD_GUI)) && !a->edit->composition_length) {
        if(event->key.key==SDLK_A) {
            editor_clipboard_action(a,CE_EDITOR_SELECT_ALL); return SDL_APP_CONTINUE;
        }
        if(event->key.key==SDLK_C || event->key.key==SDLK_X || event->key.key==SDLK_V) {
#ifndef __EMSCRIPTEN__
            editor_clipboard_action(a,event->key.key==SDLK_C?CE_EDITOR_COPY:
                event->key.key==SDLK_X?CE_EDITOR_CUT:CE_EDITOR_PASTE);
#endif
            /* DOM clipboard events own the browser transaction synchronously. */
            return SDL_APP_CONTINUE;
        }
        Action cmd; bool handled=true;
        switch(event->key.key) {
            case SDLK_N:cmd=ACT_NEW;break; case SDLK_S:cmd=ACT_SAVE;break;
            case SDLK_Z:cmd=(event->key.mod&SDL_KMOD_SHIFT)?ACT_REDO:ACT_UNDO;break;
            case SDLK_Y:cmd=ACT_REDO;break;case SDLK_B:cmd=ACT_BOLD;break;
            case SDLK_I:cmd=ACT_ITALIC;break;case SDLK_U:cmd=ACT_UNDERLINE;break;
            case SDLK_H:cmd=ACT_REPLACE;break;case SDLK_F:cmd=ACT_FIND;break; case SDLK_K:cmd=ACT_LINK;break; default:handled=false;cmd=ACT_SAVE;break;
        }
        if(handled) { action(a,cmd); reveal_cursor(a); return SDL_APP_CONTINUE; }
        /* The upstream sample uses Ctrl; map Command on macOS for clipboard. */
        if(event->key.mod&SDL_KMOD_GUI) event->key.mod|=SDL_KMOD_CTRL;
    }
    if(a->active && event->type==SDL_EVENT_KEY_DOWN && event->key.key==SDLK_TAB && !a->edit->composition_length &&
       !(event->key.mod&(SDL_KMOD_CTRL|SDL_KMOD_GUI|SDL_KMOD_ALT))) {
        CeBlockInfo block; size_t start,end; get_selection(a,&start,&end); (void)end;
        if(ce_document_block_at(a->doc,start,&block) && block.depth>0) {
            action(a,event->key.mod&SDL_KMOD_SHIFT?ACT_OUTDENT:ACT_INDENT); return SDL_APP_CONTINUE;
        }
    }
    if(a->active) {
        if(writing_markdown_input(a,event)) return SDL_APP_CONTINUE;
        bool grouping=event->type==SDL_EVENT_TEXT_INPUT && !a->edit->composition_snapshot && !a->edit->composition_length;
        size_t prior_start,prior_end; get_selection(a,&prior_start,&prior_end);
        if(event->type==SDL_EVENT_TEXT_INPUT && a->edit->composition_snapshot) {
            prior_start=(size_t)SDL_max(0,a->edit->composition_start);
            prior_end=prior_start+(size_t)SDL_max(0,a->edit->composition_removed_length);
        }
        int cursor=a->edit->cursor;
        bool navigation=event->type==SDL_EVENT_KEY_DOWN && (event->key.key==SDLK_LEFT || event->key.key==SDLK_RIGHT || event->key.key==SDLK_UP || event->key.key==SDLK_DOWN || event->key.key==SDLK_HOME || event->key.key==SDLK_END);
        if(navigation || (event->type==SDL_EVENT_MOUSE_BUTTON_DOWN &&
            contains(a->editor_area,event->button.x,event->button.y))) a->typing_mask=0;
        EditBox_HandleEvent(a->edit,event);
        if(navigation) {
            if(event->key.mod&SDL_KMOD_SHIFT) {
                if(a->edit->highlight1<0) a->edit->highlight1=cursor;
                a->edit->highlight2=a->edit->cursor;
            } else a->edit->highlight1=a->edit->highlight2=-1;
        }
        if(event->type==SDL_EVENT_TEXT_INPUT || event->type==SDL_EVENT_KEY_DOWN) {
            bool deleting=event->type==SDL_EVENT_KEY_DOWN && (event->key.key==SDLK_BACKSPACE || event->key.key==SDLK_DELETE);
            reconcile_input(a,prior_start,prior_end,deleting,grouping); reveal_cursor(a);
        }
        else if(event->type==SDL_EVENT_TEXT_EDITING) reveal_cursor(a);
    }
    editing_after_event(a,event);
    return SDL_APP_CONTINUE;
}
SDL_AppResult SDL_AppIterate(void *state) {
    App *a=state;
    editing_tick(a);
    touch_tick(a);
    library_import_poll(a);
    library_source_poll(a);
    document_import_finish(a);
    if (a->export_job) {
        const char *result = NULL;
        if (platform_export_status(a->export_job, &result) != PLATFORM_EXPORT_PENDING) {
            message(a, result); platform_export_free(a->export_job); a->export_job = NULL;
        }
    }
    if(a->active && !a->saving && (ce_document_is_modified(a->doc) || a->citation_normalized_pending) && !a->edit->composition_length && SDL_GetTicks()-a->edited_at>=1000) {
        if(!save_document(a)) a->edited_at=SDL_GetTicks();
    }
    draw(a); ++a->frames;
    if(a->smoke && a->frames>=3) { SDL_Log("NoteLab smoke test passed"); return SDL_APP_SUCCESS; }
    return SDL_APP_CONTINUE;
}
void SDL_AppQuit(void *state, SDL_AppResult result) {
    (void)result; App *a=state; if(!a) return;
    platform_web_input_destroy();
    clipboard_request_free(a->clipboard_event); a->clipboard_event=NULL;
    EditBox_Destroy(a->prompt); EditBox_Destroy(a->edit);
    ce_rich_layout_destroy(a->rich); free(a->rich_text);
    ce_citation_session_free(a->citation_session);
    manager_preview_clear(a); ce_writing_archive_list_free(&a->manager_archives); ce_writing_store_close(&a->writing_store); free(a->visible_files);
    search_results_clear(a); free(a->replace_query); free(a->replace_text); free(a->all_search_query);
    ce_document_free(a->doc); note_store_list_free(&a->files); note_store_destroy(&a->store);
    note_store_destroy(&a->library_store); free(a->notes_root);
    free(a->active); free(a->search_keyword);
    free(a->typing_color); free(a->typing_highlight); free(a->typing_link);
    free(a->saving_name); free(a->saving_snapshot);
    platform_export_free(a->export_job);
    platform_import_free(a->import_job);
    platform_library_source_free(a->source_job); free(a->source_pending_config);
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
