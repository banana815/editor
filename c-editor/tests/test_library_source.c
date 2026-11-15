#include "../src/platform_library_source_internal.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
typedef struct { unsigned starts,cancels,destroys;uint64_t now;size_t allocations,fail_at;bool start_fails; } Mock;
static bool start(PlatformLibrarySource *job,void *userdata) { Mock *m=userdata;platform_library_source_set_context(job,m);++m->starts;return !m->start_fails; }
static void cancel(PlatformLibrarySource *job) { Mock *m=platform_library_source_context(job);++m->cancels; }
static void destroy(PlatformLibrarySource *job) { Mock *m=platform_library_source_context(job);++m->destroys; }
const PlatformLibrarySourceTransport platform_library_source_transport={start,cancel,destroy};
static uint64_t clock_ms(void *context) { return ((Mock *)context)->now; }
static bool allocation(void *context,size_t size) { Mock *m=context;(void)size;return ++m->allocations<m->fail_at || !m->fail_at; }
static PlatformLibrarySource *open_job(const char *url,Mock *m,const PlatformLibrarySourceOptions *options) {
    PlatformLibrarySourceTestHooks hooks={clock_ms,allocation,m};char error[256];
    return platform_library_source_with_transport(url,options,&platform_library_source_transport,m,&hooks,error,sizeof error);
}
static void drop(PlatformLibrarySource *job) { platform_library_source_free(job);platform_library_source_backend_done(job); }
static void urls(void) {
    static const char *good[]={"http://127.0.0.1:23119/better-bibtex/export?/library.json","http://127.255.3.4/","http://10.20.30.40/","http://172.16.0.1/","http://172.31.255.255/","http://192.168.3.4/","http://169.254.2.3/","http://LOCALHOST/","http://localhost./","http://a.local/","http://a.b.localhost:8080/","http://[::1]/","http://[fc00::123]/","http://[fdff:ffff::]/","http://[fe80::1]/","http://[febf::1]/","http://[::ffff:127.0.0.1]/","http://[0:0:0:0:0:ffff:192.168.1.3]/","https://example.com/%E4%B8%AD?x=%23","HTTPS://example.com?x=1","https://[2001:db8::1]:443/"};
    static const char *bad[]={"","file:///tmp/a","ftp://localhost/a","http://example.com/","http://localhost.evil/","http://a.local.evil/","http://mylocalhost/","http://.local/","http://a..local/","http://-a.local/","http://a-.local/","http://127.1/","http://2130706433/","http://0177.0.0.1/","http://0x7f000001/","http://172.15.0.1/","http://172.32.0.1/","http://192.169.0.1/","http://0.0.0.0/","http://100.64.0.1/","http://[::]/","http://[2001:db8::1]/","http://[fec0::1]/","http://[ff00::1]/","http://[::ffff:8.8.8.8]/","http://[::1::2]/","http://[1:2:3:4:5:6:7:8:9]/","http://[fe80::1%25en0]/","http://localhost:0/","http://localhost:65536/","http://localhost:/","http://localhost:12x/","https://user:pass@example.com/","http://localhost/#fragment","http://localhost\\@evil/","http://localhost/\r\nX:a","http://localhost/%","http://localhost/%xz","http://localhost/中文","http:///missing","http://localhost%00.evil/","https://[::1]junk/"};
    for(size_t i=0;i<sizeof good/sizeof *good;i++) { Mock m={0};PlatformLibrarySource *j=open_job(good[i],&m,NULL);if(!j)fprintf(stderr,"valid URL rejected: %s\n",good[i]);assert(j && m.starts==1);drop(j);assert(m.destroys==1); }
    for(size_t i=0;i<sizeof bad/sizeof *bad;i++) { Mock m={0};PlatformLibrarySource *j=open_job(bad[i],&m,NULL);if(j)fprintf(stderr,"invalid URL accepted: %s\n",bad[i]);assert(!j && !m.starts); }
    char *huge=malloc(PLATFORM_LIBRARY_SOURCE_MAX_URL+2);assert(huge);memset(huge,'x',PLATFORM_LIBRARY_SOURCE_MAX_URL+1);huge[PLATFORM_LIBRARY_SOURCE_MAX_URL+1]=0;
    Mock m={0};assert(!open_job(huge,&m,NULL));free(huge);
}
static void lifecycle(void) {
    Mock m={0};PlatformLibrarySourceOptions options={5,100};PlatformLibrarySource *j=open_job("http://localhost/",&m,&options);assert(j);
    PlatformLibrarySourceResult out={0};assert(!platform_library_source_take_result(j,&out));
    assert(platform_library_source_response(j,200));const unsigned char bytes[]={1,0,2,0xff,3};
    assert(platform_library_source_append(j,bytes,2));assert(platform_library_source_append(j,bytes+2,3));
    platform_library_source_success(j);platform_library_source_error(j,"late failure");platform_library_source_cancel(j);
    assert(platform_library_source_status(j,NULL)==PLATFORM_LIBRARY_SOURCE_DONE);
    assert(platform_library_source_take_result(j,&out));assert(out.size==5 && out.data[5]==0 && !memcmp(out.data,bytes,5));
    PlatformLibrarySourceResult second={0};assert(!platform_library_source_take_result(j,&second));
    platform_library_source_backend_done(j);platform_library_source_free(j);assert(m.destroys==1 && !m.cancels);platform_library_source_result_free(&out);
    m=(Mock){0};j=open_job("http://localhost/",&m,&options);assert(j && platform_library_source_response(j,200));assert(platform_library_source_append(j,bytes,4));
    assert(!platform_library_source_append(j,bytes,SIZE_MAX));assert(platform_library_source_status(j,NULL)==PLATFORM_LIBRARY_SOURCE_FAILED && m.cancels==1);
    platform_library_source_success(j);assert(!platform_library_source_take_result(j,&out));drop(j);assert(m.destroys==1 && m.cancels==1);
    m=(Mock){0};j=open_job("http://localhost/",&m,NULL);assert(j);assert(!platform_library_source_response(j,302));assert(!platform_library_source_append(j,bytes,1));drop(j);
    m=(Mock){0};j=open_job("http://localhost/",&m,NULL);assert(j);assert(!platform_library_source_append(j,bytes,1));drop(j);
    m=(Mock){0};j=open_job("http://localhost/",&m,NULL);assert(j && platform_library_source_response(j,204));platform_library_source_success(j);assert(platform_library_source_take_result(j,&out) && !out.size && out.data && !out.data[0]);platform_library_source_result_free(&out);platform_library_source_backend_done(j);platform_library_source_free(j);
    m=(Mock){0};j=open_job("http://localhost/",&m,NULL);assert(j);platform_library_source_free(j);assert(m.cancels==1 && !m.destroys);
    assert(!platform_library_source_response(j,200));assert(!platform_library_source_append(j,bytes,5));platform_library_source_success(j);platform_library_source_error(j,"late");platform_library_source_backend_done(j);assert(m.destroys==1);
    m=(Mock){.start_fails=true};j=open_job("http://localhost/",&m,NULL);assert(j && platform_library_source_status(j,NULL)==PLATFORM_LIBRARY_SOURCE_FAILED);platform_library_source_free(j);assert(m.destroys==1);
}
static void limits(void) {
    PlatformLibrarySourceOptions o={1,100};Mock m={.now=1000};PlatformLibrarySource *j=open_job("http://localhost/",&m,&o);assert(j);
    m.now=1099;assert(platform_library_source_active(j));m.now=1100;assert(platform_library_source_status(j,NULL)==PLATFORM_LIBRARY_SOURCE_FAILED && m.cancels==1);drop(j);
    m=(Mock){.now=1000};j=open_job("http://localhost/",&m,&o);assert(j);m.now=999;assert(!platform_library_source_active(j));drop(j);
    o.max_bytes=PLATFORM_LIBRARY_SOURCE_MAX_BYTES+1;m=(Mock){0};assert(!open_job("http://localhost/",&m,&o));
    o.max_bytes=0;o.timeout_ms=PLATFORM_LIBRARY_SOURCE_MAX_TIMEOUT_MS+1;assert(!open_job("http://localhost/",&m,&o));
    PlatformLibrarySource *jobs[8]={0};Mock states[8]={0};for(size_t i=0;i<8;i++) { jobs[i]=open_job("http://localhost/",&states[i],NULL);assert(jobs[i]);platform_library_source_free(jobs[i]); }
    assert(!open_job("http://localhost/",&m,NULL));for(size_t i=0;i<8;i++)platform_library_source_backend_done(jobs[i]);
    j=open_job("http://localhost/",&m,NULL);assert(j);drop(j);
}
static size_t oom_trial(size_t fail_at) {
    Mock m={.fail_at=fail_at};PlatformLibrarySourceOptions o={20000,100};PlatformLibrarySource *j=open_job("http://localhost/library.json",&m,&o);
    if(j) {
        if(platform_library_source_response(j,200)) {
            char bytes[8192];memset(bytes,'a',sizeof bytes);
            if(platform_library_source_append(j,bytes,1) && platform_library_source_append(j,bytes,sizeof bytes) && platform_library_source_append(j,bytes,sizeof bytes)) platform_library_source_success(j);
        }
        PlatformLibrarySourceResult out={0};bool ok=platform_library_source_take_result(j,&out);
        assert(ok==(!fail_at || m.allocations<fail_at));if(ok)assert(out.size==16385 && out.data[out.size]==0);
        platform_library_source_result_free(&out);drop(j);assert(m.destroys==1);
    } else assert(fail_at && m.allocations>=fail_at);
    return m.allocations;
}
int main(int argc,char **argv) {
    urls();lifecycle();limits();size_t count=oom_trial(0);for(size_t i=1;i<=count;i++)oom_trial(i);
    if(argc>1) {
        char error[256];PlatformLibrarySource *j=platform_library_source_reopen_sqlite(argv[1],error,sizeof error);
#ifdef __EMSCRIPTEN__
        assert(!j);
#else
        assert(j);PlatformLibrarySourceResult result={0};assert(platform_library_source_take_result(j,&result));assert(result.native_path && !strcmp(result.native_path,argv[1]) && !result.data);platform_library_source_result_free(&result);platform_library_source_free(j);
#endif
    }
    puts("Library source core: strict URL ranges, binary streaming, bounds/deadlines, cancel/late replies, ownership and all reached allocation failures passed");return 0;
}
