#include "bundle_archive.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "utf8proc.h"
static bool fail(char *e,size_t n,const char *s) { if(e&&n) snprintf(e,n,"%s",s); return false; }
static unsigned lower(unsigned c) { return c>='A'&&c<='Z'?c+32:c; }
static bool utf8(const char *s) {
    const unsigned char *p=(const unsigned char *)s;
    while(*p) { unsigned c=*p++,n,min;
        if(c<128) continue;
        if(c>=194&&c<=223) {c&=31;n=1;min=128;} else if(c>=224&&c<=239) {c&=15;n=2;min=2048;}
        else if(c>=240&&c<=244) {c&=7;n=3;min=65536;} else return false;
        while(n--) {if((*p&192)!=128)return false;c=(c<<6)|(*p++&63);}
        if(c<min||c>0x10ffff||(c>=0xd800&&c<=0xdfff))return false;
    } return true;
}
bool ce_bundle_valid_path(const char *s) {
    if(!s||!*s||strlen(s)>2048||!utf8(s))return false;
    const char *p=s;
    while(*p) { const char *a=p;while(*p&&*p!='/')p++;size_t n=(size_t)(p-a);
        if(!n||n>255||(n==1&&*a=='.')||(n==2&&a[0]=='.'&&a[1]=='.')||a[n-1]=='.'||a[n-1]==' ')return false;
        char stem[256];size_t k=0;
        for(size_t i=0;i<n;i++) {unsigned char c=(unsigned char)a[i];if(c<32||c==127||strchr("\\:*?\"<>|",c))return false;}
        while(k<n&&a[k]!='.') {stem[k]=(char)lower((unsigned char)a[k]);k++;}while(k&&stem[k-1]==' ')k--;stem[k]=0;
        if(!strcmp(stem,"con")||!strcmp(stem,"prn")||!strcmp(stem,"aux")||!strcmp(stem,"nul")||!strcmp(stem,"clock$")||!strcmp(stem,"conin$")||!strcmp(stem,"conout$"))return false;
        if((k==4&&stem[3]>='1'&&stem[3]<='9')||(k==5&&(unsigned char)stem[3]==194&&((unsigned char)stem[4]==185||(unsigned char)stem[4]==178||(unsigned char)stem[4]==179)))
            if(!memcmp(stem,"com",3)||!memcmp(stem,"lpt",3))return false;
        if(*p&&!*++p)return false;
    } return true;
}
static bool collision(const char *a,const char *b) {
    while(*a&&*b&&lower((unsigned char)*a)==lower((unsigned char)*b)){a++;b++;}
    return (!*a&&(!*b||*b=='/'))||(!*b&&*a=='/');
}
/* Sort separators before other characters, making a file and its first
 * descendant adjacent even when a sibling named "file-other" exists. */
