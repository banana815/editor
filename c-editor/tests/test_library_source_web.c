#define SDL_MAIN_HANDLED 1
#include "platform_library_source.h"
#include <SDL3/SDL.h>
#include <emscripten.h>
static PlatformLibrarySourceResult result;
EMSCRIPTEN_KEEPALIVE int library_test_init(void) { return SDL_Init(0); }
EMSCRIPTEN_KEEPALIVE PlatformLibrarySource *library_test_start(const char *url,unsigned limit,unsigned timeout) {
    PlatformLibrarySourceOptions options={(size_t)limit,timeout};
    return platform_library_source_fetch(url,&options,NULL,0);
}
EMSCRIPTEN_KEEPALIVE int library_test_status(PlatformLibrarySource *job) { return platform_library_source_status(job,NULL); }
EMSCRIPTEN_KEEPALIVE const char *library_test_message(PlatformLibrarySource *job) {
    const char *message=NULL;platform_library_source_status(job,&message);return message;
}
EMSCRIPTEN_KEEPALIVE int library_test_take(PlatformLibrarySource *job) {
    platform_library_source_result_free(&result);return platform_library_source_take_result(job,&result);
}
EMSCRIPTEN_KEEPALIVE const unsigned char *library_test_bytes(void) { return result.data; }
EMSCRIPTEN_KEEPALIVE size_t library_test_size(void) { return result.size; }
EMSCRIPTEN_KEEPALIVE void library_test_clear(void) { platform_library_source_result_free(&result); }
EMSCRIPTEN_KEEPALIVE void library_test_cancel(PlatformLibrarySource *job) { platform_library_source_cancel(job); }
EMSCRIPTEN_KEEPALIVE void library_test_free(PlatformLibrarySource *job) { platform_library_source_free(job); }
