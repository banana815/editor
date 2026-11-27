#define SDL_MAIN_HANDLED 1
#include "../src/app.c"
#undef main
#include <assert.h>
#ifdef _MSC_VER
#include <crtdbg.h>
#endif
#ifdef _WIN32
#include <process.h>
#define test_process_id _getpid
#else
#include <unistd.h>
#define test_process_id getpid
#endif

static void fixture(App *a,const char *name,const char *text) {
    NoteResult result=note_store_create(&a->store,name,text,strlen(text));
    if(result==NOTE_EXISTS) result=note_store_save(&a->store,name,text,strlen(text));
    if(result!=NOTE_OK) fprintf(stderr,"Search fixture %s failed (%d): %s\n",name,(int)result,a->store.error);
    assert(result==NOTE_OK);
}
static void prompt(App *a,const char *text) { EditBox_SetText(a->prompt,text); accept_prompt(a); }
static void finger(App *a,SDL_EventType type,float x,float y) {
    int width,height; float wx,wy; SDL_GetWindowSize(a->window,&width,&height);
    assert(SDL_RenderCoordinatesToWindow(a->renderer,x,y,&wx,&wy));
    SDL_Event event={0}; event.type=type; event.tfinger.windowID=SDL_GetWindowID(a->window);
    event.tfinger.touchID=1; event.tfinger.fingerID=1; event.tfinger.x=wx/width; event.tfinger.y=wy/height;
    assert(SDL_AppEvent(a,&event)==SDL_APP_CONTINUE);
}
int main(int argc,char **argv) {
#ifdef _MSC_VER
    _CrtSetReportMode(_CRT_ASSERT,_CRTDBG_MODE_FILE);
    _CrtSetReportFile(_CRT_ASSERT,_CRTDBG_FILE_STDERR);
#endif
    assert(argc>=3); void *state=NULL;
    /* Never reopen another invocation's fixtures or recovery sidecars. Keep
     * each run under the caller's isolated, ignored test root for diagnosis. */
    char *notes=NULL;
    assert(SDL_asprintf(&notes,"%s/run-%ld-%llu",argv[1],(long)test_process_id(),
        (unsigned long long)SDL_GetPerformanceCounter())>0);
    char *args[]={"search-test","--notes",notes,"--font",argv[2]};
    assert(SDL_AppInit(&state,5,args)==SDL_APP_CONTINUE); App *a=state;
    fixture(a,"active.json","{\"type\":\"doc\",\"content\":[{\"type\":\"paragraph\",\"content\":[{\"type\":\"text\",\"text\":\"saved text\"}]}]}");
    fixture(a,"other.json","{\"type\":\"doc\",\"content\":[{\"type\":\"paragraph\",\"content\":[{\"type\":\"text\",\"text\":\"Other needle 中文\"}]}]}");
    fixture(a,"broken.json","{broken");
    open_note(a,"active.json"); assert(a->active && !strcmp(a->active,"active.json"));
    assert(edit_replace(a,0,ce_document_length(a->doc),"Unsaved needle needle 中文")); sync_view(a); mark_changed(a);
    char *before=ce_document_export_json(a->doc); assert(before);
    char *saved=NULL; size_t length; assert(note_store_read(&a->store,"active.json",&saved,&length)==NOTE_OK);
    search_begin_all(a); prompt(a,"needle");
    assert(a->search_open && a->prompt_mode==PROMPT_NONE && a->search_count==2 && a->search_skipped==1);
    assert(strstr(a->search_hits[0].snippet,"Unsaved"));
    char *after=ce_document_export_json(a->doc); assert(after && !strcmp(before,after)); free(after);
    char *still_saved=NULL; size_t still_length;
    assert(note_store_read(&a->store,"active.json",&still_saved,&still_length)==NOTE_OK && !strcmp(saved,still_saved));
    note_store_free(saved); note_store_free(still_saved); free(before);
    search_open_hit(a,0); assert(!a->search_open && a->edit->highlight2>a->edit->highlight1);
    assert((size_t)(a->edit->highlight2-a->edit->highlight1)==strlen("needle"));
    search_begin_replace(a,false); prompt(a,"needle");
    assert(a->prompt_mode==PROMPT_REPLACE_VALUE); prompt(a,"found");
    assert(a->prompt_mode==PROMPT_NONE && !strcmp(ce_document_text(a->doc),"Unsaved found needle 中文"));
    action(a,ACT_UNDO); assert(!strcmp(ce_document_text(a->doc),"Unsaved needle needle 中文"));
    search_begin_replace(a,true); prompt(a,"needle"); prompt(a,"needle needle");
    assert(!strcmp(ce_document_text(a->doc),"Unsaved needle needle needle needle 中文"));
    action(a,ACT_UNDO); assert(!strcmp(ce_document_text(a->doc),"Unsaved needle needle 中文"));
    assert(search_collect(a,"needle")); search_open_hit(a,1);
    assert(!a->search_open && !strcmp(a->active,"other.json"));
    assert(a->edit->highlight1==6 && a->edit->highlight2==12 && a->edit->has_focus);
    /* Empty queries stay in the prompt; cancellation never mutates text. */
    before=ce_document_export_json(a->doc);
    search_begin_replace(a,true); prompt(a,""); assert(a->prompt_mode==PROMPT_REPLACE_QUERY);
    close_prompt(a); after=ce_document_export_json(a->doc); assert(!strcmp(before,after)); free(before); free(after);
    /* A touch drag scrolls results without opening the row pressed initially;
     * a subsequent tap opens a result and highlights its current match. */
    for(unsigned i=0;i<20;i++) { char name[64]; SDL_snprintf(name,sizeof name,"result-%02u.txt",i); fixture(a,name,"needle result"); }
    assert(search_collect(a,"needle") && a->search_count==22); search_layout(a);
    float x=a->search_area.x+40,y=a->search_area.y+300;
    finger(a,SDL_EVENT_FINGER_DOWN,x,y); finger(a,SDL_EVENT_FINGER_MOTION,x,y-120); finger(a,SDL_EVENT_FINGER_UP,x,y-120);
    assert(a->search_open && a->search_scroll>100 && !strcmp(a->active,"other.json"));
    y=a->search_area.y+110;
    finger(a,SDL_EVENT_FINGER_DOWN,x,y); finger(a,SDL_EVENT_FINGER_UP,x,y);
    assert(!a->search_open && a->edit->highlight2>a->edit->highlight1 && a->edit->has_focus);
    SDL_AppQuit(a,SDL_APP_SUCCESS); SDL_free(notes); return 0;
}
