#define SDL_MAIN_HANDLED 1
#include "platform_library_source.h"
#include <SDL3/SDL.h>
#include <assert.h>
#include <stdio.h>
#include <string.h>
static const char expected[]="[{\"id\":\"local-test\",\"type\":\"book\",\"title\":\"中文🙂\"}]";
static PlatformLibrarySource *request(const char *base,const char *path,size_t limit,uint32_t timeout) {
    char url[1024],error[256];int n=snprintf(url,sizeof url,"%s%s",base,path);assert(n>0 && (size_t)n<sizeof url);
    PlatformLibrarySourceOptions options={limit,timeout};
    PlatformLibrarySource *job=platform_library_source_fetch(url,&options,error,sizeof error);
    if(!job) fprintf(stderr,"request: %s\n",error);
    assert(job);return job;
}
static PlatformLibrarySourceStatus wait_result(PlatformLibrarySource *job) {
    Uint64 until=SDL_GetTicks()+5000;PlatformLibrarySourceStatus status;
    while((status=platform_library_source_status(job,NULL))==PLATFORM_LIBRARY_SOURCE_PENDING && SDL_GetTicks()<until) SDL_Delay(5);
    assert(status!=PLATFORM_LIBRARY_SOURCE_PENDING);return status;
}
static void succeeds(const char *base,const char *path,const char *body) {
    PlatformLibrarySource *job=request(base,path,4096,3000);
    PlatformLibrarySourceStatus status=wait_result(job);
    if(status!=PLATFORM_LIBRARY_SOURCE_DONE){const char *message;platform_library_source_status(job,&message);fprintf(stderr,"%s: %s\n",path,message);}
    assert(status==PLATFORM_LIBRARY_SOURCE_DONE);PlatformLibrarySourceResult result={0};
    assert(platform_library_source_take_result(job,&result));assert(result.http_status==200);
    assert(result.size==strlen(body) && result.data && !memcmp(result.data,body,result.size+1));
    platform_library_source_free(job);assert(!strcmp((char *)result.data,body));platform_library_source_result_free(&result);
}
static void fails(const char *base,const char *path,size_t limit,uint32_t timeout) {
    PlatformLibrarySource *job=request(base,path,limit,timeout);assert(wait_result(job)==PLATFORM_LIBRARY_SOURCE_FAILED);
    PlatformLibrarySourceResult result={0};assert(!platform_library_source_take_result(job,&result));assert(!result.data && !result.size);
    platform_library_source_free(job);
}
int main(int argc,char **argv) {
    assert(argc==2 || argc==3);assert(SDL_Init(0));
    succeeds(argv[1],"/ok",expected);succeeds(argv[1],"/gzip",expected);succeeds(argv[1],"/chunked",expected);succeeds(argv[1],"/empty","");
    succeeds(argv[1],"/cookie",expected);succeeds(argv[1],"/ok",expected);
    if(argc==3) fails(argv[2],"/ok",4096,3000); /* Self-signed TLS is never trusted. */
    fails(argv[1],"/auth",4096,3000);fails(argv[1],"/redirect",4096,3000);fails(argv[1],"/missing",4096,3000);fails(argv[1],"/truncated",4096,3000);
    fails(argv[1],"/large",32,3000);fails(argv[1],"/gzip",32,3000);fails(argv[1],"/slow",4096,100);
    PlatformLibrarySource *job=request(argv[1],"/slow",4096,3000);SDL_Delay(20);
    Uint64 start=SDL_GetTicks();platform_library_source_cancel(job);assert(SDL_GetTicks()-start<100);
    assert(platform_library_source_status(job,NULL)==PLATFORM_LIBRARY_SOURCE_CANCELED);platform_library_source_free(job);
    job=request(argv[1],"/drip",4096,3000);SDL_Delay(30);platform_library_source_free(job);
    /* Any late transport completion has retained ownership independent of the
     * released caller. A fresh request verifies cancellation did not poison it. */
    succeeds(argv[1],"/ok",expected);SDL_Delay(350);
    SDL_Quit();puts("Native HTTP: plain/gzip/chunked bytes, no cookies/redirect/auth/proxy, errors, decoded cap, deadline, cancel and free with pending IO passed");return 0;
}
