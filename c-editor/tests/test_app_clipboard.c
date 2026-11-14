#define SDL_MAIN_HANDLED 1
#include "../src/app.c"
#undef main
#include <assert.h>

static void reset_document(App *a,const char *source,CeFormat format) {
    char error[512]; assert(ce_document_import(a->doc,source,format,error,sizeof error));
    ce_citation_session_free(a->citation_session); a->citation_session=NULL;
    citation_refresh(a); a->typing_mask=0; a->prompt_mode=PROMPT_NONE;
    sync_view(a); a->edit->cursor=0; EditBox_SetFocus(a->edit,true); layout(a);
}
static char *snapshot(App *a) { char *s=ce_document_export_json(a->doc); assert(s); return s; }
static void unchanged(App *a,const char *before) { char *now=snapshot(a); assert(!strcmp(now,before)); free(now); }
static void select_range(App *a,size_t start,size_t end) {
    a->edit->highlight1=(int)start; a->edit->highlight2=a->edit->cursor=(int)end;
}
static void input(App *a,const char *text) {
    SDL_Event event={0}; event.type=SDL_EVENT_TEXT_INPUT; event.text.text=text;
    assert(SDL_AppEvent(a,&event)==SDL_APP_CONTINUE);
}
static void key(App *a,SDL_Keycode code,SDL_Keymod mod) {
    SDL_Event event={0}; event.type=SDL_EVENT_KEY_DOWN; event.key.key=code; event.key.mod=mod;
    assert(SDL_AppEvent(a,&event)==SDL_APP_CONTINUE);
}
static void test_typing(App *a) {
    reset_document(a,"aa",CE_FORMAT_TEXT);
    char *before=snapshot(a);
    action(a,ACT_BOLD); unchanged(a,before); assert(!ce_document_can_undo(a->doc));
    input(a,"a"); assert(!strcmp(ce_document_text(a->doc),"aaa"));
    assert(ce_document_style_at(a->doc,0).marks&CE_MARK_BOLD);
    assert(!(ce_document_style_at(a->doc,1).marks&CE_MARK_BOLD));
    input(a,"中文🙂"); assert(ce_document_style_at(a->doc,1).marks&CE_MARK_BOLD);
    action(a,ACT_BOLD); input(a,"x"); assert(!(ce_document_style_at(a->doc,11).marks&CE_MARK_BOLD));
    action(a,ACT_UNDO); assert(!strstr(ce_document_text(a->doc),"x"));
    action(a,ACT_UNDO); assert(!strcmp(ce_document_text(a->doc),"aaa"));
    action(a,ACT_UNDO); unchanged(a,before); free(before);
    reset_document(a,"<p><strong>aa</strong></p>",CE_FORMAT_HTML); a->edit->cursor=1;
    action(a,ACT_CLEAR); input(a,"z"); assert(!strcmp(ce_document_text(a->doc),"aza"));
    assert(ce_document_style_at(a->doc,0).marks&CE_MARK_BOLD);
    assert(!(ce_document_style_at(a->doc,1).marks&CE_MARK_BOLD));
    assert(ce_document_style_at(a->doc,2).marks&CE_MARK_BOLD);
    action(a,ACT_UNDO); assert(!strcmp(ce_document_text(a->doc),"aa"));
    reset_document(a,"aa",CE_FORMAT_TEXT); action(a,ACT_BOLD);
    key(a,SDLK_RIGHT,0); key(a,SDLK_LEFT,0); input(a,"b");
    assert(!(ce_document_style_at(a->doc,0).marks&CE_MARK_BOLD));
    reset_document(a,"",CE_FORMAT_TEXT); action(a,ACT_ITALIC);
    SDL_Event ime={0}; ime.type=SDL_EVENT_TEXT_EDITING; ime.edit.text="拼音"; ime.edit.start=0; ime.edit.length=2;
    assert(SDL_AppEvent(a,&ime)==SDL_APP_CONTINUE); assert(!*ce_document_text(a->doc));
    input(a,"中文"); assert(!strcmp(ce_document_text(a->doc),"中文"));
    assert(ce_document_style_at(a->doc,0).marks&CE_MARK_ITALIC);
    const char *duplicate="<p><strong>a</strong>a</p>";
    reset_document(a,duplicate,CE_FORMAT_HTML); a->edit->cursor=1; key(a,SDLK_BACKSPACE,0);
    assert(!strcmp(ce_document_text(a->doc),"a") && !ce_document_style_at(a->doc,0).marks);
    reset_document(a,duplicate,CE_FORMAT_HTML); key(a,SDLK_DELETE,0);
    assert(!strcmp(ce_document_text(a->doc),"a") && !ce_document_style_at(a->doc,0).marks);
    reset_document(a,"<p><strong>中</strong>中</p>",CE_FORMAT_HTML); a->edit->cursor=3;
    key(a,SDLK_BACKSPACE,0); assert(!strcmp(ce_document_text(a->doc),"中") && !ce_document_style_at(a->doc,0).marks);
    reset_document(a,"<p><strong>a</strong>a<em>a</em></p>",CE_FORMAT_HTML);
    select_range(a,0,2); ime.edit.text="x"; ime.edit.length=1;
    assert(SDL_AppEvent(a,&ime)==SDL_APP_CONTINUE); input(a,"a");
    assert(!strcmp(ce_document_text(a->doc),"aa") && (ce_document_style_at(a->doc,1).marks&CE_MARK_ITALIC));
}
static void test_clipboard(App *a) {
    reset_document(a,"<p>A<strong>中文🙂</strong>Z</p>",CE_FORMAT_HTML);
    select_range(a,1,11); PlatformWebClipboardData data={0};
    char *before=snapshot(a); assert(browser_clipboard_prepare(true,&data,a));
    assert(!strcmp(data.plain,"中文🙂") && strstr(data.html,"data-notelab-slice") && strstr(data.typed_json,"bold"));
    char *typed=copy_string(data.typed_json),*html=copy_string(data.html); assert(typed && html);
    browser_clipboard_finish(false,a); unchanged(a,before); assert(!ce_document_can_undo(a->doc));
    assert(browser_clipboard_prepare(true,&data,a)); select_range(a,0,1); browser_clipboard_finish(true,a); unchanged(a,before);
    select_range(a,1,11); assert(browser_clipboard_prepare(true,&data,a));
    browser_clipboard_finish(true,a); assert(!strcmp(ce_document_text(a->doc),"AZ"));
    action(a,ACT_UNDO); unchanged(a,before); free(before);
    reset_document(a,"q",CE_FORMAT_TEXT); a->edit->cursor=1;
    data=(PlatformWebClipboardData){"plain fallback",NULL,typed}; assert(browser_clipboard_paste(&data,a));
    assert(!strcmp(ce_document_text(a->doc),"q中文🙂"));
    assert(ce_document_style_at(a->doc,1).marks&CE_MARK_BOLD);
    action(a,ACT_UNDO); assert(!strcmp(ce_document_text(a->doc),"q"));
    a->edit->cursor=1; data=(PlatformWebClipboardData){"plain fallback",html,NULL}; assert(browser_clipboard_paste(&data,a));
    assert(!strcmp(ce_document_text(a->doc),"q中文🙂"));
    before=snapshot(a); data=(PlatformWebClipboardData){"must not replace",NULL,"{invalid"};
    assert(!browser_clipboard_paste(&data,a)); unchanged(a,before); free(before);
    EditorClipboardRequest *pending=clipboard_request(a,true,true); /* collapsed: no copy */
    assert(!pending);
    select_range(a,1,11); pending=clipboard_request(a,true,true); assert(pending);
    assert(edit_replace(a,0,0,"r")); sync_view(a); select_range(a,1,11);
    before=snapshot(a); clipboard_write_complete(true,NULL,NULL,NULL,NULL,pending); unchanged(a,before); free(before);
    pending=clipboard_request(a,false,false); assert(pending);
    free(a->active); a->active=copy_string("another.json"); before=snapshot(a);
    clipboard_read_complete(true,"bad",NULL,NULL,NULL,pending); unchanged(a,before); free(before);
    reset_document(a,"clipboard native",CE_FORMAT_TEXT); select_range(a,0,9);
    editor_clipboard_action(a,CE_EDITOR_COPY); assert(SDL_HasClipboardData(CE_DOCUMENT_SLICE_MIME));
    select_range(a,16,16); editor_clipboard_action(a,CE_EDITOR_PASTE);
    assert(!strcmp(ce_document_text(a->doc),"clipboard nativeclipboard"));
    show_prompt(a,PROMPT_FIND); pending=clipboard_request(a,false,false); assert(pending);
    close_prompt(a); show_prompt(a,PROMPT_NEW);
    clipboard_read_complete(true,"stale filename",NULL,NULL,NULL,pending);
    assert(!a->prompt->text->text || !*a->prompt->text->text);
    pending=clipboard_request(a,false,false); assert(pending);
    close_prompt(a); show_prompt(a,PROMPT_NEW);
    clipboard_read_complete(true,"same-kind stale filename",NULL,NULL,NULL,pending);
    assert(!a->prompt->text->text || !*a->prompt->text->text); close_prompt(a);
    free(typed); free(html);
}
static void test_citation_clipboard(App *a) {
    reset_document(a,"original",CE_FORMAT_TEXT);
    CeDocument *candidate=edit_candidate(a); assert(candidate);
    CeZoteroItem item={0}; item.id=(char *)"A"; item.item_key=(char *)"A";
    item.title=(char *)"First book";
    item.csl_json=(char *)"{\"id\":\"A\",\"type\":\"book\",\"title\":\"First book\",\"author\":[{\"family\":\"Doe\",\"given\":\"Jane\"}],\"issued\":{\"date-parts\":[[2020]]}}";
    char *id=NULL; assert(ce_citation_insert(candidate,0,&item,"12","quotation",&id,a->edit_error,sizeof a->edit_error));
    assert(edit_commit(a,candidate,true,NULL,false)); sync_view(a);
    CeBlockInfo citation; assert(ce_document_block_info(a->doc,0,&citation) && citation.kind==CE_BLOCK_CITATION);
    select_range(a,citation.start,citation.end); PlatformWebClipboardData data;
    assert(browser_clipboard_prepare(false,&data,a)); assert(strstr(data.plain,"Doe") && !strstr(data.plain,"\xef\xbf\xbc"));
    char *typed=copy_string(data.typed_json); assert(typed); browser_clipboard_finish(true,a);
    CeBlockInfo prose; assert(ce_document_block_info(a->doc,1,&prose) && prose.kind==CE_BLOCK_PARAGRAPH);
    select_range(a,prose.end,prose.end); char *before=snapshot(a);
    data=(PlatformWebClipboardData){"fallback",NULL,typed};
    if(!browser_clipboard_paste(&data,a)) fprintf(stderr,"citation paste: %s\n",a->status);
    assert(ce_citation_prepared_stats(ce_citation_session_prepared(a->citation_session))->citation_count==2);
    size_t citations=0; bool original=false,distinct=false;
    for(size_t i=0;i<ce_document_block_count(a->doc);++i) {
        CeBlockInfo block; assert(ce_document_block_info(a->doc,i,&block)); if(block.kind!=CE_BLOCK_CITATION) continue;
        ++citations; char *json=ce_document_block_json(a->doc,i); cJSON *node=cJSON_Parse(json); free(json); assert(node);
        cJSON *attrs=cJSON_GetObjectItemCaseSensitive(node,"attrs"),*key=cJSON_GetObjectItemCaseSensitive(attrs,"citationId");
        assert(cJSON_IsString(key)); original|=!strcmp(key->valuestring,id); distinct|=strcmp(key->valuestring,id)!=0;
        cJSON_Delete(node);
    }
    assert(citations==2 && original && distinct);
    action(a,ACT_UNDO); unchanged(a,before); free(before);
    /* A pasted atom cannot quietly replace another snapshot of the same source. */
    cJSON *envelope=cJSON_Parse(typed); assert(envelope);
    cJSON *atom=cJSON_GetArrayItem(cJSON_GetObjectItemCaseSensitive(envelope,"content"),0);
    cJSON *attrs=cJSON_GetObjectItemCaseSensitive(atom,"attrs"),*source=cJSON_GetObjectItemCaseSensitive(attrs,"cslJson");
    assert(cJSON_IsString(source)); cJSON *record=cJSON_Parse(source->valuestring); assert(record);
    assert(cJSON_SetValuestring(cJSON_GetObjectItemCaseSensitive(record,"title"),"Conflicting title"));
    char *record_text=cJSON_PrintUnformatted(record); assert(record_text && cJSON_SetValuestring(source,record_text));
    cJSON_free(record_text); cJSON_Delete(record);
    char *conflict=cJSON_PrintUnformatted(envelope); assert(conflict); cJSON_Delete(envelope);
    assert(ce_document_block_info(a->doc,1,&prose)); select_range(a,prose.end,prose.end); before=snapshot(a);
    data=(PlatformWebClipboardData){"fallback",NULL,conflict}; assert(!browser_clipboard_paste(&data,a)); unchanged(a,before);
    free(before); cJSON_free(conflict); free(typed); free(id);
}
int main(int argc,char **argv) {
    assert(argc>=3); void *state=NULL;
    char *args[]={"clipboard-test","--notes",argv[1],"--font",argv[2]};
    assert(SDL_AppInit(&state,5,args)==SDL_APP_CONTINUE); App *a=state;
    free(a->active); a->active=copy_string("clipboard.json"); assert(a->active);
    test_typing(a); test_clipboard(a); test_citation_clipboard(a);
    SDL_AppQuit(a,SDL_APP_SUCCESS); puts("application typing and clipboard transactions passed"); return 0;
}
