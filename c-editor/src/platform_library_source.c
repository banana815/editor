#include "platform_library_source_internal.h"
#include <SDL3/SDL.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if defined(SDL_PLATFORM_WINDOWS)
#include <windows.h>
#include <sys/stat.h>
#elif !defined(SDL_PLATFORM_IOS) && !defined(SDL_PLATFORM_ANDROID) && !defined(__EMSCRIPTEN__)
#include <sys/stat.h>
#endif

struct PlatformLibrarySource {
    SDL_SpinLock lock;
    unsigned references;
    PlatformLibrarySourceStatus status;
    bool backend_active,cancel_sent,taken;
    char *url,*native_path;
    unsigned char *data;
    size_t size,capacity,limit;
    uint32_t timeout_ms;
    uint64_t started_ms;
    unsigned http_status;
    char message[256];
    void *context;
    const PlatformLibrarySourceTransport *transport;
    PlatformLibrarySourceTestHooks hooks;
};
static SDL_SpinLock lifetime_lock;
static unsigned live_jobs;
#define SOURCE_MAX_JOBS 8u

static void message(char *out,size_t capacity,const char *text) {
    if(out && capacity) snprintf(out,capacity,"%s",text?text:"Library source failed");
}
static size_t bounded_length(const char *text,size_t maximum) {
    if(!text) return SIZE_MAX;
    size_t n=0;while(n<=maximum && text[n]) ++n;return n>maximum?SIZE_MAX:n;
}
static int hex(unsigned char c) {
    return c>='0'&&c<='9'?c-'0':c>='a'&&c<='f'?c-'a'+10:c>='A'&&c<='F'?c-'A'+10:-1;
}
static unsigned char lower(unsigned char c) { return c>='A'&&c<='Z'?(unsigned char)(c+('a'-'A')):c; }
static bool equal_ascii(const char *s,size_t n,const char *value) {
    if(strlen(value)!=n) return false;
    for(size_t i=0;i<n;i++) if(lower((unsigned char)s[i])!=(unsigned char)value[i]) return false;
    return true;
}
static bool ipv4(const char *s,size_t n,unsigned char bytes[4]) {
    size_t at=0;
    for(size_t part=0;part<4;part++) {
        size_t start=at;unsigned value=0;
        while(at<n && s[at]>='0' && s[at]<='9') { value=value*10u+(unsigned)(s[at++]-'0');if(value>255 || at-start>3) return false; }
        if(at==start || (at-start>1 && s[start]=='0')) return false;
        bytes[part]=(unsigned char)value;
        if(part<3) { if(at==n || s[at++]!='.') return false; }
    }
    return at==n;
}
static bool private_ipv4(const unsigned char p[4]) {
    return p[0]==127 || p[0]==10 || (p[0]==172 && p[1]>=16 && p[1]<=31) ||
        (p[0]==192 && p[1]==168) || (p[0]==169 && p[1]==254);
}
static bool ipv6(const char *s,size_t n,unsigned char bytes[16]) {
    unsigned words[8]={0};size_t count=0,at=0;int compressed=-1;
    if(!n) return false;
    if(s[0]==':') { if(n<2 || s[1]!=':') return false;compressed=0;at=2; }
    while(at<n) {
        if(count==8) return false;
        size_t start=at;unsigned value=0;
        while(at<n && hex((unsigned char)s[at])>=0) {
            if(at-start==4) return false;value=value*16u+(unsigned)hex((unsigned char)s[at++]);
        }
        if(at<n && s[at]=='.') {
            unsigned char tail[4];if(count>6 || !ipv4(s+start,n-start,tail)) return false;
            words[count++]=(unsigned)tail[0]*256u+tail[1];words[count++]=(unsigned)tail[2]*256u+tail[3];at=n;break;
        }
        if(at==start) return false;
        words[count++]=value;
        if(at==n) break;
        if(s[at++]!=':' || at==n) return false;
        if(s[at]==':') { if(compressed>=0) return false;compressed=(int)count;++at; }
    }
    if(compressed>=0) {
        if(count==8) return false;
        size_t zeros=8-count,first=(size_t)compressed;
        memmove(words+first+zeros,words+first,(count-first)*sizeof *words);
        memset(words+first,0,zeros*sizeof *words);
    } else if(count!=8) return false;
    for(size_t i=0;i<8;i++) { bytes[i*2]=(unsigned char)(words[i]>>8);bytes[i*2+1]=(unsigned char)words[i]; }
    return true;
}
static bool private_ipv6(const unsigned char p[16]) {
    if((p[0]&0xfe)==0xfc || (p[0]==0xfe && (p[1]&0xc0)==0x80)) return true;
    bool zero=true;for(size_t i=0;i<15;i++) if(p[i]) zero=false;
    if(zero && p[15]==1) return true;
    zero=true;for(size_t i=0;i<10;i++) if(p[i]) zero=false;
    return zero && p[10]==0xff && p[11]==0xff && private_ipv4(p+12);
}
static bool hostname(const char *s,size_t n) {
    if(n && s[n-1]=='.') --n;
    if(!n || n>253) return false;
    size_t label=0;
    for(size_t i=0;i<n;i++) {
        unsigned char c=(unsigned char)s[i];
        if(c=='.') { if(!label || label>63 || s[i-1]=='-') return false;label=0;continue; }
        if(!((c>='a'&&c<='z')||(c>='A'&&c<='Z')||(c>='0'&&c<='9')||c=='-') || (!label && c=='-')) return false;
        ++label;
    }
    return label && label<=63 && s[n-1]!='-';
}
static bool local_hostname(const char *s,size_t n) {
    if(n && s[n-1]=='.') --n;
    return equal_ascii(s,n,"localhost") || (n>6 && equal_ascii(s+n-6,6,".local")) ||
        (n>10 && equal_ascii(s+n-10,10,".localhost"));
}
static bool url_valid(const char *url,char *error,size_t capacity) {
    size_t n=bounded_length(url,PLATFORM_LIBRARY_SOURCE_MAX_URL),start;
    bool secure=false;
    if(n==SIZE_MAX || !n) { message(error,capacity,"URL is missing or exceeds 8192 bytes");return false; }
    if(n>=8 && equal_ascii(url,8,"https://")) { secure=true;start=8; }
    else if(n>=7 && equal_ascii(url,7,"http://")) start=7;
    else { message(error,capacity,"Only HTTP and HTTPS URLs are supported");return false; }
    for(size_t i=0;i<n;i++) {
        unsigned char c=(unsigned char)url[i];
        if(c<=32 || c>=127 || c=='\\' || c=='#') { message(error,capacity,"URL must be ASCII with escaped path bytes, no spaces, controls or fragment");return false; }
        if(c=='%') { if(i+2>=n || hex((unsigned char)url[i+1])<0 || hex((unsigned char)url[i+2])<0) { message(error,capacity,"Invalid percent escape in URL");return false; }i+=2; }
    }
    size_t end=start;while(end<n && url[end]!='/' && url[end]!='?') ++end;
    if(start==end || memchr(url+start,'@',end-start)) { message(error,capacity,"URL needs a host and must not contain credentials");return false; }
    bool local=false;size_t port=end;
    if(url[start]=='[') {
        size_t close=start+1;while(close<end && url[close]!=']') ++close;
        unsigned char ip[16];
        if(close==end || !ipv6(url+start+1,close-start-1,ip)) { message(error,capacity,"Invalid IPv6 address; scoped address identifiers are not supported");return false; }
        local=private_ipv6(ip);port=close+1;
        if(port<end && url[port]!=':') { message(error,capacity,"Invalid URL authority");return false; }
    } else {
        size_t host_end=start;while(host_end<end && url[host_end]!=':') ++host_end;
        unsigned char ip[4];bool digits=true;
        for(size_t i=start;i<host_end;i++) if((url[i]<'0'||url[i]>'9') && url[i]!='.') digits=false;
        if(digits) {
            if(!ipv4(url+start,host_end-start,ip)) { message(error,capacity,"IPv4 addresses must use four canonical decimal octets");return false; }
            local=private_ipv4(ip);
        } else {
            if(!hostname(url+start,host_end-start)) { message(error,capacity,"Invalid ASCII hostname");return false; }
            local=local_hostname(url+start,host_end-start);
        }
        port=host_end;
    }
    if(port<end) {
        unsigned value=0;if(url[port++]!=':' || port==end) { message(error,capacity,"Invalid URL port");return false; }
        for(;port<end;port++) {
            if(url[port]<'0'||url[port]>'9') { message(error,capacity,"Invalid URL port");return false; }
            value=value*10u+(unsigned)(url[port]-'0');if(value>65535) { message(error,capacity,"Invalid URL port");return false; }
        }
        if(!value) { message(error,capacity,"URL port must be between 1 and 65535");return false; }
    }
    if(!secure && !local) { message(error,capacity,"HTTP is limited to localhost, private/link-local IPs and .local/.localhost hosts; use HTTPS otherwise");return false; }
    return true;
}
static uint64_t now_ms(PlatformLibrarySource *job) { return job->hooks.clock_ms?job->hooks.clock_ms(job->hooks.context):SDL_GetTicks(); }
static bool allocation(PlatformLibrarySource *job,size_t size) { return !job->hooks.allow_allocation || job->hooks.allow_allocation(job->hooks.context,size); }
static void discard(PlatformLibrarySource *job) { free(job->data);job->data=NULL;job->size=job->capacity=0; }
static void fail_locked(PlatformLibrarySource *job,const char *text) {
    if(job->status!=PLATFORM_LIBRARY_SOURCE_PENDING) return;
    job->status=PLATFORM_LIBRARY_SOURCE_FAILED;message(job->message,sizeof job->message,text);discard(job);
}
static bool active_locked(PlatformLibrarySource *job) {
    if(job->status!=PLATFORM_LIBRARY_SOURCE_PENDING) return false;
    uint64_t now=now_ms(job);
    if(now<job->started_ms || now-job->started_ms>=job->timeout_ms) { fail_locked(job,"Library source request timed out");return false; }
    return true;
}
static void release(PlatformLibrarySource *job) {
    bool destroy=false;
    SDL_LockSpinlock(&lifetime_lock);
    if(!--job->references) { --live_jobs;destroy=true; }
    SDL_UnlockSpinlock(&lifetime_lock);
    if(destroy) {
        if(job->transport && job->transport->destroy) job->transport->destroy(job);
        free(job->url);free(job->native_path);free(job->data);free(job);
    }
}
static void notify_cancel(PlatformLibrarySource *job) {
    SDL_LockSpinlock(&job->lock);
    bool notify=job->backend_active && !job->cancel_sent &&
        (job->status==PLATFORM_LIBRARY_SOURCE_CANCELED || job->status==PLATFORM_LIBRARY_SOURCE_FAILED);
    if(notify) job->cancel_sent=true;
    SDL_UnlockSpinlock(&job->lock);
    if(notify && job->transport->cancel) job->transport->cancel(job);
}
static PlatformLibrarySource *create(const PlatformLibrarySourceOptions *options,
    const PlatformLibrarySourceTestHooks *hooks,char *error,size_t capacity) {
    size_t limit=options && options->max_bytes?options->max_bytes:PLATFORM_LIBRARY_SOURCE_MAX_BYTES;
    uint32_t timeout=options && options->timeout_ms?options->timeout_ms:PLATFORM_LIBRARY_SOURCE_DEFAULT_TIMEOUT_MS;
    if(limit>PLATFORM_LIBRARY_SOURCE_MAX_BYTES || timeout>PLATFORM_LIBRARY_SOURCE_MAX_TIMEOUT_MS) { message(error,capacity,"Library source size/time limit is invalid");return NULL; }
    SDL_LockSpinlock(&lifetime_lock);
    bool admitted=live_jobs<SOURCE_MAX_JOBS;if(admitted) ++live_jobs;
    SDL_UnlockSpinlock(&lifetime_lock);
    if(!admitted) { message(error,capacity,"Too many unfinished library source jobs");return NULL; }
    PlatformLibrarySource *job=NULL;
    if(!hooks || !hooks->allow_allocation || hooks->allow_allocation(hooks->context,sizeof *job)) job=calloc(1,sizeof *job);
    if(!job) {
        SDL_LockSpinlock(&lifetime_lock);--live_jobs;SDL_UnlockSpinlock(&lifetime_lock);
        message(error,capacity,"Out of memory creating library source");return NULL;
    }
    if(hooks) job->hooks=*hooks;
    job->references=1;job->limit=limit;job->timeout_ms=timeout;job->started_ms=now_ms(job);
    return job;
}
PlatformLibrarySource *platform_library_source_with_transport(const char *url,
    const PlatformLibrarySourceOptions *options,const PlatformLibrarySourceTransport *transport,
    void *userdata,const PlatformLibrarySourceTestHooks *hooks,char *error,size_t capacity) {
    if(!url_valid(url,error,capacity)) return NULL;
    if(!transport || !transport->start) { message(error,capacity,"Library source transport is unavailable");return NULL; }
    PlatformLibrarySource *job=create(options,hooks,error,capacity);if(!job) return NULL;
    size_t size=strlen(url)+1;
    if(allocation(job,size)) job->url=malloc(size);
    if(!job->url) { message(error,capacity,"Out of memory copying source URL");release(job);return NULL; }
    memcpy(job->url,url,size);job->transport=transport;
    job->backend_active=true;
    SDL_LockSpinlock(&lifetime_lock);++job->references;SDL_UnlockSpinlock(&lifetime_lock);
    if(!platform_library_source_active(job) || !transport->start(job,userdata)) {
        platform_library_source_error(job,"Could not start library source transport");
        platform_library_source_backend_done(job);
    }
    return job;
}
PlatformLibrarySource *platform_library_source_fetch(const char *url,
    const PlatformLibrarySourceOptions *options,char *error,size_t capacity) {
    return platform_library_source_with_transport(url,options,&platform_library_source_transport,NULL,NULL,error,capacity);
}
const char *platform_library_source_url(PlatformLibrarySource *job) { return job->url; }
size_t platform_library_source_byte_limit(PlatformLibrarySource *job) { return job->limit; }
uint32_t platform_library_source_timeout(PlatformLibrarySource *job) { return job->timeout_ms; }
void platform_library_source_set_context(PlatformLibrarySource *job,void *context) { job->context=context; }
void *platform_library_source_context(PlatformLibrarySource *job) { return job->context; }
bool platform_library_source_active(PlatformLibrarySource *job) {
    SDL_LockSpinlock(&job->lock);bool ok=active_locked(job);SDL_UnlockSpinlock(&job->lock);return ok;
}
bool platform_library_source_response(PlatformLibrarySource *job,unsigned status) {
    SDL_LockSpinlock(&job->lock);
    bool ok=active_locked(job);
    if(ok && (status<200 || status>=300 || job->http_status)) {
        char error[100];snprintf(error,sizeof error,"Library source returned HTTP status %u%s",status,job->http_status?" after another response":"");
        fail_locked(job,error);ok=false;
    }
    if(ok) job->http_status=status;
    SDL_UnlockSpinlock(&job->lock);return ok;
}
bool platform_library_source_append(PlatformLibrarySource *job,const void *bytes,size_t size) {
    SDL_LockSpinlock(&job->lock);
    bool ok=active_locked(job);
    if(ok && (!job->http_status || (!bytes && size))) { fail_locked(job,"Library source transport returned invalid data");ok=false; }
    if(ok && size>job->limit-job->size) { fail_locked(job,"Library source exceeds the configured byte limit");ok=false; }
    if(ok && size) {
        size_t needed=job->size+size+1;
        if(needed>job->capacity) {
            size_t capacity=job->capacity?job->capacity:4096;
            while(capacity<needed && capacity<(job->limit+1)/2) capacity*=2;
            if(capacity<needed) capacity=needed;
            if(capacity>job->limit+1) capacity=job->limit+1;
            unsigned char *next=allocation(job,capacity)?realloc(job->data,capacity):NULL;
            if(!next) { fail_locked(job,"Out of memory receiving library source");ok=false; }
            else { job->data=next;job->capacity=capacity; }
        }
        if(ok) { memcpy(job->data+job->size,bytes,size);job->size+=size;job->data[job->size]=0; }
    }
    SDL_UnlockSpinlock(&job->lock);return ok;
}
void platform_library_source_success(PlatformLibrarySource *job) {
    SDL_LockSpinlock(&job->lock);
    if(active_locked(job)) {
        if(!job->http_status) fail_locked(job,"Library source completed without an HTTP response");
        else {
            if(!job->data && allocation(job,1)) job->data=calloc(1,1);
            if(!job->data) fail_locked(job,"Out of memory completing library source");
            else job->status=PLATFORM_LIBRARY_SOURCE_DONE;
        }
    }
    SDL_UnlockSpinlock(&job->lock);
}
void platform_library_source_error(PlatformLibrarySource *job,const char *text) {
    SDL_LockSpinlock(&job->lock);fail_locked(job,text);SDL_UnlockSpinlock(&job->lock);
}
void platform_library_source_backend_done(PlatformLibrarySource *job) {
    SDL_LockSpinlock(&job->lock);
    bool active=job->backend_active;job->backend_active=false;
    if(active && job->status==PLATFORM_LIBRARY_SOURCE_PENDING) fail_locked(job,"Library source transport ended before completion");
    SDL_UnlockSpinlock(&job->lock);
    if(active) release(job);
}
PlatformLibrarySourceStatus platform_library_source_status(PlatformLibrarySource *job,const char **out) {
    if(!job) { if(out) *out="Missing library source job";return PLATFORM_LIBRARY_SOURCE_FAILED; }
    SDL_LockSpinlock(&job->lock);active_locked(job);
    PlatformLibrarySourceStatus status=job->status;
    if(out) *out=status==PLATFORM_LIBRARY_SOURCE_PENDING?"":job->message;
    SDL_UnlockSpinlock(&job->lock);notify_cancel(job);return status;
}
bool platform_library_source_take_result(PlatformLibrarySource *job,PlatformLibrarySourceResult *out) {
    if(!job || !out || out->data || out->native_path || out->size || out->http_status) return false;
    SDL_LockSpinlock(&job->lock);
    bool ok=job->status==PLATFORM_LIBRARY_SOURCE_DONE && !job->taken;
    if(ok) {
        *out=(PlatformLibrarySourceResult){job->data,job->size,job->native_path,job->http_status};
        job->data=NULL;job->size=job->capacity=0;job->native_path=NULL;job->taken=true;
    }
    SDL_UnlockSpinlock(&job->lock);return ok;
}
void platform_library_source_result_free(PlatformLibrarySourceResult *result) {
    if(result) { free(result->data);free(result->native_path);memset(result,0,sizeof *result); }
}
void platform_library_source_cancel(PlatformLibrarySource *job) {
    if(!job) return;
    SDL_LockSpinlock(&job->lock);
    if(job->status==PLATFORM_LIBRARY_SOURCE_PENDING) { job->status=PLATFORM_LIBRARY_SOURCE_CANCELED;message(job->message,sizeof job->message,"Library source request canceled");discard(job); }
    SDL_UnlockSpinlock(&job->lock);notify_cancel(job);
}
void platform_library_source_free(PlatformLibrarySource *job) { if(job) { platform_library_source_cancel(job);release(job); } }

