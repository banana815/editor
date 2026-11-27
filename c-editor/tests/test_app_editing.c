#define SDL_MAIN_HANDLED 1
#include "../src/app.c"
#undef main
#include <assert.h>
#ifdef _MSC_VER
#include <crtdbg.h>
#endif

static void reset_editing(App *a,const char *text,CeFormat format) {
    char error[256]; assert(ce_document_import(a->doc,text,format,error,sizeof error));
    citation_refresh(a); sync_view(a); editing_break(a); a->typing_mask=0;
    a->edit->cursor=0; EditBox_SetFocus(a->edit,true); layout(a);
}
static void input_editing(App *a,const char *text) {
    SDL_Event event={0}; event.type=SDL_EVENT_TEXT_INPUT; event.text.text=text;
    assert(SDL_AppEvent(a,&event)==SDL_APP_CONTINUE);
}
static void key_editing(App *a,SDL_Keycode key,SDL_Keymod mod) {
    SDL_Event event={0}; event.type=SDL_EVENT_KEY_DOWN; event.key.key=key; event.key.mod=mod;
    assert(SDL_AppEvent(a,&event)==SDL_APP_CONTINUE);
}
static void text_is(App *a,const char *text) { assert(!strcmp(ce_document_text(a->doc),text)); }
static void test_groups(App *a) {
    reset_editing(a,"",CE_FORMAT_TEXT);
    input_editing(a,"a"); input_editing(a,"中"); input_editing(a,"🙂");
    text_is(a,"a中🙂"); action(a,ACT_UNDO); text_is(a,""); assert(!ce_document_can_undo(a->doc));
    action(a,ACT_REDO); text_is(a,"a中🙂");
    reset_editing(a,"",CE_FORMAT_TEXT); input_editing(a,"a"); a->undo_typed_at-=501;
    input_editing(a,"b"); action(a,ACT_UNDO); text_is(a,"a");
    reset_editing(a,"",CE_FORMAT_TEXT); input_editing(a,"a");
    key_editing(a,SDLK_LEFT,0); key_editing(a,SDLK_RIGHT,0); input_editing(a,"b");
    action(a,ACT_UNDO); text_is(a,"a");
    reset_editing(a,"",CE_FORMAT_TEXT); input_editing(a,"a"); action(a,ACT_BOLD);
    input_editing(a,"b"); input_editing(a,"c"); action(a,ACT_UNDO); text_is(a,"a");
    reset_editing(a,"",CE_FORMAT_TEXT); input_editing(a,"a");
    PlatformWebClipboardData paste={"PASTE",NULL,NULL}; assert(browser_clipboard_paste(&paste,a));
    input_editing(a,"b"); input_editing(a,"c"); action(a,ACT_UNDO); text_is(a,"aPASTE");
    action(a,ACT_UNDO); text_is(a,"a"); action(a,ACT_UNDO); text_is(a,"");
    reset_editing(a,"",CE_FORMAT_TEXT); input_editing(a,"a");
    SDL_Event ime={0}; ime.type=SDL_EVENT_TEXT_EDITING; ime.edit.text="拼音"; ime.edit.length=2;
    assert(SDL_AppEvent(a,&ime)==SDL_APP_CONTINUE); input_editing(a,"中"); input_editing(a,"b");
    action(a,ACT_UNDO); text_is(a,"a中"); action(a,ACT_UNDO); text_is(a,"a");
    reset_editing(a,"",CE_FORMAT_TEXT); input_editing(a,"a"); assert(save_document(a));
    input_editing(a,"b"); input_editing(a,"c"); action(a,ACT_UNDO); text_is(a,"a");
    assert(!ce_document_is_modified(a->doc));
    reset_editing(a,"",CE_FORMAT_TEXT); input_editing(a,"a");
    a->edit->highlight1=0; a->edit->highlight2=1; input_editing(a,"b"); input_editing(a,"c");
    action(a,ACT_UNDO); text_is(a,"b"); action(a,ACT_UNDO); text_is(a,"a");
}
static void test_links(App *a) {
    reset_editing(a,"<p>Start <a href=\"https://example.test/old\">one<strong>中文</strong>three</a> tail</p>",CE_FORMAT_HTML);
    char *before=ce_document_export_json(a->doc); assert(before);
    a->edit->cursor=12; key_editing(a,SDLK_K,SDL_KMOD_CTRL);
    assert(a->prompt_mode==PROMPT_LINK && a->selection_start==6 && a->selection_end==20);
    assert(!strcmp(a->prompt->text->text,"https://example.test/old"));
    EditBox_SetText(a->prompt,"https://example.test/new"); accept_prompt(a);
    assert(!strcmp(ce_document_style_at(a->doc,6).link,"https://example.test/new"));
    assert(!strcmp(ce_document_style_at(a->doc,19).link,"https://example.test/new"));
    assert(ce_document_style_at(a->doc,9).marks&CE_MARK_BOLD);
    action(a,ACT_UNDO); char *restored=ce_document_export_json(a->doc);
    assert(restored && !strcmp(restored,before)); free(restored); free(before);
    a->edit->cursor=12; action(a,ACT_LINK); EditBox_SetText(a->prompt,""); accept_prompt(a);
    assert(!(ce_document_style_at(a->doc,6).marks&CE_MARK_LINK) && !(ce_document_style_at(a->doc,19).marks&CE_MARK_LINK));
    assert(ce_document_style_at(a->doc,9).marks&CE_MARK_BOLD);
    text_is(a,"Start one中文three tail");
    reset_editing(a,"<p><a href=\"https://a.test\">one</a><a href=\"https://b.test\">two</a></p>",CE_FORMAT_HTML);
    a->edit->cursor=4; action(a,ACT_LINK);
    assert(a->selection_start==3 && a->selection_end==6 && !strcmp(a->prompt->text->text,"https://b.test")); close_prompt(a);
}
static void mouse_editing_at(App *a,SDL_EventType type,int offset,int clicks,float delta) {
    SDL_FRect caret; assert(EditBox_CaretRect(a->edit,offset,&caret));
    float x=a->edit->rect.x+caret.x+delta,y=a->edit->rect.y+caret.y+caret.h*.4f;
    SDL_Event event={0}; event.type=type;
    if(type==SDL_EVENT_MOUSE_MOTION) { event.motion.x=x; event.motion.y=y; event.motion.state=SDL_BUTTON_LMASK; }
    else { event.button.x=x; event.button.y=y; event.button.button=SDL_BUTTON_LEFT; event.button.clicks=(Uint8)clicks; }
    assert(SDL_AppEvent(a,&event)==SDL_APP_CONTINUE);
}
static void mouse_editing(App *a,SDL_EventType type,int offset,int clicks) { mouse_editing_at(a,type,offset,clicks,1); }
static void selected_is(App *a,size_t start,size_t end) {
    size_t actual_start,actual_end; get_selection(a,&actual_start,&actual_end);
    assert(start==actual_start && end==actual_end);
}
static void test_mouse(App *a) {
    reset_editing(a,"one two three\n中文🙂 next\nlast",CE_FORMAT_TEXT);
    mouse_editing_at(a,SDL_EVENT_MOUSE_BUTTON_DOWN,3,2,-1); selected_is(a,0,3);
    mouse_editing_at(a,SDL_EVENT_MOUSE_BUTTON_UP,3,2,-1);
    mouse_editing(a,SDL_EVENT_MOUSE_BUTTON_DOWN,5,2); selected_is(a,4,7);
    mouse_editing(a,SDL_EVENT_MOUSE_MOTION,10,0); selected_is(a,4,13);
    mouse_editing(a,SDL_EVENT_MOUSE_MOTION,1,0); selected_is(a,0,7);
    mouse_editing(a,SDL_EVENT_MOUSE_BUTTON_UP,1,2);
    mouse_editing(a,SDL_EVENT_MOUSE_BUTTON_DOWN,17,2); selected_is(a,14,20);
    mouse_editing(a,SDL_EVENT_MOUSE_BUTTON_UP,17,2);
    mouse_editing(a,SDL_EVENT_MOUSE_BUTTON_DOWN,20,2); selected_is(a,20,24);
    mouse_editing(a,SDL_EVENT_MOUSE_BUTTON_UP,20,2);
    mouse_editing(a,SDL_EVENT_MOUSE_BUTTON_DOWN,5,3); selected_is(a,0,14);
    mouse_editing(a,SDL_EVENT_MOUSE_MOTION,27,0); selected_is(a,0,30);
    mouse_editing(a,SDL_EVENT_MOUSE_BUTTON_UP,27,3);
    assert(!ce_document_can_undo(a->doc) && !ce_document_is_modified(a->doc));
    reset_editing(a,"e\xcc\x81 👩‍💻 tail",CE_FORMAT_TEXT);
    mouse_editing(a,SDL_EVENT_MOUSE_BUTTON_DOWN,0,2); selected_is(a,0,3);
    mouse_editing(a,SDL_EVENT_MOUSE_BUTTON_UP,0,2);
    mouse_editing(a,SDL_EVENT_MOUSE_BUTTON_DOWN,4,2); selected_is(a,4,15);
    mouse_editing(a,SDL_EVENT_MOUSE_BUTTON_UP,4,2);
    mouse_editing_at(a,SDL_EVENT_MOUSE_BUTTON_DOWN,15,2,-1); selected_is(a,4,15);
    mouse_editing_at(a,SDL_EVENT_MOUSE_BUTTON_UP,15,2,-1);
    char wrapped[210]; memset(wrapped,'a',200); memcpy(wrapped+200,"\ntail",6);
    reset_editing(a,wrapped,CE_FORMAT_TEXT);
    mouse_editing(a,SDL_EVENT_MOUSE_BUTTON_DOWN,110,3); selected_is(a,0,201);
    mouse_editing(a,SDL_EVENT_MOUSE_BUTTON_UP,110,3);
}
static void test_clicked_link(App *a) {
    reset_editing(a,"<p><a href=\"https://click.test\">one two</a></p>",CE_FORMAT_HTML);
    assert(edit_alignment(a,0,0,"left")); /* Dirty document: autosave may run while waiting. */
    mouse_editing(a,SDL_EVENT_MOUSE_BUTTON_DOWN,1,1);
    mouse_editing(a,SDL_EVENT_MOUSE_BUTTON_UP,1,1);
    assert(a->link_click_pending && a->prompt_mode==PROMPT_NONE);
    assert(save_document(a) && a->link_click_pending);
    a->link_click_ready_at=SDL_GetTicks(); editing_tick(a);
    assert(a->prompt_mode==PROMPT_LINK && a->selection_start==0 && a->selection_end==7);
    assert(!strcmp(a->prompt->text->text,"https://click.test")); close_prompt(a);
    mouse_editing(a,SDL_EVENT_MOUSE_BUTTON_DOWN,1,1);
    mouse_editing(a,SDL_EVENT_MOUSE_BUTTON_UP,1,1);
    mouse_editing(a,SDL_EVENT_MOUSE_BUTTON_DOWN,1,2);
    mouse_editing(a,SDL_EVENT_MOUSE_BUTTON_UP,1,2);
    a->link_click_ready_at=SDL_GetTicks(); editing_tick(a);
    assert(a->prompt_mode==PROMPT_NONE && !a->link_click_pending); selected_is(a,0,3);
    mouse_editing(a,SDL_EVENT_MOUSE_BUTTON_DOWN,1,1);
    mouse_editing(a,SDL_EVENT_MOUSE_MOTION,5,0);
    mouse_editing(a,SDL_EVENT_MOUSE_BUTTON_UP,5,1);
    a->link_click_ready_at=SDL_GetTicks(); editing_tick(a);
    assert(a->prompt_mode==PROMPT_NONE && !a->link_click_pending);
}
static void *fail_json(size_t size) { (void)size; return NULL; }
static void test_failed_group(App *a) {
    reset_editing(a,"",CE_FORMAT_TEXT); input_editing(a,"a"); uint64_t revision=ce_document_revision(a->doc);
    cJSON_Hooks hooks={fail_json,free}; cJSON_InitHooks(&hooks); input_editing(a,"b"); cJSON_InitHooks(NULL);
    text_is(a,"a"); assert(ce_document_revision(a->doc)==revision);
    input_editing(a,"c"); action(a,ACT_UNDO); text_is(a,"a"); action(a,ACT_UNDO); text_is(a,"");
    assert(!ce_document_join_undo(a->doc,revision));
}
int main(int argc,char **argv) {
#ifdef _MSC_VER
    _CrtSetReportMode(_CRT_ASSERT,_CRTDBG_MODE_FILE);
    _CrtSetReportFile(_CRT_ASSERT,_CRTDBG_FILE_STDERR);
    _set_abort_behavior(0,_WRITE_ABORT_MSG|_CALL_REPORTFAULT);
#endif
    assert(argc>=3); void *state=NULL;
    char *args[]={"editing-test","--notes",argv[1],"--font",argv[2]};
    assert(SDL_AppInit(&state,5,args)==SDL_APP_CONTINUE); App *a=state;
    free(a->active); a->active=copy_string("editing.json"); assert(a->active);
    NoteResult created=note_store_create(&a->store,a->active,"",0);
    assert(created==NOTE_OK || created==NOTE_EXISTS);
    test_groups(a); test_links(a); test_mouse(a); test_clicked_link(a); test_failed_group(a);
    SDL_AppQuit(a,SDL_APP_SUCCESS); puts("editing history, links and mouse selections passed"); return 0;
}
