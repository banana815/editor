#define SDL_MAIN_HANDLED 1
#include <SDL3/SDL.h>
static Uint64 test_time=1000;
static Uint64 app_test_ticks(void) { return test_time; }
static int published_input_target;
static void app_test_web_input_update(int target,SDL_Window *window,const SDL_FRect *area) {
    (void)window;(void)area;published_input_target=target;
}
#define SDL_GetTicks app_test_ticks
#define platform_web_input_update app_test_web_input_update
#include "../src/app.c"
#undef platform_web_input_update
#undef SDL_GetTicks
#undef main
#include <assert.h>

static void finger(App *a,SDL_EventType type,SDL_FingerID id,float x,float y,Uint64 elapsed) {
    test_time+=elapsed;
    int width,height; float wx,wy; SDL_GetWindowSize(a->window,&width,&height);
    assert(SDL_RenderCoordinatesToWindow(a->renderer,x,y,&wx,&wy));
    SDL_Event e={0};e.type=type;e.tfinger.timestamp=test_time*1000000;
    e.tfinger.windowID=SDL_GetWindowID(a->window);e.tfinger.touchID=1;e.tfinger.fingerID=id;
    e.tfinger.x=wx/width;e.tfinger.y=wy/height;
    assert(SDL_AppEvent(a,&e)==SDL_APP_CONTINUE);
}
static void mouse(App *a,SDL_EventType type,SDL_MouseID which,float x,float y) {
    float wx,wy;assert(SDL_RenderCoordinatesToWindow(a->renderer,x,y,&wx,&wy));
    SDL_Event e={0};e.type=type;
    if(type==SDL_EVENT_MOUSE_MOTION) { e.motion.windowID=SDL_GetWindowID(a->window);e.motion.which=which;e.motion.x=wx;e.motion.y=wy;e.motion.state=SDL_BUTTON_LMASK; }
    else { e.button.windowID=SDL_GetWindowID(a->window);e.button.which=which;e.button.button=SDL_BUTTON_LEFT;e.button.x=wx;e.button.y=wy; }
    assert(SDL_AppEvent(a,&e)==SDL_APP_CONTINUE);
}
int main(int argc,char **argv) {
    assert(argc>=3); void *state=NULL;
    char *args[]={"touch-test","--notes",argv[1],"--font",argv[2]};
    assert(SDL_AppInit(&state,5,args)==SDL_APP_CONTINUE);App *a=state;
    char contents[10000];size_t used=0;
    for(size_t i=0;i<80;i++) { int n=snprintf(contents+used,sizeof contents-used,"Line %zu 中文🙂 selectable text\n",i);assert(n>0 && (size_t)n<sizeof contents-used);used+=(size_t)n; }
    char error[256];assert(ce_document_import(a->doc,contents,CE_FORMAT_TEXT,error,sizeof error));
    free(a->active);a->active=copy_string("touch.json");assert(a->active);sync_view(a);layout(a);
    EditBox_SetFocus(a->edit,false);a->edit->cursor=5;a->scroll=100;layout(a);
    char *before=ce_document_export_json(a->doc);assert(before);
    float x=a->editor_area.x+60,y=a->editor_area.y+130;
    finger(a,SDL_EVENT_FINGER_DOWN,1,x,y,10);
    assert(a->touch.phase==CE_TOUCH_PENDING && a->edit->cursor==5 && !a->edit->has_focus);
    finger(a,SDL_EVENT_FINGER_MOTION,1,x,y-50,20);assert(a->scroll==150);
    finger(a,SDL_EVENT_FINGER_UP,1,x,y-50,20);
    assert(a->edit->cursor==5 && a->edit->highlight2<0 && !a->edit->has_focus && !a->touch_menu_visible);
    assert(!ce_document_can_undo(a->doc) && !ce_document_is_modified(a->doc));
    mouse(a,SDL_EVENT_MOUSE_BUTTON_DOWN,SDL_TOUCH_MOUSEID,x,y);
    mouse(a,SDL_EVENT_MOUSE_MOTION,SDL_TOUCH_MOUSEID,x+80,y+40);
    mouse(a,SDL_EVENT_MOUSE_BUTTON_UP,SDL_TOUCH_MOUSEID,x+80,y+40);
    assert(a->edit->cursor==5 && a->edit->highlight2<0 && a->scroll==150);
    finger(a,SDL_EVENT_FINGER_DOWN,1,x,y,20);finger(a,SDL_EVENT_FINGER_UP,1,x,y,60);
    assert(a->edit->has_focus && a->touch_menu_visible && !a->edit->highlighting);
    assert(published_input_target==1); /* Published before the tap handler returns. */
    EditBox_SetFocus(a->edit,false);a->touch_menu_visible=false;a->scroll=0;layout(a);
    finger(a,SDL_EVENT_FINGER_DOWN,1,x,y,20);test_time+=500;touch_tick(a);
    assert(a->touch.phase==CE_TOUCH_SELECTING && a->edit->highlight2>a->edit->highlight1 && !a->edit->has_focus);
    int start=a->edit->highlight1;
    finger(a,SDL_EVENT_FINGER_MOTION,1,x+120,y+40,20);
    assert(a->edit->highlight1==start && a->edit->highlight2>start);
    finger(a,SDL_EVENT_FINGER_UP,1,x+120,y+40,20);assert(a->touch_menu_visible && !a->edit->has_focus);
    layout(a);SDL_FRect long_select=a->touch_menu_buttons[CE_EDITOR_SELECT_ALL];
    finger(a,SDL_EVENT_FINGER_DOWN,1,long_select.x+5,long_select.y+5,20);
    finger(a,SDL_EVENT_FINGER_UP,1,long_select.x+5,long_select.y+5,20);
    assert(!a->edit->has_focus && a->edit->highlight1==0 && (size_t)a->edit->highlight2==ce_document_length(a->doc));
    SDL_FRect copy=a->touch_menu_buttons[CE_EDITOR_COPY];
    finger(a,SDL_EVENT_FINGER_DOWN,1,copy.x+5,copy.y+5,20);
    finger(a,SDL_EVENT_FINGER_UP,1,copy.x+5,copy.y+5,20);
    assert(!a->edit->has_focus && SDL_HasClipboardData(CE_DOCUMENT_SLICE_MIME));
    int previous=a->edit->cursor;
    finger(a,SDL_EVENT_FINGER_DOWN,1,x,y,20);finger(a,SDL_EVENT_FINGER_DOWN,2,x+30,y,20);
    assert(a->touch.phase==CE_TOUCH_BLOCKED);
    finger(a,SDL_EVENT_FINGER_UP,1,x,y,20);finger(a,SDL_EVENT_FINGER_UP,2,x+30,y,20);
    assert(a->touch.phase==CE_TOUCH_IDLE && a->edit->cursor==previous && !a->edit->has_focus);
    finger(a,SDL_EVENT_FINGER_DOWN,1,x,y,20);finger(a,SDL_EVENT_FINGER_CANCELED,1,x,y,20);
    assert(a->edit->cursor==previous && !a->edit->has_focus);
    mouse(a,SDL_EVENT_MOUSE_BUTTON_DOWN,SDL_PEN_MOUSEID,x,y);
    mouse(a,SDL_EVENT_MOUSE_MOTION,SDL_PEN_MOUSEID,x+100,y+40);
    mouse(a,SDL_EVENT_MOUSE_BUTTON_UP,SDL_PEN_MOUSEID,x+100,y+40);
    assert(a->edit->has_focus && a->edit->highlight2>=0 && !a->edit->highlighting);
    a->touch_menu_visible=true;layout(a);SDL_FRect select=a->touch_menu_buttons[CE_EDITOR_SELECT_ALL];
    previous=a->edit->cursor; int selection_start=a->edit->highlight1,selection_end=a->edit->highlight2;
    finger(a,SDL_EVENT_FINGER_DOWN,1,select.x+5,select.y+5,20);
    finger(a,SDL_EVENT_FINGER_MOTION,1,select.x+5,select.y-40,20);
    finger(a,SDL_EVENT_FINGER_UP,1,select.x+5,select.y+5,20);
    assert(a->edit->cursor==previous && a->edit->highlight1==selection_start && a->edit->highlight2==selection_end);
    finger(a,SDL_EVENT_FINGER_DOWN,1,select.x+5,select.y+5,20);
    finger(a,SDL_EVENT_FINGER_UP,1,select.x+5,select.y+5,20);
    assert(a->edit->highlight1==0 && (size_t)a->edit->highlight2==ce_document_length(a->doc));
    /* A visible menu belongs to its original note revision, even after the
     * finger has lifted. A keyboard-driven view change must cancel it too. */
    assert(a->touch_menu_visible && !a->touch.contact_count);
    ++a->citation_revision;touch_tick(a);assert(!a->touch_menu_visible);
    a->touch_menu_visible=true;a->touch_revision=a->citation_revision;
    a->prompt_mode=PROMPT_NEW;touch_tick(a);assert(!a->touch_menu_visible);a->prompt_mode=PROMPT_NONE;
    char *after=ce_document_export_json(a->doc);assert(after && !strcmp(before,after));free(before);free(after);
    SDL_AppQuit(a,SDL_APP_SUCCESS);
    puts("app touch: scroll without selection/focus, tap, long press, cancel/multiple fingers, synthetic mouse suppression and pen passed");
    return 0;
}