static bool utf8_path(const char *s,size_t n) {
    for(size_t i=0;i<n;) {
        unsigned char c=(unsigned char)s[i++];if(c<32 || c==127) return false;if(c<128) continue;
        unsigned count,min,cp;
        if(c>=0xc2 && c<=0xdf) { count=1;min=0x80;cp=c&31; }
        else if(c>=0xe0 && c<=0xef) { count=2;min=0x800;cp=c&15; }
        else if(c>=0xf0 && c<=0xf4) { count=3;min=0x10000;cp=c&7; }
        else return false;
        if(count>n-i) return false;
        while(count--) { c=(unsigned char)s[i++];if((c&0xc0)!=0x80) return false;cp=cp*64+(c&63); }
        if(cp<min || cp>0x10ffff || (cp>=0xd800 && cp<=0xdfff)) return false;
    }
    return true;
}
PlatformLibrarySource *platform_library_source_reopen_sqlite(const char *path,char *error,size_t capacity) {
    size_t n=bounded_length(path,32768);
    if(n==SIZE_MAX || !n || !utf8_path(path,n)) { message(error,capacity,"Invalid UTF-8 native file path");return NULL; }
#if defined(SDL_PLATFORM_IOS) || defined(SDL_PLATFORM_ANDROID) || defined(__EMSCRIPTEN__)
    message(error,capacity,"Choose the file again on this platform; no persistent access grant is available");return NULL;
#else
    bool regular=false;
#ifdef SDL_PLATFORM_WINDOWS
    bool absolute=(n>2 && ((path[0]>='A'&&path[0]<='Z')||(path[0]>='a'&&path[0]<='z')) && path[1]==':' && (path[2]=='\\'||path[2]=='/')) ||
        (n>4 && path[0]=='\\' && path[1]=='\\' && path[2]!='.' && path[2]!='?');
    if(!absolute) { message(error,capacity,"An absolute native file path is required");return NULL; }
    int count=MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,path,-1,NULL,0);
    wchar_t *wide=count>0?malloc((size_t)count*sizeof *wide):NULL;
    if(!wide) { message(error,capacity,"Out of memory converting file path");return NULL; }
    struct _stat64 st;
    if(MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,path,-1,wide,count) && !_wstat64(wide,&st)) regular=(st.st_mode&_S_IFMT)==_S_IFREG;
    free(wide);
#else
    if(path[0]!='/') { message(error,capacity,"An absolute native file path is required");return NULL; }
    struct stat st;if(!stat(path,&st)) regular=S_ISREG(st.st_mode);
#endif
    if(!regular) { message(error,capacity,"The selected source is missing or is not a regular file");return NULL; }
    PlatformLibrarySource *job=create(NULL,NULL,error,capacity);if(!job) return NULL;
    job->native_path=malloc(n+1);
    if(!job->native_path) { message(error,capacity,"Out of memory copying file path");release(job);return NULL; }
    memcpy(job->native_path,path,n+1);job->status=PLATFORM_LIBRARY_SOURCE_DONE;return job;
#endif
}
