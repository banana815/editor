#define SDL_MAIN_HANDLED 1
#include <SDL3/SDL.h>
static bool simulate_safe_area;
static bool app_test_safe_area(SDL_Window *window,SDL_Rect *rect) {
    if(simulate_safe_area) { *rect=(SDL_Rect){12,59,369,759}; return true; }
    return SDL_GetWindowSafeArea(window,rect);
}
#define SDL_GetWindowSafeArea app_test_safe_area
#include "../src/app.c"
#undef SDL_GetWindowSafeArea
#undef main
#include <assert.h>
struct PlatformImport { bool taken; PlatformImportKind requested; };
static const char *fixture_json="[{\"id\":\"zotero://select/groups/7/items/ABCD1234\",\"title\":\"中文论文🙂\",\"author\":[{\"family\":\"张\",\"given\":\"三\"}],\"issued\":{\"date-parts\":[[2024]]}},{\"id\":\"other\",\"title\":\"Other paper\"}]";
static const char *fixture_filename="中文🙂.json";
static size_t fixture_size=SIZE_MAX;
static PlatformImportStatus fixture_status=PLATFORM_IMPORT_DONE;
PlatformImport *platform_import_open(SDL_Window *window,PlatformImportKind kind) {
    (void)window; PlatformImport *job=SDL_calloc(1,sizeof *job); if(job)job->requested=kind; return job;
}
PlatformImportStatus platform_import_status(PlatformImport *job,const char **result) { (void)job; if(result)*result="test import"; return fixture_status; }
bool platform_import_take_result(PlatformImport *job,PlatformImportFile *file) {
    if(job->taken)return false; job->taken=true;
    file->kind=job->requested==PLATFORM_IMPORT_DOCUMENT?PLATFORM_IMPORT_DOCUMENT:PLATFORM_IMPORT_JSON;
    file->filename=SDL_strdup(fixture_filename); file->size=fixture_size==SIZE_MAX?strlen(fixture_json):fixture_size;
    file->data=SDL_malloc(file->size+1); if(file->data) { memcpy(file->data,fixture_json,file->size); file->data[file->size]=0; }
    return file->filename && file->data;
}
void platform_import_file_free(PlatformImportFile *file) { SDL_free(file->filename);SDL_free(file->data);SDL_free(file->native_path);memset(file,0,sizeof *file); }
void platform_import_free(PlatformImport *job) { SDL_free(job); }
static void press(App *app,LibraryAction which) {
    layout(app); SDL_FRect button=app->library_buttons[which]; assert(button.w);
    SDL_Event event={0}; event.type=SDL_EVENT_MOUSE_BUTTON_DOWN; event.button.button=SDL_BUTTON_LEFT;
    event.button.windowID=SDL_GetWindowID(app->window);
    assert(SDL_RenderCoordinatesToWindow(app->renderer,button.x+button.w/2,button.y+button.h/2,&event.button.x,&event.button.y));
    assert(SDL_AppEvent(app,&event)==SDL_APP_CONTINUE);
}
static void key(App *app,SDL_Keycode code,SDL_Keymod mod) {
    SDL_Event event={0}; event.type=SDL_EVENT_KEY_DOWN; event.key.key=code; event.key.mod=mod;
    assert(SDL_AppEvent(app,&event)==SDL_APP_CONTINUE);
}
static void input(App *app,const char *text) {
    SDL_Event event={0}; event.type=SDL_EVENT_TEXT_INPUT; event.text.text=text;
    assert(SDL_AppEvent(app,&event)==SDL_APP_CONTINUE);
}
static char *doc_json(App *app) { char *json=ce_document_export_json(app->doc);assert(json);return json; }
static void unchanged(App *app,const char *before) { char *now=doc_json(app);assert(!strcmp(now,before));free(now); }
static void import_note(App *a,const char *filename,const char *data) {
    fixture_filename=filename; fixture_json=data; fixture_size=SIZE_MAX; fixture_status=PLATFORM_IMPORT_DONE;
    document_import_begin(a); assert(a->import_job); library_import_poll(a); document_import_finish(a);
}
static void test_legacy_import(App *a) {
    const char *names[]={"Legacy.json","Legacy (2).json","Legacy (3).json","Literal.json","Binary.json"};
    for(size_t i=0;i<SDL_arraysize(names);i++) note_store_delete(&a->store,names[i]);
    const char *legacy="{\"type\":\"doc\",\"content\":[{\"type\":\"paragraph\",\"content\":[{\"type\":\"text\",\"text\":\"旧中文🙂\"}]},{\"type\":\"futureBlock\",\"attrs\":{\"flag\":true,\"number\":42,\"nested\":{\"key\":\"保留\"}}}]}";
    import_note(a,"Legacy.json",legacy); assert(!a->pending_note_json && !strcmp(a->active,"Legacy.json"));
    char *first=NULL; size_t first_size=0; assert(note_store_read(&a->store,"Legacy.json",&first,&first_size)==NOTE_OK);
    assert(strstr(first,"\"flag\":true") && strstr(first,"\"number\":42") && strstr(first,"保留"));
    import_note(a,"Legacy.json",legacy); assert(!strcmp(a->active,"Legacy (2).json"));
    char *original=NULL; size_t original_size=0; assert(note_store_read(&a->store,"Legacy.json",&original,&original_size)==NOTE_OK);
    assert(original_size==first_size && !memcmp(original,first,first_size)); note_store_free(first); note_store_free(original);
    import_note(a,"Legacy.html","<p><strong>旧中文</strong><br><em>尾行</em></p><div data-type=\"zoteroCitationBlock\" citationId=\"legacy\" page=\"7\" title=\"文献\"></div>");
    assert(!strcmp(a->active,"Legacy (3).json")); assert(ce_document_style_at(a->doc,0).marks&CE_MARK_BOLD);
    char *json=doc_json(a); assert(strstr(json,"\"page\":\"7\"") && strstr(json,"zoteroCitationBlock")); free(json);
    import_note(a,"Literal.txt","<literal>&\n中文🙂"); assert(!strcmp(a->active,"Literal.json"));
    assert(!strcmp(ce_document_text(a->doc),"<literal>&\n中文🙂")); json=doc_json(a);
    fixture_json="a\0b"; fixture_size=3; fixture_filename="Binary.txt";
    document_import_begin(a); library_import_poll(a); document_import_finish(a); unchanged(a,json);
    import_note(a,"bad.json","not JSON"); unchanged(a,json); assert(!a->pending_note_json);
    fixture_status=PLATFORM_IMPORT_CANCELED; document_import_begin(a); library_import_poll(a); unchanged(a,json);
    fixture_status=PLATFORM_IMPORT_DONE;
    /* A failed current-note save must abort switching to an imported copy. */
    assert(ce_document_insert(a->doc,ce_document_length(a->doc)," unsaved")); free(json); json=doc_json(a);
    note_store_destroy(&a->store);
    import_note(a,"NotCreated.json",legacy); unchanged(a,json); assert(!strcmp(a->active,"Literal.json") && !a->pending_note_json);
    assert(note_store_init(&a->store,a->notes_root)==NOTE_OK); assert(save_document(a)); free(json);
}
int main(int argc,char **argv) {
    assert(argc==3);
    SDL_SetMainReady();
    char *args[]={"test_app_library","--notes",argv[1],"--font",argv[2]};
    void *state=NULL; assert(SDL_AppInit(&state,5,args)==SDL_APP_CONTINUE); App *a=state;
    const char *initial="{\"type\":\"doc\",\"content\":[{\"type\":\"paragraph\",\"content\":[{\"type\":\"text\",\"text\":\"前后\"}]}]}";
    note_store_delete(&a->store,"Ui.json"); assert(note_store_create(&a->store,"Ui.json",initial,strlen(initial))==NOTE_OK);
    char error[256]; assert(ce_document_import(a->doc,initial,CE_FORMAT_JSON,error,sizeof error));
    free(a->active); a->active=copy_string("Ui.json"); sync_view(a); a->edit->cursor=3; EditBox_SetFocus(a->edit,true);
    char *before=doc_json(a); library_show(a); assert(!a->edit->has_focus && !a->library_citation_mode);
    press(a,LIB_IMPORT); library_import_poll(a); assert(a->library.library.item_count==2);
    unchanged(a,before); a->library.selected_item=0;
    press(a,LIB_PAGE); input(a,"12–14"); key(a,SDLK_RETURN,0); assert(a->library_open && a->prompt_mode==PROMPT_NONE);
    press(a,LIB_NOTE); input(a,"中文摘录"); key(a,SDLK_RETURN,0); assert(a->prompt_mode==PROMPT_CITATION_NOTE);
    input(a,"第二行 <&>"); key(a,SDLK_RETURN,SDL_KMOD_CTRL); assert(a->prompt_mode==PROMPT_NONE);
    assert(!strcmp(a->insert_note,"中文摘录\n第二行 <&>")); unchanged(a,before);
    press(a,LIB_NOTE);
    SDL_Event preedit={0}; preedit.type=SDL_EVENT_TEXT_EDITING; preedit.edit.text="尚未提交"; preedit.edit.start=0; preedit.edit.length=4;
    assert(SDL_AppEvent(a,&preedit)==SDL_APP_CONTINUE); assert(a->prompt->composition_length);
    key(a,SDLK_RETURN,SDL_KMOD_CTRL); assert(a->prompt_mode==PROMPT_CITATION_NOTE); unchanged(a,before);
    key(a,SDLK_ESCAPE,0); assert(!strcmp(a->insert_note,"中文摘录\n第二行 <&>")); unchanged(a,before);
    press(a,LIB_INSERT); assert(!a->library_open && a->edit->has_focus);
    assert(!strcmp(ce_document_text(a->doc),"前\n\xef\xbf\xbc\n后"));
    char *with_citation=doc_json(a); assert(strstr(with_citation,"12–14") && strstr(with_citation,"cslJson"));
    assert(ce_document_undo(a->doc)); unchanged(a,before); assert(ce_document_redo(a->doc)); sync_view(a);a->edit->cursor=4;
    library_show(a); assert(a->library_citation_mode && a->citation_id && *a->citation_id);
    press(a,LIB_PAGE); EditBox_SetText(a->prompt,""); a->prompt->cursor=0;key(a,SDLK_RETURN,0);
    press(a,LIB_INSERT); char *updated=doc_json(a); assert(strstr(updated,"\"page\":\"\""));
    assert(ce_document_undo(a->doc)); unchanged(a,with_citation); assert(ce_document_redo(a->doc)); sync_view(a);a->edit->cursor=4;
    library_show(a); press(a,LIB_SYNC); char *synced=doc_json(a); assert(strstr(synced,"approximate") && strstr(synced,"zoteroEndnoteSection"));
    assert(ce_document_undo(a->doc)); unchanged(a,updated); assert(ce_document_redo(a->doc)); sync_view(a);
    press(a,LIB_ITEMS); a->library.selected_item=1;
    fixture_json="not JSON"; press(a,LIB_IMPORT);library_import_poll(a);assert(a->library.library.item_count==2 && a->library.selected_item==1); unchanged(a,synced);
    press(a,LIB_SEARCH);input(a,"中文");key(a,SDLK_RETURN,0);assert(a->library.row_count==1 && a->library.selected_item==SIZE_MAX);
    SDL_SetWindowSize(a->window,390,400);layout(a);assert(a->width==390);press(a,LIB_VIEW);layout(a);
    assert(a->library_buttons[LIB_INSERT].y>=56 && a->library_buttons[LIB_SYNC].y+a->library_buttons[LIB_SYNC].h<=a->library_area.h);
    press(a,LIB_CLOSE); assert(a->edit->has_focus);
    assert(save_document(a));
    /* A repaired duplicate ID after an old in-flow footer must still edit the
     * originally selected citation, not the earlier block sharing its ID. */
    const char *duplicates="{\"type\":\"doc\",\"content\":[{\"type\":\"zoteroCitationBlock\",\"attrs\":{\"citationId\":\"dup\",\"itemUri\":\"uri:first\",\"title\":\"First\"}},{\"type\":\"zoteroEndnoteSection\",\"attrs\":{\"entriesJson\":\"[]\"}},{\"type\":\"zoteroCitationBlock\",\"attrs\":{\"citationId\":\"dup\",\"itemUri\":\"uri:second\",\"title\":\"Second\"}}]}";
    assert(ce_document_import(a->doc,duplicates,CE_FORMAT_JSON,error,sizeof error)); sync_view(a); a->edit->cursor=8;
    library_show(a); library_action(a,LIB_SYNC);
    assert(a->edit->cursor==4 && a->citation_id && strcmp(a->citation_id,"dup"));
    assert(a->citation_title && !strcmp(a->citation_title,"Second"));
    library_close(a);
    /* Simulate a 3x mobile framebuffer while retaining 393x852 window points.
     * A target has its own presentation state, exercising actual scaled draw. */
    assert(SDL_SetWindowSize(a->window,393,852)); layout(a); library_show(a);
    SDL_Texture *high_density=SDL_CreateTexture(a->renderer,SDL_PIXELFORMAT_RGBA8888,SDL_TEXTUREACCESS_TARGET,1179,2556);
    assert(high_density && SDL_SetRenderTarget(a->renderer,high_density)); draw(a);
    int logical_w=0,logical_h=0; SDL_RendererLogicalPresentation presentation;
    assert(SDL_GetRenderLogicalPresentation(a->renderer,&logical_w,&logical_h,&presentation));
    assert(logical_w==393 && logical_h==852 && presentation==SDL_LOGICAL_PRESENTATION_STRETCH);
    assert(a->width==393 && a->height==852 && a->library_area.w==393);
    SDL_Surface *pixels=SDL_RenderReadPixels(a->renderer,NULL); assert(pixels && pixels->w==1179 && pixels->h==2556);
    char *image_path=NULL; assert(SDL_asprintf(&image_path,"%s/high-density.bmp",argv[1])>0);
    assert(SDL_SaveBMP(pixels,image_path)); SDL_free(image_path); SDL_DestroySurface(pixels);
    assert(SDL_SetRenderTarget(a->renderer,NULL)); SDL_DestroyTexture(high_density); layout(a);
    press(a,LIB_CLOSE); assert(!a->library_open && a->edit->has_focus);
    simulate_safe_area=true; layout(a); assert(a->width==369 && a->height==759);
    SDL_Rect viewport; assert(SDL_GetRenderViewport(a->renderer,&viewport));
    assert(viewport.x==12 && viewport.y==59 && viewport.w==369 && viewport.h==759);
    library_show(a); draw(a); press(a,LIB_CLOSE); assert(!a->library_open);
    draw(a);
    SDL_Rect ime; int ime_cursor=0; assert(SDL_GetTextInputArea(a->window,&ime,&ime_cursor));
    assert(ime.x>=12 && ime.y>=59 && ime.x+ime.w<=381 && ime.y+ime.h<=818);
    /* Read the whole framebuffer, including the untouched device insets. */
    assert(SDL_SetRenderViewport(a->renderer,NULL)); pixels=SDL_RenderReadPixels(a->renderer,NULL); assert(pixels);
    assert(SDL_asprintf(&image_path,"%s/safe-area.bmp",argv[1])>0); assert(SDL_SaveBMP(pixels,image_path));
    SDL_free(image_path); SDL_DestroySurface(pixels); simulate_safe_area=false;
    layout(a); test_legacy_import(a);
    /* Fail a cache write without altering the previous on-disk cache. The
     * injected root points through an existing file, not an actual user path. */
    char *cached=NULL; size_t cached_size=0;
    assert(note_store_read(&a->library_store,"library.json",&cached,&cached_size)==NOTE_OK);
    note_store_destroy(&a->library_store); a->library_store_ready=false;
    char *original_root=a->notes_root,*bad_root=NULL; assert(SDL_asprintf(&bad_root,"%s/Ui.json",original_root)>0);
    a->notes_root=copy_string(bad_root); SDL_free(bad_root);
    fixture_filename="cache.json"; fixture_json="[{\"id\":\"persisted\",\"title\":\"缓存恢复中文\"}]";
    library_show(a); library_action(a,LIB_IMPORT); library_import_poll(a);
    assert(a->library.library.item_count==1 && a->library_unsaved && *a->library_warning && !a->library_cache_saving);
    assert(strstr(a->status,"尚未持久保存"));
    free(a->notes_root); a->notes_root=original_root;
    assert(library_cache_store_open(a)); char *still_cached=NULL; size_t still_size=0;
    assert(note_store_read(&a->library_store,"library.json",&still_cached,&still_size)==NOTE_OK);
    assert(cached_size==still_size && !memcmp(cached,still_cached,cached_size)); note_store_free(cached); note_store_free(still_cached);
    library_action(a,LIB_RETRY); assert(!a->library_unsaved && !*a->library_warning); library_close(a);
    free(before);free(with_citation);free(updated);free(synced);SDL_AppQuit(a,SDL_APP_SUCCESS);
    state=NULL; assert(SDL_AppInit(&state,5,args)==SDL_APP_CONTINUE); a=state;
    assert(a->library.library.item_count==1 && !strcmp(a->library.library.items[0].title,"缓存恢复中文"));
    assert(!*a->library_warning && !a->library_unsaved);
    assert(note_store_save(&a->library_store,"library.json","broken cache",12)==NOTE_OK); SDL_AppQuit(a,SDL_APP_SUCCESS);
    state=NULL; assert(SDL_AppInit(&state,5,args)==SDL_APP_CONTINUE); a=state;
    assert(!a->library.library.item_count && a->library_restore_failed && *a->library_warning && !a->library_unsaved);
    open_note(a,"Ui.json"); assert(!strcmp(a->status,a->library_warning));
    cached=NULL; assert(note_store_read(&a->library_store,"library.json",&cached,&cached_size)==NOTE_OK);
    assert(cached_size==12 && !memcmp(cached,"broken cache",12)); note_store_free(cached);
    assert(note_store_delete(&a->library_store,"library.json")==NOTE_OK); SDL_AppQuit(a,SDL_APP_SUCCESS);
    puts("actual app citations, cache recovery/failure, safe-area HiDPI and non-overwriting legacy import passed");return 0;
}
