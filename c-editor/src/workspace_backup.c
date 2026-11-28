#ifndef _WIN32
#define _POSIX_C_SOURCE 200809L
#ifdef __APPLE__
#define _DARWIN_C_SOURCE
#endif
#endif
#include "workspace_backup.h"
#include "bundle_archive.h"
#include "note_store.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <errno.h>
#include <time.h>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
typedef struct { HANDLE handle; wchar_t *path; } Dir;
#else
#include <unistd.h>
#include <fcntl.h>
#include <dirent.h>
#include <sys/stat.h>
typedef struct {int fd;} Dir;
#endif
static bool fail(char *e,size_t n,const char *s){if(e&&n)snprintf(e,n,"%s",s);return false;}
static char *copy(const char *s){size_t n=strlen(s)+1;char *p=malloc(n);if(p)memcpy(p,s,n);return p;}
static char *join(const char *a,const char *b){size_t n=strlen(a),m=strlen(b);if(n>SIZE_MAX-m-2)return NULL;char *p=malloc(n+m+2);if(p){memcpy(p,a,n);p[n]='/';memcpy(p+n+1,b,m+1);}return p;}
#ifdef _WIN32
static wchar_t *wide(const char *s){int n=MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,s,-1,NULL,0);wchar_t *w=n?malloc((size_t)n*sizeof *w):NULL;if(w&&!MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,s,-1,w,n)){free(w);return NULL;}return w;}
static char *utf8(const wchar_t *s){int n=WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,s,-1,NULL,0,NULL,NULL);char *p=n?malloc((size_t)n):NULL;if(p&&!WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,s,-1,p,n,NULL,NULL)){free(p);return NULL;}return p;}
static wchar_t *path_at(const Dir *d,const char *name){wchar_t *s=wide(name);if(!s)return NULL;size_t n=wcslen(d->path),m=wcslen(s);wchar_t *p=malloc((n+m+2)*sizeof *p);if(p){memcpy(p,d->path,n*sizeof *p);p[n]=L'/';memcpy(p+n+1,s,(m+1)*sizeof *p);}free(s);return p;}
static bool dir_path(Dir *d,wchar_t *path){HANDLE h=CreateFileW(path,FILE_LIST_DIRECTORY,FILE_SHARE_READ|FILE_SHARE_WRITE,NULL,OPEN_EXISTING,FILE_FLAG_BACKUP_SEMANTICS|FILE_FLAG_OPEN_REPARSE_POINT,NULL);BY_HANDLE_FILE_INFORMATION i;
    if(h==INVALID_HANDLE_VALUE){DWORD why=GetLastError();free(path);SetLastError(why);return false;}
    bool queried=GetFileInformationByHandle(h,&i)!=0;DWORD why=queried?ERROR_INVALID_DATA:GetLastError();
    if(!queried||!(i.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY)||(i.dwFileAttributes&FILE_ATTRIBUTE_REPARSE_POINT)){CloseHandle(h);free(path);SetLastError(why);return false;}d->handle=h;d->path=path;return true;}