static int path_compare(const void *left,const void *right){const unsigned char *a=*(const unsigned char *const *)left,*b=*(const unsigned char *const *)right;while(*a&&*a==*b){a++;b++;}unsigned x=*a=='/'?1:*a?*a+2:0,y=*b=='/'?1:*b?*b+2:0;return x<y?-1:x>y?1:0;}
static bool unique_paths(const CeBundleEntry *entries,size_t count){
    char **keys=calloc(count?count:1,sizeof *keys);if(!keys)return false;bool ok=true;
    for(size_t i=0;i<count;i++){utf8proc_uint8_t *key=NULL;utf8proc_ssize_t n=utf8proc_map((const utf8proc_uint8_t *)entries[i].name,0,&key,UTF8PROC_NULLTERM|UTF8PROC_STABLE|UTF8PROC_COMPOSE|UTF8PROC_CASEFOLD);if(n<0){free(key);ok=false;break;}keys[i]=(char *)key;}
    if(ok){qsort(keys,count,sizeof *keys,path_compare);for(size_t i=1;i<count;i++)if(collision(keys[i-1],keys[i])){ok=false;break;}}
    for(size_t i=0;i<count;i++)free(keys[i]);free(keys);return ok;
}
static uint32_t crc32(const unsigned char *p,size_t n) {uint32_t c=0xffffffffu;while(n--){c^=*p++;for(int i=0;i<8;i++)c=(c>>1)^(0xedb88320u& (0u-(c&1u)));}return ~c;}
static uint16_t r16(const unsigned char *p){return (uint16_t)(p[0]|p[1]<<8);}
static uint32_t r32(const unsigned char *p){return (uint32_t)p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24;}
static void w16(unsigned char *p,unsigned v){p[0]=(unsigned char)v;p[1]=(unsigned char)(v>>8);}
static void w32(unsigned char *p,uint32_t v){w16(p,v);w16(p+2,v>>16);}
static bool validate(const CeBundleEntry *entries,size_t count,size_t *total,char *e,size_t cap) {
    size_t n=22;
    if(count>CE_BUNDLE_MAX_ENTRIES||(count&&!entries))return fail(e,cap,"Too many archive entries");
    for(size_t i=0;i<count;i++) {
        if(!ce_bundle_valid_path(entries[i].name)||(!entries[i].data&&entries[i].size))return fail(e,cap,"Invalid archive path or data");
        size_t overhead=76+2*strlen(entries[i].name);
        if(overhead>CE_BUNDLE_MAX_BYTES-n||entries[i].size>CE_BUNDLE_MAX_BYTES-n-overhead)return fail(e,cap,"Archive exceeds 128 MiB");
        n+=overhead+entries[i].size;
    }if(!unique_paths(entries,count))return fail(e,cap,"Archive has duplicate, normalized, or conflicting paths");*total=n;return true;
}
bool ce_bundle_write(const CeBundleEntry *entries,size_t count,unsigned char **out,size_t *size,char *e,size_t cap) {
    size_t total;if(!out||!size||!validate(entries,count,&total,e,cap))return false;
    unsigned char *b=calloc(total,1);uint32_t *offsets=calloc(count?count:1,sizeof *offsets),*crcs=calloc(count?count:1,sizeof *crcs);
    if(!b||!offsets||!crcs){free(b);free(offsets);free(crcs);return fail(e,cap,"Cannot allocate archive");}
    size_t p=0;
    for(size_t i=0;i<count;i++) {size_t len=strlen(entries[i].name);offsets[i]=(uint32_t)p;crcs[i]=crc32(entries[i].data,entries[i].size);
        w32(b+p,0x04034b50);w16(b+p+4,20);w16(b+p+6,0x800);w16(b+p+12,33);w32(b+p+14,crcs[i]);w32(b+p+18,(uint32_t)entries[i].size);w32(b+p+22,(uint32_t)entries[i].size);w16(b+p+26,(unsigned)len);
        memcpy(b+p+30,entries[i].name,len);if(entries[i].size)memcpy(b+p+30+len,entries[i].data,entries[i].size);p+=30+len+entries[i].size;
    }
    size_t central=p;
    for(size_t i=0;i<count;i++){size_t len=strlen(entries[i].name);w32(b+p,0x02014b50);w16(b+p+4,20);w16(b+p+6,20);w16(b+p+8,0x800);w16(b+p+14,33);w32(b+p+16,crcs[i]);w32(b+p+20,(uint32_t)entries[i].size);w32(b+p+24,(uint32_t)entries[i].size);w16(b+p+28,(unsigned)len);w32(b+p+42,offsets[i]);memcpy(b+p+46,entries[i].name,len);p+=46+len;}
    w32(b+p,0x06054b50);w16(b+p+8,(unsigned)count);w16(b+p+10,(unsigned)count);w32(b+p+12,(uint32_t)(p-central));w32(b+p+16,(uint32_t)central);
    free(offsets);free(crcs);*out=b;*size=total;return true;
}
void ce_bundle_free(CeBundle *b){if(!b)return;for(size_t i=0;i<b->count;i++){free((void *)b->entries[i].name);free((void *)b->entries[i].data);}free(b->entries);memset(b,0,sizeof *b);}
bool ce_bundle_read(const void *bytes,size_t size,CeBundle *out,char *e,size_t cap) {
    if(!bytes||!out||size<22||size>CE_BUNDLE_MAX_BYTES)return fail(e,cap,"Invalid ZIP size");
    const unsigned char *b=bytes,*end=b+size-22;size_t count=r16(end+10),central=r32(end+16),central_size=r32(end+12);
    if(r32(end)!=0x06054b50||r16(end+4)||r16(end+6)||r16(end+8)!=count||r16(end+20)||count>CE_BUNDLE_MAX_ENTRIES||central>size-22||central_size!=size-22-central)return fail(e,cap,"Invalid ZIP directory");
    CeBundle next={0};next.entries=calloc(count?count:1,sizeof *next.entries);if(!next.entries)return fail(e,cap,"Cannot allocate ZIP directory");
    size_t p=central,local=0;bool ok=true;
    for(size_t i=0;ok&&i<count;i++) {
        if(p>size-22||size-22-p<46){ok=false;break;}
        const unsigned char *c=b+p;size_t len=r16(c+28),n=r32(c+24),at=r32(c+42);
        if(r32(c)!=0x02014b50||r16(c+6)>20||(r16(c+8)!=0x800&&r16(c+8)!=0)||r16(c+10)||r16(c+30)||r16(c+32)||r16(c+34)||r32(c+20)!=n||r32(c+38)&0xf0000010u||!len||len>2048||len>size-22-p-46||at!=local||local>central||central-local<30){ok=false;break;}
        const unsigned char *h=b+local;
        if(r32(h)!=0x04034b50||r16(h+4)>20||r16(h+6)!=r16(c+8)||r16(h+8)||r32(h+14)!=r32(c+16)||r32(h+18)!=n||r32(h+22)!=n||r16(h+26)!=len||r16(h+28)||len>central-local-30||n>central-local-30-len||memcmp(h+30,c+46,len)||memchr(c+46,0,len)){ok=false;break;}
        char *name=malloc(len+1);unsigned char *data=malloc(n?n:1);
        if(!name||!data){free(name);free(data);ok=false;break;}memcpy(name,c+46,len);name[len]=0;if(n)memcpy(data,h+30+len,n);
        next.entries[next.count++]=(CeBundleEntry){name,data,n};
        if(!ce_bundle_valid_path(name)||crc32(data,n)!=r32(c+16)){ok=false;break;}
        p+=46+len;local+=30+len+n;
    }
    if(!ok||p!=size-22||local!=central||!unique_paths(next.entries,next.count)){ce_bundle_free(&next);return fail(e,cap,"ZIP is damaged, unsupported, or contains unsafe paths");}
    *out=next;return true;
}
