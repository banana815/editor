/* Linux system libcurl transport. No metadata parsing or application callbacks. */
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#include "platform_library_source_internal.h"
#include <SDL3/SDL.h>
#include <curl/curl.h>
#include <stdint.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>

#if LIBCURL_VERSION_NUM < 0x075400
#error "Library HTTP transport requires libcurl 7.84 or newer"
#endif

typedef struct {
    PlatformLibrarySource *job;
    SDL_AtomicInt canceled;
    CURL *easy; /* Accessed only by the worker. */
    bool response_seen;
    size_t header_bytes;
} CurlSource;
static SDL_SpinLock initialization_lock;
static bool initialized;
static bool initialize_curl(void) {
    SDL_LockSpinlock(&initialization_lock);
    if(!initialized && curl_global_init(CURL_GLOBAL_DEFAULT)==CURLE_OK) initialized=true;
    bool ok=initialized;
    SDL_UnlockSpinlock(&initialization_lock);
    /* Keep this module's initialization reference for the process lifetime.
     * Cleaning up here would race unrelated libcurl users/worker threads. */
    return ok;
}
static bool active(CurlSource *source) {
    return !SDL_GetAtomicInt(&source->canceled) && platform_library_source_active(source->job);
}
static bool response(CurlSource *source) {
    if(source->response_seen) return active(source);
    long status=0;
    if(curl_easy_getinfo(source->easy,CURLINFO_RESPONSE_CODE,&status)!=CURLE_OK || status<100 || status>599) {
        platform_library_source_error(source->job,"HTTP server returned no valid status");return false;
    }
    source->response_seen=true;
    return platform_library_source_response(source->job,(unsigned)status);
}
static size_t write_body(char *bytes,size_t size,size_t count,void *userdata) {
    CurlSource *source=userdata;
    if(size && count>SIZE_MAX/size) { platform_library_source_error(source->job,"HTTP chunk length overflow");return 0; }
    size_t length=size*count;
    return active(source) && response(source) && platform_library_source_append(source->job,bytes,length)?length:0;
}
static size_t read_header(char *bytes,size_t size,size_t count,void *userdata) {
    (void)bytes;CurlSource *source=userdata;
    if(size && count>SIZE_MAX/size) return 0;
    size_t length=size*count;
    if(length>65536-source->header_bytes) { platform_library_source_error(source->job,"HTTP headers exceed 64 KiB");return 0; }
    source->header_bytes+=length;return active(source)?length:0;
}
static int transfer_progress(void *userdata,curl_off_t total,curl_off_t current,curl_off_t upload_total,curl_off_t upload_current) {
    (void)total;(void)current;(void)upload_total;(void)upload_current;
    return active(userdata)?0:1;
}
static int SDLCALL curl_worker(void *userdata) {
    CurlSource *source=userdata;PlatformLibrarySource *job=source->job;
    /* NOSIGNAL does not promise suppression of TLS backend SIGPIPE. Block only
     * this worker's signal; never change the application's global handlers. */
    sigset_t blocked;sigemptyset(&blocked);sigaddset(&blocked,SIGPIPE);
    if(pthread_sigmask(SIG_BLOCK,&blocked,NULL)!=0) {
        platform_library_source_error(job,"Cannot configure HTTP worker signals");
        platform_library_source_backend_done(job);return 0;
    }
    CURL *easy=curl_easy_init();CURLM *multi=NULL;struct curl_slist *headers=NULL;
    CURLcode code=CURLE_OK;bool added=false,completed=false;char error[CURL_ERROR_SIZE]={0};
    source->easy=easy;
    if(!easy) { platform_library_source_error(job,"Cannot allocate HTTP request");goto done; }
#define SET(option,value) do { code=curl_easy_setopt(easy,option,value);if(code!=CURLE_OK)goto curl_error; } while(0)
    SET(CURLOPT_URL,platform_library_source_url(job));
    SET(CURLOPT_HTTPGET,1L);SET(CURLOPT_FOLLOWLOCATION,0L);SET(CURLOPT_MAXREDIRS,0L);
    SET(CURLOPT_PROXY,"");SET(CURLOPT_NETRC,(long)CURL_NETRC_IGNORED);
    SET(CURLOPT_HTTPAUTH,(long)CURLAUTH_NONE);SET(CURLOPT_PROXYAUTH,(long)CURLAUTH_NONE);
    SET(CURLOPT_SSL_VERIFYPEER,1L);SET(CURLOPT_SSL_VERIFYHOST,2L);
    SET(CURLOPT_NOSIGNAL,1L);
    SET(CURLOPT_TIMEOUT_MS,(long)platform_library_source_timeout(job));
    SET(CURLOPT_CONNECTTIMEOUT_MS,(long)platform_library_source_timeout(job));
#if LIBCURL_VERSION_NUM >= 0x075500
    SET(CURLOPT_PROTOCOLS_STR,"HTTP,HTTPS");SET(CURLOPT_REDIR_PROTOCOLS_STR,"HTTP,HTTPS");
#else
    SET(CURLOPT_PROTOCOLS,(long)(CURLPROTO_HTTP|CURLPROTO_HTTPS));
    SET(CURLOPT_REDIR_PROTOCOLS,(long)(CURLPROTO_HTTP|CURLPROTO_HTTPS));
#endif
    SET(CURLOPT_USERAGENT,"NoteLab/0.2");SET(CURLOPT_ACCEPT_ENCODING,"");
    SET(CURLOPT_WRITEFUNCTION,write_body);SET(CURLOPT_WRITEDATA,source);
    SET(CURLOPT_HEADERFUNCTION,read_header);SET(CURLOPT_HEADERDATA,source);
    SET(CURLOPT_XFERINFOFUNCTION,transfer_progress);SET(CURLOPT_XFERINFODATA,source);SET(CURLOPT_NOPROGRESS,0L);
    SET(CURLOPT_ERRORBUFFER,error);
    headers=curl_slist_append(NULL,"Accept: application/json, text/plain;q=0.9");
    if(!headers) { platform_library_source_error(job,"Cannot allocate HTTP headers");goto done; }
    SET(CURLOPT_HTTPHEADER,headers);
#undef SET
    multi=curl_multi_init();
    if(!multi || curl_multi_add_handle(multi,easy)!=CURLM_OK) {
        platform_library_source_error(job,"Cannot initialize HTTP transfer");goto done;
    }
    added=true;
    while(active(source)) {
        int running=0;
        CURLMcode result=curl_multi_perform(multi,&running);
        if(result!=CURLM_OK) { platform_library_source_error(job,curl_multi_strerror(result));goto done; }
        int remaining=0;CURLMsg *message;
        while((message=curl_multi_info_read(multi,&remaining))) if(message->msg==CURLMSG_DONE) {
            code=message->data.result;completed=true;
        }
        if(completed) break;
        if(!running) { platform_library_source_error(job,"HTTP transfer ended without completion");goto done; }
        result=curl_multi_poll(multi,NULL,0,100,NULL);
        if(result!=CURLM_OK) { platform_library_source_error(job,curl_multi_strerror(result));goto done; }
    }
    if(!active(source)) goto done;
    if(code!=CURLE_OK) goto curl_error;
    if(completed && response(source)) platform_library_source_success(job);
    goto done;
curl_error:
    if(active(source)) platform_library_source_error(job,*error?error:curl_easy_strerror(code));
done:
    if(added) curl_multi_remove_handle(multi,easy);
    if(multi) curl_multi_cleanup(multi);
    if(easy) curl_easy_cleanup(easy);
    curl_slist_free_all(headers);source->easy=NULL;
    platform_library_source_backend_done(job);
    return 0;
}
static bool curl_start(PlatformLibrarySource *job,void *userdata) {
    (void)userdata;
    if(!initialize_curl()) { platform_library_source_error(job,"Cannot initialize system libcurl");return false; }
    const curl_version_info_data *version=curl_version_info(CURLVERSION_NOW);
    /* Without an asynchronous resolver, NOSIGNAL cannot bound a DNS lookup. */
    if(!version || !(version->features&CURL_VERSION_ASYNCHDNS) || !(version->features&CURL_VERSION_THREADSAFE)) {
        platform_library_source_error(job,"System libcurl requires thread-safe initialization and asynchronous DNS");return false;
    }
    CurlSource *source=calloc(1,sizeof *source);
    if(!source) { platform_library_source_error(job,"Cannot allocate HTTP transport");return false; }
    source->job=job;platform_library_source_set_context(job,source);
    SDL_Thread *thread=SDL_CreateThread(curl_worker,"library-http",source);
    if(!thread) { platform_library_source_error(job,"Cannot start HTTP worker");return false; }
    SDL_DetachThread(thread);return true;
}
static void curl_cancel(PlatformLibrarySource *job) {
    CurlSource *source=platform_library_source_context(job);
    if(source) SDL_SetAtomicInt(&source->canceled,1);
}
static void curl_destroy(PlatformLibrarySource *job) { free(platform_library_source_context(job)); }
const PlatformLibrarySourceTransport platform_library_source_transport={curl_start,curl_cancel,curl_destroy};
