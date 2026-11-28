#include "image_codec.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
/* stb_image's temporary allocations are bounded as well as its final pixels. */
typedef union { size_t size; long double alignment; void *pointer; unsigned char reserved[16]; } ImageAllocation;
static size_t image_allocated;
#define IMAGE_MEMORY_LIMIT ((size_t)128*1024*1024)
static void *image_malloc(size_t size) {
    if(size>IMAGE_MEMORY_LIMIT-image_allocated || size>SIZE_MAX-sizeof(ImageAllocation)) return NULL;
    ImageAllocation *a=malloc(sizeof(*a)+size); if(!a) return NULL;
    a->size=size; image_allocated+=size; return a+1;
}
static void image_free(void *p) { if(p) { ImageAllocation *a=(ImageAllocation*)p-1; image_allocated-=a->size; free(a); } }
static void *image_realloc(void *p,size_t size) {
    if(!p) return image_malloc(size);
    ImageAllocation *a=(ImageAllocation*)p-1; size_t old=a->size;
    if(size>IMAGE_MEMORY_LIMIT-(image_allocated-old) || size>SIZE_MAX-sizeof(*a)) return NULL;
    a=realloc(a,sizeof(*a)+size); if(!a) return NULL;
    image_allocated=image_allocated-old+size; a->size=size; return a+1;
}
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#define STBI_ONLY_BMP
#define STBI_NO_STDIO
#define STBI_NO_HDR
#define STBI_NO_LINEAR
#define STBI_MAX_DIMENSIONS 16384
#define STBI_MALLOC image_malloc
#define STBI_REALLOC image_realloc
#define STBI_FREE image_free
#include "../vendor/stb_image.h"
static uint32_t image_be32(const unsigned char *p) { return ((uint32_t)p[0]<<24)|((uint32_t)p[1]<<16)|((uint32_t)p[2]<<8)|p[3]; }
static bool image_png_complete(const unsigned char *b,size_t size) {
    size_t at=8; bool first=true,content=false;
    while(at<size) {
        if(size-at<12) return false;
        uint32_t n=image_be32(b+at); if(n>size-at-12) return false;
        if(first && (n!=13 || memcmp(b+at+4,"IHDR",4))) return false;
        first=false;
        uint32_t crc=UINT32_MAX;
        for(size_t i=at+4;i<at+8+n;i++) { crc^=b[i]; for(unsigned bit=0;bit<8;bit++) crc=(crc>>1)^((0u-(crc&1u))&0xedb88320u); }
        if((crc^UINT32_MAX)!=image_be32(b+at+8+n)) return false;
        if(!memcmp(b+at+4,"IDAT",4)) content=true;
        if(!memcmp(b+at+4,"IEND",4)) return !n && content && at+12==size;
        at+=(size_t)n+12;
    }
    return false;
}
bool ce_image_decode(const void *bytes,size_t size,CeImageInfo *info,unsigned char **pixels,char *error,size_t capacity) {
    const unsigned char *b=bytes; const char *extension=NULL; int w=0,h=0,n=0;
    if(bytes && size>=8 && !memcmp(b,"\x89PNG\r\n\x1a\n",8)) extension="png";
    else if(bytes && size>=3 && b[0]==255 && b[1]==216 && b[2]==255) extension="jpg";
    else if(bytes && size>=2 && b[0]=='B' && b[1]=='M') extension="bmp";
    if(!extension || size>CE_IMAGE_MAX_BYTES ||
       (!strcmp(extension,"png") && !image_png_complete(b,size)) ||
       (!strcmp(extension,"jpg") && (b[size-2]!=255 || b[size-1]!=217)) ||
       !stbi_info_from_memory(b,(int)size,&w,&h,&n) ||
       w<=0 || h<=0 || (size_t)w>CE_IMAGE_MAX_PIXELS/(size_t)h) goto failed;
    unsigned char *decoded=stbi_load_from_memory(b,(int)size,&w,&h,&n,4);
    if(!decoded) goto failed;
    unsigned char *copy=NULL;
    if(pixels) { size_t length=(size_t)w*(size_t)h*4; copy=malloc(length); if(copy) memcpy(copy,decoded,length); }
    stbi_image_free(decoded);
    if(pixels && !copy) goto failed;
    if(info) *info=(CeImageInfo){(unsigned)w,(unsigned)h,extension};
    if(pixels) *pixels=copy;
    if(error && capacity) *error=0;
    return true;
failed:
    if(error && capacity) snprintf(error,capacity,"图片无法解码或超出限制（PNG/JPEG/BMP，16 MiB、1600 万像素）");
    return false;
}
