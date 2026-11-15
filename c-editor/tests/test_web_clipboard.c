/* Production bridge ownership exercised in WASM, with only the SDL event
 * queue/window calls stubbed. Browser API promises are controlled by Node. */
#include "platform_web_input.h"
#include <emscripten.h>
#include <assert.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
static int completed,successful,failed,finished,accepted,pasted;
static char last_plain[128],last_html[128],last_typed[128];
Uint32 SDL_RegisterEvents(int count) { (void)count;return SDL_EVENT_USER; }
int SDL_PeepEvents(SDL_Event *events,int num,SDL_EventAction action,Uint32 min,Uint32 max) { (void)events;(void)num;(void)action;(void)min;(void)max;return 0; }
bool SDL_PushEvent(SDL_Event *event) { (void)event;return true; }
void SDL_FlushEvent(Uint32 type) { (void)type; }
SDL_WindowID SDL_GetWindowID(SDL_Window *window) { (void)window;return 1; }
size_t SDL_utf8strlen(const char *text) { return strlen(text); }
static void completion(bool success,const char *plain,const char *html,const char *typed,const char *error,void *userdata) {
    (void)error;assert(userdata==(void *)17);++completed;
    if(success)++successful;else ++failed;
    snprintf(last_plain,sizeof last_plain,"%s",plain?plain:"");
    snprintf(last_html,sizeof last_html,"%s",html?html:"");
    snprintf(last_typed,sizeof last_typed,"%s",typed?typed:"");
}
static bool prepare(bool cut,PlatformWebClipboardData *out,void *state) {
    assert(state==(void *)23);(void)cut;*out=(PlatformWebClipboardData){"中文🙂","<b>中文🙂</b>","{\"version\":1}"};return true;
}
static void finish(bool success,void *state) { assert(state==(void *)23);++finished;if(success)++accepted; }
static bool paste(const PlatformWebClipboardData *data,void *state) {
    assert(state==(void *)23);assert(data && data->plain);++pasted;return true;
}
EMSCRIPTEN_KEEPALIVE void test_init(void) {
    platform_web_input_init();platform_web_input_clipboard_rich(prepare,finish,paste,(void *)23);
}
EMSCRIPTEN_KEEPALIVE int test_start(int write) {
    char plain[]="copied before return",html[]="<b>copied</b>",typed[]="{\"version\":1}";
    bool ok=write?platform_web_clipboard_write(plain,html,typed,completion,(void *)17):platform_web_clipboard_read(completion,(void *)17);
    memset(plain,'x',sizeof plain-1);memset(html,'x',sizeof html-1);memset(typed,'x',sizeof typed-1);
    return ok;
}
EMSCRIPTEN_KEEPALIVE void test_destroy(void) { platform_web_input_destroy(); }
EMSCRIPTEN_KEEPALIVE int test_count(int kind) { return kind==0?completed:kind==1?successful:kind==2?failed:kind==3?finished:kind==4?accepted:pasted; }
EMSCRIPTEN_KEEPALIVE const char *test_text(int kind) { return kind==0?last_plain:kind==1?last_html:last_typed; }
