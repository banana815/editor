#define SDL_MAIN_HANDLED 1
#include "../src/app.c"
#undef main
#include <assert.h>
#ifdef _WIN32
#include <crtdbg.h>
#endif
static void screenshot(App *a,const char *directory,const char *name) {
    char path[1024]; SDL_snprintf(path,sizeof path,"%s/%s.bmp",directory,name);
    a->screenshot=path; a->frames=2; draw(a); a->screenshot=NULL;
    size_t length=0; void *data=SDL_LoadFile(path,&length); assert(data&&length>1000); SDL_free(data);
}
static void clear_records(NoteStore *store) {
    NoteList list={0}; assert(note_store_list(store,&list)==NOTE_OK);
    for(size_t i=0;i<list.count;i++) assert(note_store_delete(store,list.names[i])==NOTE_OK);
    note_store_list_free(&list);
}
static void finger(App *a,Uint32 type,float x,float y) {
    SDL_Event e={0};e.type=type;e.tfinger.windowID=SDL_GetWindowID(a->window);
    e.tfinger.touchID=42;e.tfinger.fingerID=1;e.tfinger.x=x/a->width;e.tfinger.y=y/a->height;
    assert(SDL_AppEvent(a,&e)==SDL_APP_CONTINUE);
}
int main(int argc,char **argv) {
#ifdef _WIN32
    _CrtSetReportMode(_CRT_ASSERT,_CRTDBG_MODE_FILE);_CrtSetReportFile(_CRT_ASSERT,_CRTDBG_FILE_STDERR);
#endif
    assert(argc>=3);void *state=NULL;char *args[]={"manager-test","--notes",argv[1],"--font",argv[2]};
    assert(SDL_AppInit(&state,5,args)==SDL_APP_CONTINUE);App *a=state;
    assert(manager_ready(a));clear_records(&a->writing_store.versions);clear_records(&a->writing_store.trash);
    const char *first="<p>first 中文</p>";note_store_delete(&a->store,"managed.json");note_store_delete(&a->store,"restored.json");
    assert(note_store_create(&a->store,"managed.json",first,strlen(first))==NOTE_OK);refresh_files(a);open_note(a,"managed.json");
    memset(&a->article_info,0,sizeof a->article_info);
    strcpy(a->article_info.folder,"博客");strcpy(a->article_info.tags,"草稿,写作");a->article_info.pinned=true;a->article_info.goal=100;
    assert(manager_save_info(a));manager_load_article(a);assert(a->article_info.pinned&&a->article_info.goal==100);
    manager_show(a,0);screenshot(a,argv[1],"article-management");manager_close(a);
    strcpy(a->folder_filter,"博客");manager_filter_files(a);assert(a->visible_count>=1&&!strcmp(a->files.names[a->visible_files[0]],"managed.json"));
    strcpy(a->folder_filter,"不存在");manager_filter_files(a);assert(a->visible_count==0);*a->folder_filter=0;manager_filter_files(a);
    assert(edit_replace(a,0,ce_document_length(a->doc),"second"));sync_view(a);mark_changed(a);assert(save_document(a));
    manager_show(a,1);assert(a->manager_open&&a->manager_archives.count);manager_select_archive(a,0);
    char *before=ce_document_export_json(a->doc);assert(before&&a->manager_preview_text&&a->manager_archive_data);
    draw(a);char *after=ce_document_export_json(a->doc);assert(after&&!strcmp(before,after));free(before);free(after);
    screenshot(a,argv[1],"article-history");
    manager_restore(a,NULL);assert(!a->manager_open);assert(ce_document_can_undo(a->doc));action(a,ACT_UNDO);assert(!strcmp(ce_document_text(a->doc),"second"));assert(save_document(a));
    action(a,ACT_DELETE);accept_prompt(a);assert(!a->active);char *bytes=NULL;size_t length=0;assert(note_store_read(&a->store,"managed.json",&bytes,&length)==NOTE_NOT_FOUND);
    manager_show(a,2);assert(a->manager_archives.count);size_t selected=0;for(;selected<a->manager_archives.count;selected++)if(!strcmp(a->manager_archives.entries[selected].name,"managed.json"))break;assert(selected<a->manager_archives.count);
    manager_select_archive(a,selected);manager_restore(a,"restored.json");assert(a->active&&!strcmp(a->active,"restored.json")&&!strcmp(ce_document_text(a->doc),"second"));
    assert(a->article_info.goal==100&&a->article_info.pinned);manager_show(a,0);manager_action(a,MAN_PARAGRAPH);assert(a->paragraph_focus);
    manager_action(a,MAN_STATUS);assert(a->article_info.status==1);manager_close(a);
    char summary[512];writing_summary(a,summary,sizeof summary);assert(strstr(summary,"目标 6/100"));
    /* Touch scrolling a compact manager must not activate the button beneath
     * the initial contact; a tap after scrolling still activates its row. */
    assert(SDL_SetWindowSize(a->window,390,400));layout(a);manager_show(a,0);manager_layout(a);
    SDL_FRect goal=a->manager_buttons[MAN_GOAL];
    finger(a,SDL_EVENT_FINGER_DOWN,goal.x+15,goal.y+20);
    finger(a,SDL_EVENT_FINGER_MOTION,goal.x+15,goal.y-90);
    finger(a,SDL_EVENT_FINGER_UP,goal.x+15,goal.y-90);
    assert(a->manager_scroll>50&&a->prompt_mode==PROMPT_NONE&&a->article_info.goal==100);
    manager_layout(a);SDL_FRect para=a->manager_buttons[MAN_PARAGRAPH];
    finger(a,SDL_EVENT_FINGER_DOWN,para.x+15,para.y+20);finger(a,SDL_EVENT_FINGER_UP,para.x+15,para.y+20);
    assert(!a->paragraph_focus);manager_close(a);
    assert(SDL_SetWindowSize(a->window,390,844));layout(a);manager_show(a,0);screenshot(a,argv[1],"article-management-phone");manager_close(a);
    assert(SDL_SetWindowSize(a->window,1100,760));layout(a);
    char error[256];const char *focus="<h1>把想法写成文章</h1><p>写作从一个简单的想法开始。</p><p>这一段是现在的焦点。把注意力放在文字上，其他段落会稍稍淡出。</p><p>留一点空间，让下一句话慢慢生长。</p>";
    assert(ce_document_import(a->doc,focus,CE_FORMAT_HTML,error,sizeof error));sync_view(a);
    a->edit->cursor=(int)(strstr(ce_document_text(a->doc),"这一段")-ce_document_text(a->doc));
    a->paragraph_focus=true;screenshot(a,argv[1],"article-paragraph-focus");
    SDL_AppQuit(a,SDL_APP_SUCCESS);puts("Article metadata, filters, history preview/undo, trash restore and goals passed");return 0;
}
