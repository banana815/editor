#include "bundle_archive.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static unsigned get32(const unsigned char *p){return (unsigned)p[0]|(unsigned)p[1]<<8|(unsigned)p[2]<<16|(unsigned)p[3]<<24;}
static void put32(unsigned char *p,unsigned v){for(int i=0;i<4;i++)p[i]=(unsigned char)(v>>(8*i));}
int main(void){
    char error[256];const unsigned char binary[]={0,1,2,255};CeBundleEntry entries[]={{"article.html",(const unsigned char *)"hello",5},{"images/中文.png",binary,sizeof binary},{"empty",NULL,0}};unsigned char *zip=NULL;size_t size=0;
    assert(ce_bundle_write(entries,3,&zip,&size,error,sizeof error));CeBundle b={0};assert(ce_bundle_read(zip,size,&b,error,sizeof error));assert(b.count==3&&!strcmp(b.entries[1].name,"images/中文.png")&&b.entries[1].size==4&&!memcmp(b.entries[1].data,binary,4));ce_bundle_free(&b);
    const char *bad[]={"../outside","/absolute","C:/x","x//a","x/../a","x/./a","x/a ","x/a.","CON.txt","x/com1.png","x\\bad","x/a:","x/","bad\xc0\x80"};
    for(size_t i=0;i<sizeof bad/sizeof *bad;i++)assert(!ce_bundle_valid_path(bad[i]));
    assert(ce_bundle_valid_path(".notelab-writing/assets/a.png"));
    for(size_t n=0;n<size;n++){assert(!ce_bundle_read(zip,n,&b,error,sizeof error));assert(!b.entries&&!b.count);}
    zip[30+strlen(entries[0].name)]^=1;assert(!ce_bundle_read(zip,size,&b,error,sizeof error));zip[30+strlen(entries[0].name)]^=1;
    unsigned char *sentinel=(unsigned char *)1;size_t unchanged=7;CeBundleEntry duplicate[]={{"A/file",binary,4},{"a/FILE",binary,4}};assert(!ce_bundle_write(duplicate,2,&sentinel,&unchanged,error,sizeof error)&&sentinel==(void *)1&&unchanged==7);
    duplicate[1].name="a/file/child";assert(!ce_bundle_write(duplicate,2,&sentinel,&unchanged,error,sizeof error));
    duplicate[0].name="é.txt";duplicate[1].name="e\xcc\x81.txt";assert(!ce_bundle_write(duplicate,2,&sentinel,&unchanged,error,sizeof error));
    duplicate[0].name="Straße.txt";duplicate[1].name="STRASSE.txt";assert(!ce_bundle_write(duplicate,2,&sentinel,&unchanged,error,sizeof error));
    CeBundleEntry huge={"big",binary,CE_BUNDLE_MAX_BYTES};assert(!ce_bundle_write(&huge,1,&sentinel,&unchanged,error,sizeof error));assert(!ce_bundle_write(entries,CE_BUNDLE_MAX_ENTRIES+1,&sentinel,&unchanged,error,sizeof error));
    /* Central/local name disagreement, encryption and unsupported compression. */
    zip[6]|=1;assert(!ce_bundle_read(zip,size,&b,error,sizeof error));zip[6]&=(unsigned char)~1;
    zip[8]=8;assert(!ce_bundle_read(zip,size,&b,error,sizeof error));zip[8]=0;
    size_t central=get32(zip+size-6);unsigned attrs=get32(zip+central+38);put32(zip+central+38,0120777u<<16);assert(!ce_bundle_read(zip,size,&b,error,sizeof error));put32(zip+central+38,attrs);
    unsigned offset=get32(zip+central+42);put32(zip+central+42,1);assert(!ce_bundle_read(zip,size,&b,error,sizeof error));put32(zip+central+42,offset);
    unsigned length=get32(zip+central+24);put32(zip+central+24,0xffffffff);assert(!ce_bundle_read(zip,size,&b,error,sizeof error));put32(zip+central+24,length);
    assert(ce_bundle_read(zip,size,&b,error,sizeof error));ce_bundle_free(&b);free(zip);
    assert(ce_bundle_write(NULL,0,&zip,&size,error,sizeof error)&&size==22);assert(ce_bundle_read(zip,size,&b,error,sizeof error)&&b.count==0);ce_bundle_free(&b);free(zip);
    CeBundleEntry golden={"crc.txt",(const unsigned char *)"123456789",9};assert(ce_bundle_write(&golden,1,&zip,&size,error,sizeof error));assert(get32(zip+14)==0xcbf43926u&&zip[12]==33);free(zip);
    puts("ZIP roundtrip, CRC, truncation, path safety and atomic output passed");return 0;
}
