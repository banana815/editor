#define SDL_MAIN_HANDLED 1
#include "../src/app.c"
#undef main
#include <assert.h>

static void press(App *a,SDL_Keycode key,SDL_Keymod mod) {
    SDL_Event event={0}; event.type=SDL_EVENT_KEY_DOWN;
    event.key.key=key; event.key.mod=mod;
    assert(SDL_AppEvent(a,&event)==SDL_APP_CONTINUE);
}
static void screenshot(App *a,const char *directory,const char *name) {
    char path[1024]; SDL_snprintf(path,sizeof path,"%s/%s.bmp",directory,name);
    a->screenshot=path; a->frames=2; draw(a); a->screenshot=NULL;
    size_t length=0; void *data=SDL_LoadFile(path,&length); assert(data && length>1000); SDL_free(data);
}
int main(int argc,char **argv) {
    assert(argc>=3); void *state=NULL;
    char *args[]={"workspace-test","--notes",argv[1],"--font",argv[2]};
    assert(SDL_AppInit(&state,5,args)==SDL_APP_CONTINUE); App *a=state;
    char error[256];
    const char *article="{\"type\":\"doc\",\"content\":["
        "{\"type\":\"heading\",\"attrs\":{\"level\":1},\"content\":[{\"type\":\"text\",\"text\":\"把想法写成文章\"}]},"
        "{\"type\":\"paragraph\",\"content\":[{\"type\":\"text\",\"text\":\"留一页安静的空间，让文字慢慢生长。Note Lab keeps your words at the center of the page.\"}]},"
        "{\"type\":\"heading\",\"attrs\":{\"level\":2},\"content\":[{\"type\":\"text\",\"text\":\"从一个想法开始\"}]},"
        "{\"type\":\"paragraph\",\"content\":[{\"type\":\"text\",\"text\":\"先写下来，再整理结构。标题会出现在左侧大纲中，随时可以跳回正在思考的段落。\"}]},"
        "{\"type\":\"heading\",\"attrs\":{\"level\":3},\"content\":[{\"type\":\"text\",\"text\":\"保持写作的节奏\"}]},"
        "{\"type\":\"paragraph\",\"content\":[{\"type\":\"text\",\"text\":\"进入专注模式，继续完成这一页。草稿会自动保存，格式工具在需要时展开。\"}]}]}";
    assert(ce_document_import(a->doc,article,CE_FORMAT_JSON,error,sizeof error));
    free(a->active); a->active=copy_string("写作练习.json"); assert(a->active);
    sync_view(a); layout(a);
    assert(!a->tools && !a->focus && a->editor_area.w<=760);
    char *before=ce_document_export_json(a->doc); assert(before);
    a->edit->cursor=9; a->edit->highlight1=0; a->edit->highlight2=9;
    char summary[256]; writing_summary(a,summary,sizeof summary);
    assert(a->writing_stats.characters==3 && strstr(summary,"选中"));
    press(a,SDLK_F,SDL_KMOD_CTRL|SDL_KMOD_SHIFT); layout(a);
    assert(a->focus && !a->library_button.w && !a->sidebar_area.w);
    assert(a->edit->cursor==9 && a->edit->highlight2==9);
    for(size_t i=0;i<SDL_arraysize(a->buttons);i++) assert(!a->buttons[i].box.w);
    press(a,SDLK_ESCAPE,0); layout(a); assert(!a->focus && a->sidebar);
    press(a,SDLK_T,SDL_KMOD_CTRL|SDL_KMOD_SHIFT); layout(a); assert(a->tools);
    press(a,SDLK_T,SDL_KMOD_CTRL|SDL_KMOD_SHIFT); layout(a); assert(!a->tools);
    action(a,ACT_OUTLINE); assert(outline_count(a)==3);
    outline_jump(a,1); CeBlockInfo block;
    assert(ce_document_block_at(a->doc,(size_t)a->edit->cursor,&block) && block.level==2);
    assert(a->edit->highlight1<0 && a->edit->has_focus);
    char *after=ce_document_export_json(a->doc); assert(after && !strcmp(before,after));
    assert(!ce_document_can_undo(a->doc) && !ce_document_is_modified(a->doc));
    free(before); free(after);
    screenshot(a,argv[1],"writing-workspace");
    action(a,ACT_FOCUS); screenshot(a,argv[1],"writing-focus"); action(a,ACT_FOCUS);
    assert(SDL_SetWindowSize(a->window,390,844)); layout(a);
    assert(a->focus_button.x>=0 && a->tools_button.x+a->tools_button.w<=a->width);
    outline_jump(a,2); assert(!a->sidebar); layout(a);
    assert(a->editor_area.x>=0 && a->editor_area.x+a->editor_area.w<=a->width);
    screenshot(a,argv[1],"writing-phone");
    action(a,ACT_FOCUS); press(a,SDLK_ESCAPE,0); assert(!a->focus && !a->sidebar);
    SDL_AppQuit(a,SDL_APP_SUCCESS);
    return 0;
}
