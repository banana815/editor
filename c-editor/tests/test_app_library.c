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
static const char *fixture_json="[{\"id\":\"zotero://select/groups/7/items/ABCD1234\",\"type\":\"book\",\"title\":\"中文论文🙂\",\"author\":[{\"family\":\"张\",\"given\":\"三\"}],\"issued\":{\"date-parts\":[[2024]]}},{\"id\":\"other\",\"type\":\"book\",\"title\":\"Other paper\"}]";
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
struct PlatformLibrarySource { bool taken; PlatformLibrarySourceStatus state; };
static unsigned source_started;
static const char *source_fixture="[{\"id\":\"new-source\",\"type\":\"book\",\"title\":\"Fetched library\"}]";
static size_t source_fixture_size=SIZE_MAX;
PlatformLibrarySource *platform_library_source_fetch(const char *url,const PlatformLibrarySourceOptions *options,char *error,size_t capacity) {
    (void)options;
    if(!url || !*url) { SDL_strlcpy(error,"Invalid test URL",capacity); return NULL; }
    PlatformLibrarySource *job=calloc(1,sizeof *job); if(job) ++source_started; return job;
}
PlatformLibrarySource *platform_library_source_reopen_sqlite(const char *path,char *error,size_t capacity) {
    return platform_library_source_fetch(path,NULL,error,capacity);
}
PlatformLibrarySourceStatus platform_library_source_status(PlatformLibrarySource *job,const char **status) {
    if(status) *status="test source"; return job->state;
}
bool platform_library_source_take_result(PlatformLibrarySource *job,PlatformLibrarySourceResult *result) {
    if(job->taken || job->state!=PLATFORM_LIBRARY_SOURCE_DONE) return false;
    job->taken=true; result->size=source_fixture_size==SIZE_MAX?strlen(source_fixture):source_fixture_size;
    result->data=malloc(result->size+1); assert(result->data);
    memcpy(result->data,source_fixture,result->size); result->data[result->size]=0; result->http_status=200; return true;
}
void platform_library_source_result_free(PlatformLibrarySourceResult *result) { free(result->data); free(result->native_path); memset(result,0,sizeof *result); }
void platform_library_source_cancel(PlatformLibrarySource *job) { if(job) job->state=PLATFORM_LIBRARY_SOURCE_CANCELED; }
void platform_library_source_free(PlatformLibrarySource *job) { free(job); }
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
static char *citation_text(App *a) {
    const CeCitationPrepared *prepared=ce_citation_session_prepared(a->citation_session);assert(prepared);
    for(size_t i=0;i<ce_document_block_count(a->doc);i++) {
        CeBlockInfo block;assert(ce_document_block_info(a->doc,i,&block));if(block.kind!=CE_BLOCK_CITATION)continue;
        char *serialized=ce_document_block_json(a->doc,i);assert(serialized);cJSON *node=cJSON_Parse(serialized);free(serialized);assert(node);
        const cJSON *attrs=cJSON_GetObjectItemCaseSensitive(node,"attrs"),*id=cJSON_GetObjectItemCaseSensitive(attrs,"citationId");assert(cJSON_IsString(id));
        const CeCitationCache *cache=ce_citation_prepared_citation(prepared,id->valuestring);assert(cache);cJSON_Delete(node);
        if(a->citation_style==CE_CITATION_PROFILE_GBT) {
            const CeCslOutput *output=ce_citation_cache_citation_output(cache);
            for(size_t j=0;j<ce_csl_output_run_count(output);j++) assert(ce_csl_output_run(output,j)->format&CE_CSL_SUPERSCRIPT);
        }
        CeCslOutputString text={0};char error[256];assert(ce_csl_output_plain(ce_citation_cache_citation_output(cache),&text,error,sizeof error));
        char *result=copy_string(text.data);assert(result);ce_csl_output_string_free(&text);return result;
    }
    assert(false);return NULL;
}
static void citation_is(App *a,const char *expected) {
    char *actual=citation_text(a);
    if(strcmp(actual,expected)) fprintf(stderr,"citation expected <%s> actual <%s>\n",expected,actual);
    assert(!strcmp(actual,expected));free(actual);
}
static void bibliography_is(App *a,const char *expected) {
    const CeCitationPrepared *prepared=ce_citation_session_prepared(a->citation_session);assert(prepared);
    const CeCitationCache *cache=ce_citation_prepared_bibliography(prepared);assert(cache && ce_citation_cache_entry_count(cache)==1);
    const CeCitationCacheEntry *entry=ce_citation_cache_entry(cache,0);assert(entry && entry->visible && entry->row_count==1);
    char error[256];CeCslOutputString text={0};assert(ce_csl_output_plain(entry->rows[0].body,&text,error,sizeof error));
    if(strcmp(text.data,expected)) fprintf(stderr,"bibliography expected <%s> actual <%s>\n",expected,text.data);
    assert(!strcmp(text.data,expected));ce_csl_output_string_free(&text);
}
static void test_selected_bibliography_ui(App *a) {
    char *before=doc_json(a); size_t cursor=(size_t)a->edit->cursor;
    layout(a); SDL_Event pointer={0}; pointer.type=SDL_EVENT_MOUSE_BUTTON_DOWN;
    pointer.button.button=SDL_BUTTON_RIGHT; pointer.button.x=a->library_rows.x+16; pointer.button.y=a->library_rows.y+20;
    library_event(a,&pointer); pointer.type=SDL_EVENT_MOUSE_BUTTON_UP; library_event(a,&pointer);
    assert(!a->library.checked_count && !a->library_dragging);
    pointer.type=SDL_EVENT_MOUSE_BUTTON_DOWN; pointer.button.button=SDL_BUTTON_LEFT; library_event(a,&pointer);
    SDL_Event motion={0}; motion.type=SDL_EVENT_MOUSE_MOTION; motion.motion.x=pointer.button.x+30; motion.motion.y=pointer.button.y;
    library_event(a,&motion); pointer.type=SDL_EVENT_MOUSE_BUTTON_UP; library_event(a,&pointer);
    assert(!a->library.checked_count && !a->library_dragging);
    pointer.type=SDL_EVENT_MOUSE_BUTTON_DOWN; library_event(a,&pointer);
    pointer.type=SDL_EVENT_MOUSE_BUTTON_UP; library_event(a,&pointer); assert(a->library.checked_count==1);
    key(a,SDLK_SPACE,0); assert(!a->library.checked_count);
    a->library.selected_item=0; key(a,SDLK_SPACE,0); assert(a->library.checked_count==1);
    assert(ce_library_panel_query(&a->library,"Other"));
    key(a,SDLK_A,SDL_KMOD_CTRL); assert(a->library.checked_count==2);
    assert(ce_library_panel_query(&a->library,""));
    press(a,LIB_VIEW); press(a,LIB_BIBLIOGRAPHY); assert(!a->library_open);
    const CeCitationProcessStats *stats=ce_citation_prepared_stats(ce_citation_session_prepared(a->citation_session));
    assert(stats && stats->citation_count==0 && stats->reference_count==2 && stats->selected_count==2);
    char *selected=doc_json(a); assert(strstr(selected,"selectedItemsJson") && !strstr(selected,"zoteroCitationBlock"));
    library_show(a); library_action(a,LIB_STYLE);
    stats=ce_citation_prepared_stats(ce_citation_session_prepared(a->citation_session));
    assert(stats->reference_count==2 && a->citation_style==CE_CITATION_PROFILE_IEEE);
    action(a,ACT_UNDO); unchanged(a,selected); free(selected);
    library_action(a,LIB_CHECK_NONE); assert(!a->library.checked_count);
    library_action(a,LIB_BIBLIOGRAPHY); assert(!a->library_open);
    stats=ce_citation_prepared_stats(ce_citation_session_prepared(a->citation_session));
    assert(stats->reference_count==0 && stats->citation_count==0);
    action(a,ACT_UNDO); action(a,ACT_UNDO); unchanged(a,before); free(before);
    /* Opening the library reads the current prose selection into its quote
     * field; it does not insert a citation or mutate the document. */
    a->edit->highlight1=0; a->edit->highlight2=3; a->edit->cursor=3;
    library_show(a); assert(a->insert_note && !strcmp(a->insert_note,"前")); library_close(a);
    free(a->insert_note); a->insert_note=NULL;
    a->edit->highlight1=a->edit->highlight2=-1; a->edit->cursor=(int)cursor; library_show(a);
}
static void test_library_source_controller(App *a) {
    char error[256]; char *saved=ce_zotero_cache_export(&a->library.library,error,sizeof error); assert(saved);
    char *doc=doc_json(a); CeZoteroItem *retained=a->library.library.items;
    unsigned calls=source_started;
    library_source_refresh(a); /* File source deliberately reopens the picker. */
    assert(a->import_job && source_started==calls); platform_import_free(a->import_job); a->import_job=NULL;
    assert(library_source_begin(a,"http","https://example.invalid/library")); assert(a->source_job);
    library_action(a,LIB_IMPORT); assert(!a->import_job);
    document_import_begin(a); assert(!a->import_job);
    library_action(a,LIB_SOURCE_CANCEL); assert(!a->source_job && !a->source_pending_config);
    assert(a->library.library.items==retained);
    const char *good=source_fixture;
    source_fixture="not JSON";
    assert(library_source_begin(a,"http","https://example.invalid/bad")); a->source_job->state=PLATFORM_LIBRARY_SOURCE_DONE;
    library_source_poll(a); assert(!a->source_job && a->library.library.items==retained);
    source_fixture="a\0b"; source_fixture_size=3;
    assert(library_source_begin(a,"http","https://example.invalid/binary")); a->source_job->state=PLATFORM_LIBRARY_SOURCE_DONE;
    library_source_poll(a); assert(!a->source_job && a->library.library.items==retained);
    char *unchanged_cache=ce_zotero_cache_export(&a->library.library,error,sizeof error);
    assert(unchanged_cache && !strcmp(unchanged_cache,saved)); free(unchanged_cache);
    source_fixture=good; source_fixture_size=SIZE_MAX;
    assert(library_source_begin(a,"http","https://example.invalid/finished-but-canceled"));
    a->source_job->state=PLATFORM_LIBRARY_SOURCE_DONE; library_action(a,LIB_SOURCE_CANCEL);
    assert(!a->source_job && a->library.library.items==retained);
    assert(library_source_begin(a,"http","https://example.invalid/library")); a->source_job->state=PLATFORM_LIBRARY_SOURCE_DONE;
    a->library_cache_saving=true; library_source_poll(a); assert(a->source_job && a->library.library.items==retained);
    a->library_cache_saving=false;
    a->library_citation_mode=true; free(a->citation_note); a->citation_note=copy_string("existing note");
    char *draft=a->insert_note; library_prompt(a,PROMPT_CITATION_NOTE);
    EditBox_SetText(a->prompt,"edited current note"); library_source_poll(a);
    assert(a->source_job && a->library.library.items==retained && a->library_citation_mode);
    key(a,SDLK_RETURN,SDL_KMOD_CTRL);
    assert(a->prompt_mode==PROMPT_NONE && !strcmp(a->citation_note,"edited current note") && a->insert_note==draft);
    library_source_poll(a);
    assert(!a->source_job && a->library.library.item_count==1 && !strcmp(a->library.library.items[0].title,"Fetched library"));
    cJSON *config=library_source_read(a); assert(config && !strcmp(json_string(config,"location"),"https://example.invalid/library")); cJSON_Delete(config);
    calls=source_started; assert(library_cache_load(a,false)); assert(source_started==calls && !a->source_job);
    library_source_refresh(a); assert(a->source_job && source_started==calls+1);
    library_action(a,LIB_SOURCE_CANCEL); unchanged(a,doc); free(doc);
    CeZoteroLibrary restored={0}; assert(ce_zotero_cache_import(saved,&restored,error,sizeof error)); free(saved);
    assert(ce_library_panel_replace(&a->library,&restored)); a->library_unsaved=true; assert(library_cache_save(a));
}
static void test_missing_snapshot(App *a) {
    char *restore=doc_json(a);char error[256];
    const char *legacy="{\"type\":\"doc\",\"content\":[{\"type\":\"paragraph\",\"content\":[{\"type\":\"text\",\"text\":\"Original prose\"}]},{\"type\":\"zoteroCitationBlock\",\"attrs\":{\"citationId\":\"unresolved\",\"itemUri\":\"https://example.invalid/not-in-library\",\"title\":\"Incomplete legacy metadata\"}}]}";
    assert(ce_document_import(a->doc,legacy,CE_FORMAT_JSON,error,sizeof error));
    ce_citation_session_free(a->citation_session);a->citation_session=NULL;++a->citation_revision;
    sync_view(a);CeBlockInfo block;assert(ce_document_block_info(a->doc,1,&block));a->edit->cursor=(int)block.start;
    library_show(a);assert(a->library_citation_mode);char *before=doc_json(a);CeCitationProfile profile=a->citation_style;
    library_action(a,LIB_SYNC);unchanged(a,before);assert(strstr(a->status,"CSL snapshot") && !a->citation_session);
    library_action(a,LIB_STYLE);unchanged(a,before);assert(a->citation_style==profile && !a->citation_session);
    free(a->citation_page);a->citation_page=copy_string("99");assert(a->citation_page);
    library_action(a,LIB_INSERT);unchanged(a,before);assert(a->library_open && !a->citation_session);
    library_close(a);free(before);
    assert(ce_document_import(a->doc,restore,CE_FORMAT_JSON,error,sizeof error));free(restore);
    citation_refresh(a);assert(a->citation_session);sync_view(a);
}
static void test_legacy_bibliography_preserved(App *a) {
    char *return_to=copy_string(a->active);assert(return_to);
    const char *fixture="{\"type\":\"doc\",\"content\":[{\"type\":\"paragraph\",\"content\":[{\"type\":\"text\",\"text\":\"Reading list\"}]},{\"type\":\"zoteroEndnoteSection\",\"attrs\":{\"style\":\"apa\",\"entriesJson\":\"[{\\\"citationId\\\":\\\"import-only\\\",\\\"number\\\":1,\\\"entryHtml\\\":\\\"<i>独立文献 &amp; title</i>.\\\",\\\"key\\\":\\\"UNREFERENCED\\\",\\\"itemUri\\\":\\\"zotero://select/library/items/UNREFERENCED\\\",\\\"pdfUri\\\":\\\"\\\"}]\"}}]}";
    for(unsigned cited=0;cited<2;cited++) for(unsigned custom=0;custom<2;custom++) {
        cJSON *tree=cJSON_Parse(fixture);assert(tree);
        cJSON *content=cJSON_GetObjectItemCaseSensitive(tree,"content");
        cJSON *attrs=cJSON_GetObjectItemCaseSensitive(cJSON_GetArrayItem(content,1),"attrs");
        if(custom) {
            cJSON *metadata=cJSON_Parse("{\"flag\":true,\"weight\":42,\"labels\":[\"原样保留\"]}");assert(metadata);
            assert(cJSON_AddItemToObject(attrs,"futureSection",metadata));
            cJSON *value=cJSON_GetObjectItemCaseSensitive(attrs,"entriesJson");
            cJSON *entries=cJSON_Parse(value->valuestring);assert(entries);
            metadata=cJSON_Parse("{\"enabled\":false,\"value\":9,\"text\":\"<literal>&\"}");assert(metadata);
            assert(cJSON_AddItemToObject(cJSON_GetArrayItem(entries,0),"futureEntry",metadata));
            char *serialized=cJSON_PrintUnformatted(entries);assert(serialized);
            assert(cJSON_SetValuestring(value,serialized));cJSON_free(serialized);cJSON_Delete(entries);
        }
        if(cited) {
            cJSON *citation=cJSON_Parse("{\"type\":\"zoteroCitationBlock\",\"attrs\":{\"citationId\":\"cited\",\"sourceId\":\"cited-book\",\"title\":\"Cited book\",\"cslJson\":\"{\\\"id\\\":\\\"cited-book\\\",\\\"type\\\":\\\"book\\\",\\\"title\\\":\\\"Cited book\\\"}\"}}");assert(citation);
            assert(cJSON_InsertItemInArray(content,1,citation));
            cJSON *value=cJSON_GetObjectItemCaseSensitive(attrs,"entriesJson");
            cJSON *entries=cJSON_Parse(value->valuestring);assert(entries);
            cJSON *matched=cJSON_Parse("{\"citationId\":\"cited\",\"number\":1,\"entryHtml\":\"<i>Cited book</i>.\",\"key\":\"cited-book\",\"itemUri\":\"\",\"pdfUri\":\"\"}");assert(matched);
            cJSON_SetNumberValue(cJSON_GetObjectItemCaseSensitive(cJSON_GetArrayItem(entries,0),"number"),2);
            assert(cJSON_InsertItemInArray(entries,0,matched));
            char *serialized=cJSON_PrintUnformatted(entries);assert(serialized);
            assert(cJSON_SetValuestring(value,serialized));cJSON_free(serialized);cJSON_Delete(entries);
        }
        char *original=cJSON_PrintUnformatted(tree);assert(original);cJSON_Delete(tree);
        char name[64];SDL_snprintf(name,sizeof name,"Legacy bibliography %u-%u.json",cited,custom);
        note_store_delete(&a->store,name);
        assert(note_store_create(&a->store,name,original,strlen(original))==NOTE_OK);
        open_note(a,name);assert(!strcmp(a->active,name));
        /* Opening a legacy reading list is not authorization to delete entries
         * which have no matching citation, even with another valid citation. */
        unchanged(a,original);
        assert(!a->citation_session && *a->citation_warning && !a->citation_normalized_pending);
        assert(!ce_document_can_undo(a->doc));
        CeDocument *expected=ce_document_new();char error[256];assert(expected);
        assert(ce_document_import(expected,original,CE_FORMAT_JSON,error,sizeof error));
        assert(ce_document_insert(expected,0,"Edited "));char *edited=ce_document_export_json(expected);assert(edited);
        ce_document_free(expected);cJSON_free(original);
        a->edit->cursor=0;a->edit->highlight1=a->edit->highlight2=-1;EditBox_SetFocus(a->edit,true);
        input(a,"Edited ");unchanged(a,edited);assert(!a->citation_session);
        assert(save_document(a));
        char *saved=NULL;size_t size=0;assert(note_store_read(&a->store,name,&saved,&size)==NOTE_OK);
        assert(size==strlen(edited) && !memcmp(saved,edited,size));note_store_free(saved);
        open_note(a,name);unchanged(a,edited);
        assert(!a->citation_session && *a->citation_warning && !a->citation_normalized_pending);
        free(edited);open_note(a,return_to);assert(!strcmp(a->active,return_to));
        assert(note_store_delete(&a->store,name)==NOTE_OK);
    }
    free(return_to);
}
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
    assert(a->sidebar && !a->sidebar_manual);
    assert(SDL_SetWindowSize(a->window,320,640)); layout(a);
    assert(!a->sidebar && !a->note_import_button.w && a->editor_area.x==24);
    action(a,ACT_SIDEBAR); layout(a);
    assert(a->sidebar && a->sidebar_manual && a->note_import_button.w>0);
    action(a,ACT_SIDEBAR); layout(a); assert(!a->sidebar);
    assert(SDL_SetWindowSize(a->window,1100,760)); layout(a);
    assert(!a->sidebar); /* Explicit choices survive resizing and drawing. */
    a->sidebar_manual=false; layout(a); assert(a->sidebar);
    const char *initial="{\"type\":\"doc\",\"content\":[{\"type\":\"paragraph\",\"content\":[{\"type\":\"text\",\"text\":\"前后\"}]}]}";
    note_store_delete(&a->store,"Ui.json"); assert(note_store_create(&a->store,"Ui.json",initial,strlen(initial))==NOTE_OK);
    char error[256]; assert(ce_document_import(a->doc,initial,CE_FORMAT_JSON,error,sizeof error));
    free(a->active); a->active=copy_string("Ui.json"); sync_view(a); a->edit->cursor=3; EditBox_SetFocus(a->edit,true);
    char *before=doc_json(a); library_show(a); assert(!a->edit->has_focus && !a->library_citation_mode);
    press(a,LIB_IMPORT); library_import_poll(a); assert(a->library.library.item_count==2);
    test_selected_bibliography_ui(a);
    test_library_source_controller(a);
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
    assert(!strcmp(ce_document_text(a->doc),"前\n\xef\xbf\xbc\n后\n\xef\xbf\xbc"));
    char *with_citation=doc_json(a); assert(strstr(with_citation,"12–14") && strstr(with_citation,"cslJson"));
    assert(strstr(with_citation,"cslRenderJson") && strstr(with_citation,"zoteroEndnoteSection") && !strstr(with_citation,"approximate"));
    citation_is(a,"(张, 2024, pp. 12–14)");bibliography_is(a,"张三. (2024). 中文论文🙂.");
    layout(a);draw(a);assert(a->rich_valid);
    CeBlockInfo citation_block,reference_block;SDL_FRect card,reference;
    assert(ce_document_block_info(a->doc,1,&citation_block) && citation_block.kind==CE_BLOCK_CITATION);
    assert(ce_document_block_info(a->doc,3,&reference_block) && reference_block.kind==CE_BLOCK_ENDNOTES);
    assert(ce_rich_layout_caret(a->rich,citation_block.start,&card) && ce_rich_layout_caret(a->rich,reference_block.start,&reference));
    assert(card.h>TTF_GetFontHeight(a->font)*2 && reference.y>card.y && reference.h>TTF_GetFontHeight(a->font));
    SDL_Surface *formal_frame=SDL_RenderReadPixels(a->renderer,NULL);assert(formal_frame);
    char *formal_path=NULL;assert(SDL_asprintf(&formal_path,"%s/formal-citation.bmp",argv[1])>0);
    assert(SDL_SaveBMP(formal_frame,formal_path));SDL_free(formal_path);SDL_DestroySurface(formal_frame);
    action(a,ACT_UNDO); unchanged(a,before);
    assert(ce_citation_prepared_stats(ce_citation_session_prepared(a->citation_session))->citation_count==0);
    action(a,ACT_REDO); unchanged(a,with_citation); citation_is(a,"(张, 2024, pp. 12–14)"); sync_view(a);a->edit->cursor=4;
    library_show(a); assert(a->library_citation_mode && a->citation_id && *a->citation_id);
    press(a,LIB_PAGE); EditBox_SetText(a->prompt,""); a->prompt->cursor=0;key(a,SDLK_RETURN,0);
    press(a,LIB_INSERT); char *updated=doc_json(a); assert(strstr(updated,"\"page\":\"\"")); citation_is(a,"(张, 2024)");
    action(a,ACT_UNDO); unchanged(a,with_citation); action(a,ACT_REDO); sync_view(a);a->edit->cursor=4;
    library_show(a); press(a,LIB_SYNC); unchanged(a,updated); /* Explicit sync is idempotent. */
    action(a,ACT_UNDO); unchanged(a,with_citation); action(a,ACT_REDO); unchanged(a,updated);
    assert(a->citation_session && a->citation_style==CE_CITATION_PROFILE_APA);
    press(a,LIB_STYLE); assert(a->citation_style==CE_CITATION_PROFILE_IEEE); citation_is(a,"[1]");
    char *ieee=doc_json(a); assert(strstr(ieee,"cslRenderJson") && !strstr(ieee,"approximate"));
    press(a,LIB_STYLE);assert(a->citation_style==CE_CITATION_PROFILE_GBT);citation_is(a,"[1]");
    char *gbt=doc_json(a);assert(strstr(gbt,"gbt-7714-2015-numeric"));
    action(a,ACT_UNDO);unchanged(a,ieee);assert(a->citation_style==CE_CITATION_PROFILE_IEEE);
    action(a,ACT_REDO);unchanged(a,gbt);assert(a->citation_style==CE_CITATION_PROFILE_GBT);
    action(a,ACT_UNDO);unchanged(a,ieee);free(gbt);
    action(a,ACT_UNDO); unchanged(a,updated); assert(a->citation_style==CE_CITATION_PROFILE_APA);
    action(a,ACT_REDO); unchanged(a,ieee); assert(a->citation_style==CE_CITATION_PROFILE_IEEE);
    action(a,ACT_UNDO); unchanged(a,updated); free(ieee);
    char *synced=doc_json(a);
    press(a,LIB_ITEMS); a->library.selected_item=1;
    fixture_json="not JSON"; press(a,LIB_IMPORT);library_import_poll(a);assert(a->library.library.item_count==2 && a->library.selected_item==1); unchanged(a,synced);
    press(a,LIB_SEARCH);input(a,"中文");key(a,SDLK_RETURN,0);assert(a->library.row_count==1 && a->library.selected_item==SIZE_MAX);
    SDL_SetWindowSize(a->window,390,400);layout(a);assert(a->width==390);press(a,LIB_VIEW);layout(a);
    assert(!a->sidebar);
    assert(a->library_buttons[LIB_INSERT].y>=56 && a->library_buttons[LIB_SYNC].y+a->library_buttons[LIB_SYNC].h<=a->library_area.h);
    press(a,LIB_CLOSE); assert(a->edit->has_focus);
    assert(save_document(a));
    action(a,ACT_SIDEBAR); layout(a); assert(a->sidebar);
    open_note(a,"Ui.json"); layout(a); assert(!a->sidebar); unchanged(a,synced);
    assert(a->citation_session && a->citation_style==CE_CITATION_PROFILE_APA);citation_is(a,"(张, 2024)");
    /* A repaired duplicate ID after an old in-flow footer must still edit the
     * originally selected citation, not the earlier block sharing its ID. */
    const char *duplicates="{\"type\":\"doc\",\"content\":[{\"type\":\"zoteroCitationBlock\",\"attrs\":{\"citationId\":\"dup\",\"itemUri\":\"https://example.invalid/items/first\",\"title\":\"First\",\"cslJson\":\"{\\\"id\\\":\\\"first\\\",\\\"type\\\":\\\"book\\\",\\\"title\\\":\\\"First\\\",\\\"author\\\":[{\\\"family\\\":\\\"First\\\",\\\"given\\\":\\\"Alice\\\"}],\\\"issued\\\":{\\\"date-parts\\\":[[2020]]}}\"}},{\"type\":\"zoteroEndnoteSection\",\"attrs\":{\"entriesJson\":\"[]\"}},{\"type\":\"zoteroCitationBlock\",\"attrs\":{\"citationId\":\"dup\",\"itemUri\":\"https://example.invalid/items/second\",\"title\":\"Second\",\"cslJson\":\"{\\\"id\\\":\\\"second\\\",\\\"type\\\":\\\"book\\\",\\\"title\\\":\\\"Second\\\",\\\"author\\\":[{\\\"family\\\":\\\"Second\\\",\\\"given\\\":\\\"Alice\\\"}],\\\"issued\\\":{\\\"date-parts\\\":[[2020]]}}\"}}]}";
    assert(ce_document_import(a->doc,duplicates,CE_FORMAT_JSON,error,sizeof error));
    ce_citation_session_free(a->citation_session); a->citation_session=NULL; ++a->citation_revision; sync_view(a); a->edit->cursor=8;
    library_show(a); library_action(a,LIB_SYNC);
    assert(a->edit->cursor==4 && a->citation_id && strcmp(a->citation_id,"dup"));
    assert(a->citation_title && !strcmp(a->citation_title,"Second"));
    const CeCitationProcessStats *stats=ce_citation_prepared_stats(ce_citation_session_prepared(a->citation_session));
    assert(stats && stats->citation_count==2 && stats->reference_count==2 && stats->repaired_ids==1);
    library_close(a);test_missing_snapshot(a);
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
    /* Exercise the production screenshot path: it includes device insets and
     * must restore the UI viewport after reading the complete framebuffer. */
    assert(SDL_asprintf(&image_path,"%s/safe-area.bmp",argv[1])>0);
    a->screenshot=image_path; a->frames=2; draw(a); a->screenshot=NULL; a->frames=0;
    pixels=SDL_LoadBMP(image_path); assert(pixels && pixels->w==393 && pixels->h==852);
    assert(SDL_GetRenderViewport(a->renderer,&viewport));
    assert(viewport.x==12 && viewport.y==59 && viewport.w==369 && viewport.h==759);
    SDL_free(image_path); SDL_DestroySurface(pixels); simulate_safe_area=false;
    layout(a); test_legacy_import(a); test_legacy_bibliography_preserved(a);
    /* Fail a cache write without altering the previous on-disk cache. The
     * injected root points through an existing file, not an actual user path. */
    char *cached=NULL; size_t cached_size=0;
    assert(note_store_read(&a->library_store,"library.json",&cached,&cached_size)==NOTE_OK);
    note_store_destroy(&a->library_store); a->library_store_ready=false;
    char *original_root=a->notes_root,*bad_root=NULL; assert(SDL_asprintf(&bad_root,"%s/Ui.json",original_root)>0);
    a->notes_root=copy_string(bad_root); SDL_free(bad_root);
    fixture_filename="cache.json"; fixture_json="[{\"id\":\"persisted\",\"type\":\"book\",\"title\":\"缓存恢复中文\",\"author\":[{\"family\":\"Cache\",\"given\":\"Casey\"}],\"issued\":{\"date-parts\":[[2025]]}}]";
    library_show(a); library_action(a,LIB_IMPORT); library_import_poll(a);
    assert(a->library.library.item_count==1 && a->library_unsaved && *a->library_warning && !a->library_cache_saving);
    assert(strstr(a->status,"尚未持久保存"));
    free(a->notes_root); a->notes_root=original_root;
    assert(library_cache_store_open(a)); char *still_cached=NULL; size_t still_size=0;
    assert(note_store_read(&a->library_store,"library.json",&still_cached,&still_size)==NOTE_OK);
    assert(cached_size==still_size && !memcmp(cached,still_cached,cached_size)); note_store_free(cached); note_store_free(still_cached);
    library_action(a,LIB_RETRY); assert(!a->library_unsaved && !*a->library_warning); library_close(a);
    /* Startup must load the user's saved library before resolving the first
     * old note. This source deliberately has no embedded CSL snapshot. */
    const char *startup_name="000 Legacy Cache.json";
    const char *startup_json="{\"type\":\"doc\",\"content\":[{\"type\":\"zoteroCitationBlock\",\"attrs\":{\"citationId\":\"restart\",\"sourceId\":\"persisted\",\"title\":\"缓存恢复中文\"}}]}";
    note_store_delete(&a->store,startup_name);
    assert(note_store_create(&a->store,startup_name,startup_json,strlen(startup_json))==NOTE_OK);
    free(before);free(with_citation);free(updated);free(synced);SDL_AppQuit(a,SDL_APP_SUCCESS);
    state=NULL; assert(SDL_AppInit(&state,5,args)==SDL_APP_CONTINUE); a=state;
    assert(a->library.library.item_count==1 && !strcmp(a->library.library.items[0].title,"缓存恢复中文"));
    assert(!*a->library_warning && !a->library_unsaved);
    assert(!strcmp(a->active,startup_name) && a->citation_session && !*a->citation_warning);
    citation_is(a,"(Cache, 2025)");
    char *restored=doc_json(a);assert(strstr(restored,"cslJson") && strstr(restored,"cslRenderJson"));free(restored);
    assert(a->citation_normalized_pending && save_document(a));
    char *saved_restart=NULL;size_t saved_restart_size=0;
    assert(note_store_read(&a->store,startup_name,&saved_restart,&saved_restart_size)==NOTE_OK);
    assert(strstr(saved_restart,"cslJson") && strstr(saved_restart,"cslRenderJson"));note_store_free(saved_restart);
    assert(note_store_save(&a->library_store,"library.json","broken cache",12)==NOTE_OK); SDL_AppQuit(a,SDL_APP_SUCCESS);
    state=NULL; assert(SDL_AppInit(&state,5,args)==SDL_APP_CONTINUE); a=state;
    assert(!a->library.library.item_count && a->library_restore_failed && *a->library_warning && !a->library_unsaved);
    open_note(a,"Ui.json"); assert(!strcmp(a->status,a->library_warning));
    cached=NULL; assert(note_store_read(&a->library_store,"library.json",&cached,&cached_size)==NOTE_OK);
    assert(cached_size==12 && !memcmp(cached,"broken cache",12)); note_store_free(cached);
    assert(note_store_delete(&a->library_store,"library.json")==NOTE_OK);
    assert(note_store_delete(&a->store,startup_name)==NOTE_OK);SDL_AppQuit(a,SDL_APP_SUCCESS);
    puts("actual app citations, cache recovery/failure, safe-area HiDPI and non-overwriting legacy import passed");return 0;
}
