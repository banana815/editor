/* Windows system HTTP/TLS transport. A worker serializes asynchronous WinHTTP
 * calls; callbacks only publish completion records. No application callbacks. */
#include "platform_library_source_internal.h"
#include <SDL3/SDL.h>
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <winhttp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    PlatformLibrarySource *job;
    SDL_AtomicInt canceled;
    SDL_SpinLock lock;
    HANDLE ready,closed;
    DWORD notification,error,received;
    unsigned char bytes[32768];
} WinSource;
static bool active(WinSource *source) {
    return !SDL_GetAtomicInt(&source->canceled) && platform_library_source_active(source->job);
}
static void win_error(PlatformLibrarySource *job,DWORD code) {
    char message[96];snprintf(message,sizeof message,"Windows HTTP request failed (error %lu)",(unsigned long)code);
    platform_library_source_error(job,message);
}
static void CALLBACK notify(HINTERNET handle,DWORD_PTR context,DWORD status,void *information,DWORD size) {
    (void)handle;WinSource *source=(WinSource *)context;
    if(!source) return;
    if(status==WINHTTP_CALLBACK_STATUS_HANDLE_CLOSING) {
        /* This is the last callback for the request. No source access follows
         * SetEvent: the worker may wake and release the context immediately. */
        SetEvent(source->closed);return;
    }
    if(status!=WINHTTP_CALLBACK_STATUS_SENDREQUEST_COMPLETE &&
       status!=WINHTTP_CALLBACK_STATUS_HEADERS_AVAILABLE &&
       status!=WINHTTP_CALLBACK_STATUS_READ_COMPLETE &&
       status!=WINHTTP_CALLBACK_STATUS_REQUEST_ERROR) return;
    SDL_LockSpinlock(&source->lock);
    source->notification=status;source->received=status==WINHTTP_CALLBACK_STATUS_READ_COMPLETE?size:0;
    if(status==WINHTTP_CALLBACK_STATUS_REQUEST_ERROR) {
        source->error=information && size>=sizeof(WINHTTP_ASYNC_RESULT)?
            ((WINHTTP_ASYNC_RESULT *)information)->dwError:ERROR_WINHTTP_INTERNAL_ERROR;
    }
    SDL_UnlockSpinlock(&source->lock);
    SetEvent(source->ready);
}
static bool wait_for(WinSource *source,DWORD expected) {
    while(active(source)) {
        DWORD result=WaitForSingleObject(source->ready,50);
        if(result==WAIT_TIMEOUT) continue;
        if(result!=WAIT_OBJECT_0) { win_error(source->job,GetLastError());return false; }
        SDL_LockSpinlock(&source->lock);
        DWORD status=source->notification,error=source->error;
        SDL_UnlockSpinlock(&source->lock);
        if(status==WINHTTP_CALLBACK_STATUS_REQUEST_ERROR) { win_error(source->job,error);return false; }
        if(status!=expected) { platform_library_source_error(source->job,"Unexpected Windows HTTP completion");return false; }
        return active(source);
    }
    return false;
}
static WCHAR *wide_url(const char *value) {
    int length=MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,value,-1,NULL,0);
    if(length<=0 || (size_t)length>SIZE_MAX/sizeof(WCHAR)) return NULL;
    WCHAR *result=malloc((size_t)length*sizeof *result);
    if(result && !MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,value,-1,result,length)) { free(result);result=NULL; }
    return result;
}
static int SDLCALL win_worker(void *userdata) {
    WinSource *source=userdata;PlatformLibrarySource *job=source->job;
    HINTERNET session=NULL,connection=NULL,request=NULL;
    WCHAR *url=NULL,*host=NULL,*path=NULL;bool callback_ready=false,success=false;
    URL_COMPONENTS components;memset(&components,0,sizeof components);components.dwStructSize=sizeof components;
    components.dwHostNameLength=(DWORD)-1;components.dwUrlPathLength=(DWORD)-1;components.dwExtraInfoLength=(DWORD)-1;
    if(!active(source)) goto done;
    url=wide_url(platform_library_source_url(job));
    if(!url) { platform_library_source_error(job,"Cannot allocate HTTP URL");goto done; }
    if(!WinHttpCrackUrl(url,0,0,&components)) { win_error(job,GetLastError());goto done; }
    if(components.nScheme!=INTERNET_SCHEME_HTTP && components.nScheme!=INTERNET_SCHEME_HTTPS) {
        platform_library_source_error(job,"Unsupported HTTP URL scheme");goto done;
    }
    size_t host_length=components.dwHostNameLength,path_length=(size_t)components.dwUrlPathLength+components.dwExtraInfoLength;
    if(host_length>PLATFORM_LIBRARY_SOURCE_MAX_URL || path_length>PLATFORM_LIBRARY_SOURCE_MAX_URL) {
        platform_library_source_error(job,"HTTP URL is too long");goto done;
    }
    host=malloc((host_length+1)*sizeof *host);path=malloc((path_length+2)*sizeof *path);
    if(!host || !path) { platform_library_source_error(job,"Cannot allocate HTTP URL components");goto done; }
    memcpy(host,components.lpszHostName,host_length*sizeof *host);host[host_length]=0;
    size_t at=0;
    if(components.dwUrlPathLength) { memcpy(path,components.lpszUrlPath,(size_t)components.dwUrlPathLength*sizeof *path);at=components.dwUrlPathLength; }
    else path[at++]=L'/';
    if(components.dwExtraInfoLength) { memcpy(path+at,components.lpszExtraInfo,(size_t)components.dwExtraInfoLength*sizeof *path);at+=components.dwExtraInfoLength; }
    path[at]=0;
    session=WinHttpOpen(L"NoteLab/0.2",WINHTTP_ACCESS_TYPE_NO_PROXY,WINHTTP_NO_PROXY_NAME,WINHTTP_NO_PROXY_BYPASS,WINHTTP_FLAG_ASYNC);
    if(!session) { win_error(job,GetLastError());goto done; }
    int timeout=(int)platform_library_source_timeout(job);
    if(!WinHttpSetTimeouts(session,timeout,timeout,timeout,timeout)) { win_error(job,GetLastError());goto done; }
    connection=WinHttpConnect(session,host,components.nPort,0);
    if(!connection) { win_error(job,GetLastError());goto done; }
    request=WinHttpOpenRequest(connection,L"GET",path,NULL,WINHTTP_NO_REFERER,WINHTTP_DEFAULT_ACCEPT_TYPES,
        components.nScheme==INTERNET_SCHEME_HTTPS?WINHTTP_FLAG_SECURE:0);
    if(!request) { win_error(job,GetLastError());goto done; }
    DWORD disable=WINHTTP_DISABLE_COOKIES|WINHTTP_DISABLE_REDIRECTS|WINHTTP_DISABLE_AUTHENTICATION;
    DWORD autologon=WINHTTP_AUTOLOGON_SECURITY_LEVEL_HIGH;
    DWORD decompress=WINHTTP_DECOMPRESSION_FLAG_GZIP|WINHTTP_DECOMPRESSION_FLAG_DEFLATE;
    DWORD max_headers=65536;
    DWORD_PTR context=(DWORD_PTR)source;
    if(!WinHttpSetOption(request,WINHTTP_OPTION_DISABLE_FEATURE,&disable,sizeof disable) ||
       !WinHttpSetOption(request,WINHTTP_OPTION_AUTOLOGON_POLICY,&autologon,sizeof autologon) ||
       !WinHttpSetOption(request,WINHTTP_OPTION_DECOMPRESSION,&decompress,sizeof decompress) ||
       !WinHttpSetOption(request,WINHTTP_OPTION_MAX_RESPONSE_HEADER_SIZE,&max_headers,sizeof max_headers) ||
       !WinHttpSetOption(request,WINHTTP_OPTION_CONTEXT_VALUE,&context,sizeof context)) { win_error(job,GetLastError());goto done; }
    if(WinHttpSetStatusCallback(request,notify,WINHTTP_CALLBACK_FLAG_ALL_COMPLETIONS|WINHTTP_CALLBACK_FLAG_HANDLES,0)==WINHTTP_INVALID_STATUS_CALLBACK) {
        win_error(job,GetLastError());goto done;
    }
    callback_ready=true;
    if(!active(source)) goto done;
    if(!WinHttpSendRequest(request,L"Accept: application/json, text/plain;q=0.9\r\n",(DWORD)-1,
        WINHTTP_NO_REQUEST_DATA,0,0,context)) { win_error(job,GetLastError());goto done; }
    if(!wait_for(source,WINHTTP_CALLBACK_STATUS_SENDREQUEST_COMPLETE)) goto done;
    if(!WinHttpReceiveResponse(request,NULL)) { win_error(job,GetLastError());goto done; }
    if(!wait_for(source,WINHTTP_CALLBACK_STATUS_HEADERS_AVAILABLE)) goto done;
    DWORD status=0,length=sizeof status;
    if(!WinHttpQueryHeaders(request,WINHTTP_QUERY_STATUS_CODE|WINHTTP_QUERY_FLAG_NUMBER,
        WINHTTP_HEADER_NAME_BY_INDEX,&status,&length,WINHTTP_NO_HEADER_INDEX)) { win_error(job,GetLastError());goto done; }
    if(!platform_library_source_response(job,(unsigned)status)) goto done;
    while(active(source)) {
        if(!WinHttpReadData(request,source->bytes,sizeof source->bytes,NULL)) { win_error(job,GetLastError());goto done; }
        if(!wait_for(source,WINHTTP_CALLBACK_STATUS_READ_COMPLETE)) goto done;
        SDL_LockSpinlock(&source->lock);DWORD received=source->received;SDL_UnlockSpinlock(&source->lock);
        if(!received) { success=true;break; }
        if(received>sizeof source->bytes || !platform_library_source_append(job,source->bytes,received)) goto done;
    }
done:
    /* Only this worker calls WinHTTP APIs or closes handles. Any pending ASYNC
     * operation is canceled after its initiating API has returned. Session and
     * connection have no callback/context; only the request can access source. */
    if(request) {
        BOOL closed=WinHttpCloseHandle(request);
        if(!closed && callback_ready) {
            /* A failed OS close gives no proof that callbacks have drained.
             * Fail closed and keep the backend reference/context alive rather
             * than turn this exceptional OS failure into a use-after-free. The
             * common eight-live-job limit bounds quarantined contexts. */
            win_error(job,GetLastError());
            if(connection) WinHttpCloseHandle(connection);
            if(session) WinHttpCloseHandle(session);
            free(url);free(host);free(path);return 0;
        }
        if(closed && callback_ready) {
            /* OS teardown can outlive cancellation. Retain the backend reference
             * until the documented final HANDLE_CLOSING callback, never time out
             * by freeing memory that a late completion might still access. */
            WaitForSingleObject(source->closed,INFINITE);
        }
    }
    if(connection) WinHttpCloseHandle(connection);
    if(session) WinHttpCloseHandle(session);
    free(url);free(host);free(path);
    if(success && active(source)) platform_library_source_success(job);
    platform_library_source_backend_done(job);
    return 0;
}
static bool win_start(PlatformLibrarySource *job,void *userdata) {
    (void)userdata;WinSource *source=calloc(1,sizeof *source);
    if(!source) { platform_library_source_error(job,"Cannot allocate HTTP transport");return false; }
    source->job=job;platform_library_source_set_context(job,source);
    source->ready=CreateEventW(NULL,FALSE,FALSE,NULL);
    if(!source->ready) { win_error(job,GetLastError());return false; }
    source->closed=CreateEventW(NULL,TRUE,FALSE,NULL);
    if(!source->closed) { win_error(job,GetLastError());return false; }
    SDL_Thread *thread=SDL_CreateThread(win_worker,"library-http",source);
    if(!thread) { platform_library_source_error(job,"Cannot start HTTP worker");return false; }
    SDL_DetachThread(thread);return true;
}
static void win_cancel(PlatformLibrarySource *job) {
    WinSource *source=platform_library_source_context(job);if(source) SDL_SetAtomicInt(&source->canceled,1);
}
static void win_destroy(PlatformLibrarySource *job) {
    WinSource *source=platform_library_source_context(job);if(!source)return;
    if(source->ready) CloseHandle(source->ready);
    if(source->closed) CloseHandle(source->closed);
    free(source);
}
const PlatformLibrarySourceTransport platform_library_source_transport={win_start,win_cancel,win_destroy};
