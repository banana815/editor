#include "image_publish.h"
#include "bundle_archive.h"
#include "cJSON.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static const char *image_uri(const cJSON *node) {
    const cJSON *a=cJSON_GetObjectItemCaseSensitive(node,"attrs"),*v=cJSON_GetObjectItemCaseSensitive(a,"src");
    return cJSON_IsString(v)?v->valuestring:NULL;
}
static bool read_image(CeImageStore *store,const cJSON *node,unsigned char **bytes,size_t *size,char *error,size_t capacity) {
    if(!ce_image_store_read(store,image_uri(node),bytes,size)) { if(error && capacity) snprintf(error,capacity,"%s",store->error); return false; }
    CeImageInfo info;
    if(!ce_image_decode(*bytes,*size,&info,NULL,error,capacity)) { free(*bytes); *bytes=NULL; return false; }
    const char *name=ce_image_asset_name(image_uri(node));
    if(strcmp(name+65,info.extension)) { free(*bytes); *bytes=NULL; if(error && capacity) snprintf(error,capacity,"图片扩展名与内容不符"); return false; }
    return true;
}
char *ce_image_publish_url(void *context,const cJSON *node,char *error,size_t capacity) {
    CeImageStore *store=context; unsigned char *data=NULL; size_t size=0;
    if(!read_image(store,node,&data,&size,error,capacity)) return NULL;
    const char *name=ce_image_asset_name(image_uri(node));
    const char *mime=!strcmp(name+65,"jpg")?"jpeg":name+65;
    size_t length=((size+2)/3)*4; char *url=malloc(length+32); if(!url) { free(data); return NULL; }
    int n=snprintf(url,length+32,"data:image/%s;base64,",mime); size_t at=(size_t)n;
    static const char alphabet[]="ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    for(size_t i=0;i<size;i+=3) {
        unsigned v=(unsigned)data[i]<<16; if(i+1<size) v|=(unsigned)data[i+1]<<8; if(i+2<size) v|=data[i+2];
        url[at++]=alphabet[(v>>18)&63]; url[at++]=alphabet[(v>>12)&63];
        url[at++]=i+1<size?alphabet[(v>>6)&63]:'='; url[at++]=i+2<size?alphabet[v&63]:'=';
    }
    url[at]=0; free(data); return url;
}
typedef struct { CeImageStore *store; CeBundleEntry *entries; size_t count,total; } ImageBundle;
static char *bundle_url(void *context,const cJSON *node,char *error,size_t capacity) {
    ImageBundle *bundle=context; const char *name=ce_image_asset_name(image_uri(node));
    if(!name) { if(error && capacity) snprintf(error,capacity,"图片不是本地受管资源"); return NULL; }
    char path[80]; snprintf(path,sizeof path,"assets/%s",name);
    for(size_t i=0;i<bundle->count;i++) if(!strcmp(bundle->entries[i].name,path)) { char *copy=malloc(strlen(path)+1); if(copy) strcpy(copy,path); return copy; }
    unsigned char *bytes=NULL; size_t size=0;
    if(!read_image(bundle->store,node,&bytes,&size,error,capacity)) return NULL;
    if(bundle->count>=9999 || size>120u*1024u*1024u-bundle->total) { free(bytes); if(error && capacity) snprintf(error,capacity,"图片打包总量过大"); return NULL; }
    char *owned=malloc(strlen(path)+1),*url=malloc(strlen(path)+1);
    CeBundleEntry *entries=realloc(bundle->entries,(bundle->count+1)*sizeof *entries);
    if(entries) bundle->entries=entries;
    if(!owned || !url || !entries) { free(bytes); free(owned); free(url); return NULL; }
    strcpy(owned,path); strcpy(url,path);
    bundle->entries[bundle->count++]=(CeBundleEntry){owned,bytes,size}; bundle->total+=size; return url;
}
bool ce_image_publish_bundle(const CeDocument *doc,CePublishFormat format,CePublishAtomHtml atom,void *context,CeImageStore *store,
                              unsigned char **out,size_t *size,char *error,size_t capacity) {
    ImageBundle bundle={.store=store};
    char *source=ce_document_publish_images(doc,format,atom,context,bundle_url,&bundle,error,capacity);
    bool ok=false;
    if(source) {
        if(format==CE_PUBLISH_HTML) { char *full=ce_document_publish_html_page(source);free(source);source=full; }
        CeBundleEntry *entries=source?realloc(bundle.entries,(bundle.count+1)*sizeof *entries):NULL;
        if(entries) { bundle.entries=entries; bundle.entries[bundle.count]=(CeBundleEntry){format==CE_PUBLISH_HTML?"article.html":"article.md",(unsigned char*)source,strlen(source)};
            ok=ce_bundle_write(bundle.entries,bundle.count+1,out,size,error,capacity); }
    }
    free(source); for(size_t i=0;i<bundle.count;i++) { free((void*)bundle.entries[i].name); free((void*)bundle.entries[i].data); } free(bundle.entries); return ok;
}