static bool dir_open(Dir *d,const char *path){wchar_t *p=wide(path);return p&&dir_path(d,p);}
static void dir_close(Dir *d){CloseHandle(d->handle);free(d->path);}
static bool child_dir(const Dir *parent,const char *name,Dir *out,bool create,bool exclusive){wchar_t *p=path_at(parent,name);if(!p)return false;if(create&&!CreateDirectoryW(p,NULL)&&(exclusive||GetLastError()!=ERROR_ALREADY_EXISTS)){free(p);return false;}return dir_path(out,p);}
static bool read_file(const Dir *parent,const char *name,unsigned char **bytes,size_t *size){wchar_t *p=path_at(parent,name);if(!p)return false;HANDLE h=CreateFileW(p,GENERIC_READ,FILE_SHARE_READ,NULL,OPEN_EXISTING,FILE_FLAG_OPEN_REPARSE_POINT,NULL);free(p);if(h==INVALID_HANDLE_VALUE)return false;BY_HANDLE_FILE_INFORMATION i;LARGE_INTEGER len={0};bool ok=GetFileInformationByHandle(h,&i)&&!(i.dwFileAttributes&(FILE_ATTRIBUTE_DIRECTORY|FILE_ATTRIBUTE_REPARSE_POINT))&&GetFileSizeEx(h,&len)&&len.QuadPart>=0&&(uint64_t)len.QuadPart<=CE_BUNDLE_MAX_BYTES;size_t n=ok?(size_t)len.QuadPart:0;unsigned char *b=ok?malloc(n?n:1):NULL;ok=ok&&b;DWORD got=0;if(ok)ok=ReadFile(h,b,(DWORD)n,&got,NULL)&&got==n;unsigned char extra;if(ok)ok=ReadFile(h,&extra,1,&got,NULL)&&got==0;CloseHandle(h);if(!ok){free(b);return false;}*bytes=b;*size=n;return true;}
static bool write_file(const Dir *parent,const char *name,const unsigned char *b,size_t n){wchar_t *p=path_at(parent,name);if(!p)return false;HANDLE h=CreateFileW(p,GENERIC_WRITE,0,NULL,CREATE_NEW,FILE_ATTRIBUTE_NORMAL|FILE_FLAG_OPEN_REPARSE_POINT,NULL);free(p);if(h==INVALID_HANDLE_VALUE)return false;DWORD wrote=0;bool ok=WriteFile(h,b,(DWORD)n,&wrote,NULL)&&wrote==n&&FlushFileBuffers(h);CloseHandle(h);return ok;}
#else
static bool dir_open(Dir *d,const char *path){d->fd=open(path,O_RDONLY|O_DIRECTORY|O_NOFOLLOW);return d->fd>=0;}
static void dir_close(Dir *d){close(d->fd);}
static bool child_dir(const Dir *parent,const char *name,Dir *out,bool create,bool exclusive){if(create&&mkdirat(parent->fd,name,0700)&&(exclusive||errno!=EEXIST))return false;out->fd=openat(parent->fd,name,O_RDONLY|O_DIRECTORY|O_NOFOLLOW);return out->fd>=0;}
static bool read_file(const Dir *parent,const char *name,unsigned char **bytes,size_t *size){int fd=openat(parent->fd,name,O_RDONLY|O_NOFOLLOW|O_NONBLOCK);if(fd<0)return false;struct stat st;bool ok=fstat(fd,&st)==0&&S_ISREG(st.st_mode)&&st.st_size>=0&&(uint64_t)st.st_size<=CE_BUNDLE_MAX_BYTES;size_t n=ok?(size_t)st.st_size:0;unsigned char *b=ok?malloc(n?n:1):NULL;ok=ok&&b;size_t pos=0;while(ok&&pos<n){ssize_t got=read(fd,b+pos,n-pos);if(got<0&&errno==EINTR)continue;if(got<=0)ok=false;else pos+=(size_t)got;}unsigned char extra;if(ok)ok=read(fd,&extra,1)==0;close(fd);if(!ok){free(b);return false;}*bytes=b;*size=n;return true;}
static bool write_file(const Dir *parent,const char *name,const unsigned char *b,size_t n){int fd=openat(parent->fd,name,O_WRONLY|O_CREAT|O_EXCL|O_NOFOLLOW,0600);if(fd<0)return false;size_t pos=0;bool ok=true;while(pos<n){ssize_t put=write(fd,b+pos,n-pos);if(put<0&&errno==EINTR)continue;if(put<=0){ok=false;break;}pos+=(size_t)put;}if(ok)ok=fsync(fd)==0;if(close(fd))ok=false;return ok;}
#endif
static bool control_name(const char *name){return !strcmp(name,".notelab-restored")||!strcmp(name,".notelab-recovery");}
static bool excluded(const char *name){return !strncmp(name,".editor-save-",13)&&strlen(name)>17&&!strcmp(name+strlen(name)-4,".tmp");}
typedef struct {CeBundle bundle;size_t bytes;char *error;size_t cap;} Capture;
static bool capture_dir(Capture *c,const Dir *d,const char *prefix,unsigned depth);
static bool capture_one(Capture *c,const Dir *d,const char *prefix,const char *name,bool directory,unsigned depth){
    if(!strcmp(name,".")||!strcmp(name,"..")||excluded(name)||(!depth&&control_name(name)))return true;
    char *rel=join(prefix,name);if(!rel)return fail(c->error,c->cap,"Cannot allocate backup path");
    if(!ce_bundle_valid_path(rel)){free(rel);return fail(c->error,c->cap,"Workspace has a nonportable filename");}
    if(directory){Dir child;bool ok=child_dir(d,name,&child,false,false);if(ok){ok=capture_dir(c,&child,rel,depth+1);dir_close(&child);}free(rel);return ok;}
    unsigned char *data=NULL;size_t size=0;
    if(c->bundle.count>=CE_BUNDLE_MAX_ENTRIES||!read_file(d,name,&data,&size)){free(rel);return fail(c->error,c->cap,"Cannot read workspace file, or file is a link");}
    size_t overhead=76+2*strlen(rel);
    if(overhead>CE_BUNDLE_MAX_BYTES-c->bytes||size>CE_BUNDLE_MAX_BYTES-c->bytes-overhead){free(data);free(rel);return fail(c->error,c->cap,"Workspace backup exceeds 128 MiB");}
    CeBundleEntry *p=realloc(c->bundle.entries,(c->bundle.count+1)*sizeof *p);if(!p){free(data);free(rel);return fail(c->error,c->cap,"Cannot allocate workspace backup");}
    c->bundle.entries=p;p[c->bundle.count++]=(CeBundleEntry){rel,data,size};c->bytes+=overhead+size;return true;
}
static bool capture_dir(Capture *c,const Dir *d,const char *prefix,unsigned depth){
    if(depth>32)return fail(c->error,c->cap,"Workspace folders exceed depth limit");bool ok=true;
#ifdef _WIN32
    wchar_t *pattern=path_at(d,"*");if(!pattern)return false;WIN32_FIND_DATAW item;HANDLE search=FindFirstFileW(pattern,&item);free(pattern);if(search==INVALID_HANDLE_VALUE)return GetLastError()==ERROR_FILE_NOT_FOUND;
    do {char *name=utf8(item.cFileName);if(!name){ok=false;break;}if(!excluded(name)&&!(depth==0&&control_name(name))&&(item.dwFileAttributes&FILE_ATTRIBUTE_REPARSE_POINT)){free(name);ok=false;break;}ok=capture_one(c,d,prefix,name,(item.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY)!=0,depth);free(name);if(!ok)break;}while(FindNextFileW(search,&item));if(ok&&GetLastError()!=ERROR_NO_MORE_FILES)ok=false;FindClose(search);
#else
    int fd=dup(d->fd);DIR *scan=fd>=0?fdopendir(fd):NULL;if(!scan){if(fd>=0)close(fd);return false;}struct dirent *item;errno=0;
    while((item=readdir(scan))){if(!strcmp(item->d_name,".")||!strcmp(item->d_name,"..")||excluded(item->d_name)||(depth==0&&control_name(item->d_name)))continue;struct stat st;if(fstatat(d->fd,item->d_name,&st,AT_SYMLINK_NOFOLLOW)||(!S_ISREG(st.st_mode)&&!S_ISDIR(st.st_mode))){ok=false;break;}if(!capture_one(c,d,prefix,item->d_name,S_ISDIR(st.st_mode),depth)){ok=false;break;}errno=0;}if(ok&&errno)ok=false;closedir(scan);
#endif
    return ok?true:fail(c->error,c->cap,"Cannot capture workspace; links and unreadable files are not supported");
}
static const char marker[]="{\"format\":\"NoteLab workspace\",\"version\":1}";
bool ce_workspace_backup(const char *root,unsigned char **bytes,size_t *size,char *error,size_t cap){
    Dir d;if(!root||!bytes||!size||!dir_open(&d,root))return fail(error,cap,"Cannot open workspace for backup");
    Capture c={0};c.error=error;c.cap=cap;c.bytes=22+76+2*strlen("notelab-backup.json")+strlen(marker);
    bool ok=capture_dir(&c,&d,"workspace",0);dir_close(&d);
    if(ok){CeBundleEntry *p=realloc(c.bundle.entries,(c.bundle.count+1)*sizeof *p);char *name=copy("notelab-backup.json"),*data=copy(marker);if(!p||!name||!data){if(p)c.bundle.entries=p;free(name);free(data);ok=false;}else{c.bundle.entries=p;p[c.bundle.count++]=(CeBundleEntry){name,(unsigned char *)data,strlen(data)};ok=ce_bundle_write(p,c.bundle.count,bytes,size,error,cap);}}
    ce_bundle_free(&c.bundle);if(!ok&&error&&cap&&!*error)fail(error,cap,"Cannot prepare workspace backup");return ok;
}
static bool relative_valid(const char *relative){if(!relative)return false;if(!*relative)return true;const char *prefix=".notelab-restored/";return !strncmp(relative,prefix,strlen(prefix))&&ce_bundle_valid_path(relative)&&!strchr(relative+strlen(prefix),'/');}
static bool open_relative(const Dir *base,const char *relative,Dir *out){if(!relative_valid(relative)||!*relative)return false;Dir restored;if(!child_dir(base,".notelab-restored",&restored,false,false))return false;bool ok=child_dir(&restored,relative+strlen(".notelab-restored/"),out,false,false);dir_close(&restored);return ok;}
static bool write_relative(Dir *root,const char *relative,const unsigned char *bytes,size_t size){const char *slash=strchr(relative,'/');if(!slash)return write_file(root,relative,bytes,size);size_t len=(size_t)(slash-relative);char name[256];if(!len||len>=sizeof name)return false;memcpy(name,relative,len);name[len]=0;Dir child;if(!child_dir(root,name,&child,true,false))return false;bool ok=write_relative(&child,slash+1,bytes,size);dir_close(&child);return ok;}
bool ce_workspace_restore_stage(const char *base,const void *bytes,size_t size,char **relative,char **root,char *error,size_t cap){
    CeBundle b={0};if(!relative||!root||!ce_bundle_read(bytes,size,&b,error,cap))return false;size_t markers=0;bool ok=true;
    for(size_t i=0;i<b.count;i++){const CeBundleEntry *v=&b.entries[i];if(!strcmp(v->name,"notelab-backup.json")){markers++;if(v->size!=strlen(marker)||memcmp(v->data,marker,v->size))ok=false;}else if(strncmp(v->name,"workspace/",10)||!v->name[10])ok=false;else{const char *p=v->name+10;while(*p){char name[256];const char *slash=strchr(p,'/');size_t n=slash?(size_t)(slash-p):strlen(p);if(n>=sizeof name){ok=false;break;}memcpy(name,p,n);name[n]=0;if(excluded(name)||(p==v->name+10&&control_name(name))){ok=false;break;}if(!slash)break;p=slash+1;}}}
    if(!ok||markers!=1){ce_bundle_free(&b);return fail(error,cap,"Not a complete Note Lab workspace backup");}
    Dir parent,container,stage;if(!dir_open(&parent,base)){ce_bundle_free(&b);return fail(error,cap,"Cannot open workspace base");}
    ok=child_dir(&parent,".notelab-restored",&container,true,false);dir_close(&parent);if(!ok){ce_bundle_free(&b);return fail(error,cap,"Cannot prepare recovery directory");}
    static unsigned sequence;char name[96],rel[128];bool created=false;
    for(unsigned attempt=0;attempt<1000&&!created;attempt++){snprintf(name,sizeof name,"restore-%lld-%u",(long long)time(NULL),++sequence);created=child_dir(&container,name,&stage,true,true);}dir_close(&container);
    if(!created){ce_bundle_free(&b);return fail(error,cap,"Cannot create an exclusive recovery workspace");}
    for(size_t i=0;ok&&i<b.count;i++)if(strncmp(b.entries[i].name,"workspace/",10)==0)ok=write_relative(&stage,b.entries[i].name+10,b.entries[i].data,b.entries[i].size);
    dir_close(&stage);ce_bundle_free(&b);if(!ok)return fail(error,cap,"Recovery write failed; original workspace remains active");
    snprintf(rel,sizeof rel,".notelab-restored/%s",name);char *r=copy(rel),*p=join(base,rel);if(!r||!p){free(r);free(p);return fail(error,cap,"Cannot allocate restored workspace path");}*relative=r;*root=p;return true;
}
bool ce_workspace_activate(const char *base,const char *relative,char *error,size_t cap){
    if(!relative_valid(relative))return fail(error,cap,"Invalid recovery activation path");Dir parent,control;
    if(!dir_open(&parent,base))return fail(error,cap,"Cannot open workspace base");
    if(*relative){Dir target;if(!open_relative(&parent,relative,&target)){dir_close(&parent);return fail(error,cap,"Recovered workspace is unavailable");}dir_close(&target);}
    bool ok=child_dir(&parent,".notelab-recovery",&control,true,false);dir_close(&parent);if(!ok)return fail(error,cap,"Cannot open recovery control directory");
    char *path=join(base,".notelab-recovery");NoteStore store={0};char value[2300];snprintf(value,sizeof value,"%s",relative);
    ok=path&&note_store_init(&store,path)==NOTE_OK;free(path);
    if(ok){NoteResult result=note_store_create(&store,"active.txt",value,strlen(value));if(result==NOTE_EXISTS)result=note_store_save(&store,"active.txt",value,strlen(value));ok=result==NOTE_OK;if(!ok)fail(error,cap,store.error);}
    note_store_destroy(&store);dir_close(&control);return ok?true:fail(error,cap,"Cannot save recovery selection; previous workspace remains active");
}
bool ce_workspace_resolve(const char *base,char **root,char *error,size_t cap){
    if(!base||!root)return fail(error,cap,"Invalid workspace base");Dir parent;if(!dir_open(&parent,base))return fail(error,cap,"Cannot open workspace base");
    Dir control;bool exists=child_dir(&parent,".notelab-recovery",&control,false,false);
    if(!exists){
#ifdef _WIN32
        DWORD why=GetLastError();bool absent=why==ERROR_FILE_NOT_FOUND||why==ERROR_PATH_NOT_FOUND;
#else
        bool absent=errno==ENOENT;
#endif
        dir_close(&parent);if(!absent)return fail(error,cap,"Recovery control directory is invalid");char *p=copy(base);if(!p)return fail(error,cap,"Cannot allocate workspace root");*root=p;return true;
    }
    /* A failed first activation may have created the control directory only. */
    bool missing=false;
#ifdef _WIN32
    wchar_t *marker_path=path_at(&control,"active.txt");
    if(marker_path){DWORD attrs=GetFileAttributesW(marker_path);DWORD why=GetLastError();missing=attrs==INVALID_FILE_ATTRIBUTES&&(why==ERROR_FILE_NOT_FOUND||why==ERROR_PATH_NOT_FOUND);free(marker_path);}
#else
    struct stat marker_stat;missing=fstatat(control.fd,"active.txt",&marker_stat,AT_SYMLINK_NOFOLLOW)<0&&errno==ENOENT;
#endif
    if(missing){dir_close(&control);dir_close(&parent);char *p=copy(base);if(!p)return fail(error,cap,"Cannot allocate workspace root");*root=p;return true;}
    unsigned char *bytes=NULL;size_t size=0;bool ok=read_file(&control,"active.txt",&bytes,&size);dir_close(&control);
    if(!ok){dir_close(&parent);return fail(error,cap,"Recovery selection cannot be read");}
    if(size>2048||memchr(bytes,0,size)){free(bytes);dir_close(&parent);return fail(error,cap,"Recovery selection is invalid");}
    char *relative=malloc(size+1);if(relative){memcpy(relative,bytes,size);relative[size]=0;}free(bytes);ok=relative&&relative_valid(relative);
    if(ok&&*relative){Dir target;ok=open_relative(&parent,relative,&target);if(ok)dir_close(&target);}dir_close(&parent);
    char *p=ok?(*relative?join(base,relative):copy(base)):NULL;free(relative);if(!p)return fail(error,cap,"Selected recovery workspace is invalid or unavailable");*root=p;return true;
}
