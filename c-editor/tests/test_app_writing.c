#define SDL_MAIN_HANDLED 1
#include "../src/app.c"
#undef main
#include <assert.h>

static void reset_writing(App *a,const char *source,CeFormat format) {
    char error[512]; assert(ce_document_import(a->doc,source,format,error,sizeof error));
    ce_citation_session_free(a->citation_session); a->citation_session=NULL;
    citation_refresh(a); a->typing_mask=0; a->prompt_mode=PROMPT_NONE;
    sync_view(a); a->edit->cursor=0; EditBox_SetFocus(a->edit,true); layout(a);
}
static void writing_input(App *a,const char *text) {
    SDL_Event event={0}; event.type=SDL_EVENT_TEXT_INPUT; event.text.text=text;
    assert(SDL_AppEvent(a,&event)==SDL_APP_CONTINUE);
}
static void writing_key(App *a,SDL_Keycode code,SDL_Keymod mod) {
    SDL_Event event={0}; event.type=SDL_EVENT_KEY_DOWN; event.key.key=code; event.key.mod=mod;
    assert(SDL_AppEvent(a,&event)==SDL_APP_CONTINUE);
}
static void expect_block(App *a,size_t offset,CeBlockKind kind,unsigned level) {
    CeBlockInfo block; assert(ce_document_block_at(a->doc,offset,&block));
    assert(block.kind==kind); if(kind==CE_BLOCK_HEADING) assert(block.level==level);
}
static void expect_snapshot(App *a,const char *before) {
    char *now=ce_document_export_json(a->doc); assert(now && !strcmp(now,before)); free(now);
}
static void test_heading_keys(App *a) {
    for(unsigned level=1;level<=6;++level) {
        reset_writing(a,"<p><strong>中文</strong> title</p><p>tail</p>",CE_FORMAT_HTML);
        a->edit->cursor=6;
        char *before=ce_document_export_json(a->doc); assert(before);
        writing_key(a,SDLK_0+level,(level%2 ? SDL_KMOD_CTRL : SDL_KMOD_GUI)|SDL_KMOD_ALT);
        expect_block(a,0,CE_BLOCK_HEADING,level);
        expect_block(a,13,CE_BLOCK_PARAGRAPH,0);
        assert(a->edit->cursor==6 && ce_document_style_at(a->doc,0).marks&CE_MARK_BOLD);
        assert(!strcmp(ce_document_text(a->doc),"中文 title\ntail"));
        action(a,ACT_UNDO); expect_snapshot(a,before); assert(!ce_document_can_undo(a->doc));
        action(a,ACT_REDO); expect_block(a,0,CE_BLOCK_HEADING,level);
        writing_key(a,SDLK_0,SDL_KMOD_CTRL|SDL_KMOD_ALT); expect_block(a,0,CE_BLOCK_PARAGRAPH,0);
        free(before);
    }
    reset_writing(a,"first\nsecond\nlast",CE_FORMAT_TEXT);
    a->edit->highlight1=0; a->edit->highlight2=a->edit->cursor=12;
    writing_key(a,SDLK_3,SDL_KMOD_CTRL|SDL_KMOD_ALT);
    expect_block(a,0,CE_BLOCK_HEADING,3); expect_block(a,6,CE_BLOCK_HEADING,3);
    expect_block(a,13,CE_BLOCK_PARAGRAPH,0);
    assert(a->edit->highlight1==0 && a->edit->highlight2==12 && a->edit->cursor==12);
}
static void test_markdown_headings(App *a) {
    for(unsigned level=1;level<=6;++level) {
        char prefix[7]; memset(prefix,'#',level); prefix[level]='\0';
        reset_writing(a,prefix,CE_FORMAT_TEXT); a->edit->cursor=(int)level;
        char *before=ce_document_export_json(a->doc); assert(before);
        writing_input(a," "); expect_block(a,0,CE_BLOCK_HEADING,level);
        assert(!*ce_document_text(a->doc) && a->edit->cursor==0);
        action(a,ACT_UNDO); expect_snapshot(a,before); assert(!ce_document_can_undo(a->doc));
        action(a,ACT_REDO); expect_block(a,0,CE_BLOCK_HEADING,level);
        writing_input(a,"文章🙂"); assert(!strcmp(ce_document_text(a->doc),"文章🙂"));
        expect_block(a,0,CE_BLOCK_HEADING,level); free(before);
    }
    reset_writing(a,"<p>intro</p><p>##<em>标题</em></p><p>tail</p>",CE_FORMAT_HTML);
    a->edit->cursor=8; writing_input(a," ");
    assert(!strcmp(ce_document_text(a->doc),"intro\n标题\ntail") && a->edit->cursor==6);
    expect_block(a,6,CE_BLOCK_HEADING,2);
    assert(ce_document_style_at(a->doc,6).marks&CE_MARK_ITALIC);
    const char *literal[]={"#######"," ##","x#"};
    for(size_t i=0;i<SDL_arraysize(literal);++i) {
        reset_writing(a,literal[i],CE_FORMAT_TEXT); a->edit->cursor=(int)strlen(literal[i]);
        writing_input(a," "); expect_block(a,0,CE_BLOCK_PARAGRAPH,0);
        assert(ce_document_length(a->doc)==strlen(literal[i])+1);
    }
    reset_writing(a,"",CE_FORMAT_TEXT); writing_input(a,"## pasted");
    expect_block(a,0,CE_BLOCK_PARAGRAPH,0); assert(!strcmp(ce_document_text(a->doc),"## pasted"));
    reset_writing(a,"<ul><li><p>##</p></li></ul>",CE_FORMAT_HTML);
    a->edit->cursor=2; writing_input(a," "); expect_block(a,0,CE_BLOCK_BULLET,0);
    assert(!strcmp(ce_document_text(a->doc),"## "));
    reset_writing(a,"##",CE_FORMAT_TEXT); a->edit->highlight1=0; a->edit->highlight2=a->edit->cursor=2;
    writing_input(a," "); expect_block(a,0,CE_BLOCK_PARAGRAPH,0);
    assert(!strcmp(ce_document_text(a->doc)," "));
    reset_writing(a,"##",CE_FORMAT_TEXT); a->edit->cursor=2;
    SDL_Event ime={0}; ime.type=SDL_EVENT_TEXT_EDITING; ime.edit.text="拼音"; ime.edit.length=2;
    assert(SDL_AppEvent(a,&ime)==SDL_APP_CONTINUE);
    writing_key(a,SDLK_1,SDL_KMOD_CTRL|SDL_KMOD_ALT);
    expect_block(a,0,CE_BLOCK_PARAGRAPH,0);
    writing_input(a," "); expect_block(a,0,CE_BLOCK_PARAGRAPH,0);
    assert(!strcmp(ce_document_text(a->doc),"## "));
}
static void test_list_toggle(App *a) {
    reset_writing(a,"<ul><li><p><strong>A</strong></p></li><li><p>B</p></li></ul>",CE_FORMAT_HTML);
    char *before=ce_document_export_json(a->doc); assert(before);
    action(a,ACT_BULLETS); expect_block(a,0,CE_BLOCK_PARAGRAPH,0); expect_block(a,2,CE_BLOCK_BULLET,0);
    assert(ce_document_style_at(a->doc,0).marks&CE_MARK_BOLD);
    action(a,ACT_UNDO); expect_snapshot(a,before); free(before);
    a->edit->highlight1=0; a->edit->highlight2=a->edit->cursor=3;
    action(a,ACT_BULLETS); expect_block(a,0,CE_BLOCK_PARAGRAPH,0); expect_block(a,2,CE_BLOCK_PARAGRAPH,0);
    assert(a->edit->highlight1==0 && a->edit->highlight2==3);
    action(a,ACT_NUMBERS); expect_block(a,0,CE_BLOCK_ORDERED,0); expect_block(a,2,CE_BLOCK_ORDERED,0);
    action(a,ACT_NUMBERS); expect_block(a,0,CE_BLOCK_PARAGRAPH,0); expect_block(a,2,CE_BLOCK_PARAGRAPH,0);
    reset_writing(a,"<ol><li><p>A</p></li><li><p>B</p></li></ol>",CE_FORMAT_HTML);
    action(a,ACT_BULLETS); expect_block(a,0,CE_BLOCK_BULLET,0); expect_block(a,2,CE_BLOCK_BULLET,0);
    reset_writing(a,"<p>A</p><ul><li><p>B</p></li></ul>",CE_FORMAT_HTML);
    a->edit->highlight1=0; a->edit->highlight2=a->edit->cursor=3;
    action(a,ACT_BULLETS); expect_block(a,0,CE_BLOCK_BULLET,0); expect_block(a,2,CE_BLOCK_BULLET,0);
}
static void *reject_json_allocation(size_t size) { (void)size; return NULL; }
static void test_failed_heading_preserves_document(App *a) {
    reset_writing(a,"##",CE_FORMAT_TEXT); a->edit->cursor=2;
    char *before=ce_document_export_json(a->doc); assert(before);
    cJSON_Hooks hooks={reject_json_allocation,free}; cJSON_InitHooks(&hooks);
    writing_input(a," ");
    cJSON_InitHooks(NULL);
    expect_snapshot(a,before); assert(a->edit->cursor==2 && !strcmp(a->edit->text->text,"##"));
    assert(!ce_document_can_undo(a->doc) && !ce_document_is_modified(a->doc));
    free(before);
}
static void test_longform_input(App *a) {
    const char *markers[]={"-","*","+",">","23.","4)","0."};
    for(size_t i=0;i<SDL_arraysize(markers);i++) {
        reset_writing(a,markers[i],CE_FORMAT_TEXT); a->edit->cursor=(int)strlen(markers[i]);
        char *before=ce_document_export_json(a->doc); assert(before);
        writing_input(a," "); expect_block(a,0,i<3?CE_BLOCK_BULLET:i==3?CE_BLOCK_QUOTE:CE_BLOCK_ORDERED,0);
        assert(!*ce_document_text(a->doc)); action(a,ACT_UNDO); expect_snapshot(a,before); free(before);
    }
    reset_writing(a,"-",CE_FORMAT_TEXT); a->edit->cursor=1; writing_input(a," "); writing_input(a,"A");
    writing_key(a,SDLK_RETURN,0); expect_block(a,2,CE_BLOCK_BULLET,0);
    writing_key(a,SDLK_RETURN,0); expect_block(a,2,CE_BLOCK_PARAGRAPH,0);
    reset_writing(a,"<p></p><p><a href=\"https://example.com\">Reference link</a></p>",CE_FORMAT_HTML);
    writing_input(a,"-"); writing_input(a," "); writing_input(a,"First item"); writing_key(a,SDLK_RETURN,0);
    writing_input(a,"Second item"); writing_key(a,SDLK_RETURN,0); writing_key(a,SDLK_RETURN,0);
    assert(a->edit->cursor==23); expect_block(a,23,CE_BLOCK_PARAGRAPH,0);
    writing_input(a,"```js"); writing_key(a,SDLK_RETURN,0); expect_block(a,23,CE_BLOCK_CODE,0);
    writing_input(a,"code"); assert(!strcmp(ce_document_text(a->doc),"First item\nSecond item\ncode\nReference link"));
    assert(!(ce_document_style_at(a->doc,23).marks&CE_MARK_LINK));
    assert(ce_document_style_at(a->doc,28).marks&CE_MARK_LINK);
    reset_writing(a,">",CE_FORMAT_TEXT); a->edit->cursor=1; writing_input(a," "); writing_input(a,"quote");
    writing_key(a,SDLK_RETURN,0); expect_block(a,6,CE_BLOCK_QUOTE,0);
    writing_key(a,SDLK_RETURN,0); expect_block(a,6,CE_BLOCK_PARAGRAPH,0);
    reset_writing(a,"```c",CE_FORMAT_TEXT); a->edit->cursor=4; writing_key(a,SDLK_RETURN,0);
    expect_block(a,0,CE_BLOCK_CODE,0); writing_input(a,"x"); writing_key(a,SDLK_RETURN,0); writing_input(a,"y");
    assert(!strcmp(ce_document_text(a->doc),"x\ny") && ce_document_block_count(a->doc)==1);
    writing_key(a,SDLK_RETURN,SDL_KMOD_CTRL); expect_block(a,4,CE_BLOCK_PARAGRAPH,0);
    reset_writing(a,"```",CE_FORMAT_TEXT); a->edit->cursor=3; writing_key(a,SDLK_RETURN,0);
    writing_input(a,"code"); writing_key(a,SDLK_RETURN,0); writing_input(a,"```");
    char *fenced=ce_document_export_json(a->doc); assert(fenced);
    writing_key(a,SDLK_RETURN,0); assert(!strcmp(ce_document_text(a->doc),"code\n"));
    expect_block(a,5,CE_BLOCK_PARAGRAPH,0); action(a,ACT_UNDO); expect_snapshot(a,fenced); free(fenced);
    reset_writing(a,"---",CE_FORMAT_TEXT); a->edit->cursor=3; char *before=ce_document_export_json(a->doc);
    writing_key(a,SDLK_RETURN,0); expect_block(a,0,CE_BLOCK_RULE,0); expect_block(a,4,CE_BLOCK_PARAGRAPH,0);
    assert(a->edit->cursor==4); action(a,ACT_UNDO); expect_snapshot(a,before); free(before);
    reset_writing(a,"",CE_FORMAT_TEXT); writing_input(a,"- pasted"); expect_block(a,0,CE_BLOCK_PARAGRAPH,0);
}
int main(int argc,char **argv) {
    assert(argc>=3); void *state=NULL;
    char *args[]={"writing-test","--notes",argv[1],"--font",argv[2]};
    assert(SDL_AppInit(&state,5,args)==SDL_APP_CONTINUE); App *a=state;
    free(a->active); a->active=copy_string("writing.json"); assert(a->active);
    test_heading_keys(a); test_markdown_headings(a); test_list_toggle(a);
    test_failed_heading_preserves_document(a); test_longform_input(a);
    SDL_AppQuit(a,SDL_APP_SUCCESS); puts("application writing commands passed"); return 0;
}
