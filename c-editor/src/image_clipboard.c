#include "image_clipboard.h"
#include "image_publish.h"
#include "cJSON.h"
#include "csl_json.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define PAYLOAD "notelabClipboardImageData"
static bool fail(char *error,size_t capacity,const char *message) { if(error && capacity) snprintf(error,capacity,"%s",message);return false; }
static bool contains_image(const cJSON *node,unsigned depth) {
    if(depth>128) return false; /* Already validated slice depth is bounded. */
    const cJSON *type=cJSON_GetObjectItemCaseSensitive(node,"type");
    if(cJSON_IsString(type) && !strcmp(type->valuestring,"image")) return true;
    const cJSON *content=cJSON_GetObjectItemCaseSensitive(node,"content"),*child;
    cJSON_ArrayForEach(child,content) if(contains_image(child,depth+1)) return true;
    return false;
}
bool ce_image_slice_has_images(const CeDocumentSlice *slice,bool *out,char *error,size_t capacity) {
    if(!out) return fail(error,capacity,"缺少图片检查输出");
    char *json=NULL;if(!ce_document_slice_json(slice,&json,error,capacity))return false;
    cJSON *root=cJSON_Parse(json);free(json);
    if(!root) return fail(error,capacity,"无法检查剪贴板图片");
    bool result=contains_image(root,0);cJSON_Delete(root);*out=result;return true;
}
static int value(unsigned char c) { if(c>='A'&&c<='Z') return c-'A';if(c>='a'&&c<='z')return c-'a'+26;if(c>='0'&&c<='9')return c-'0'+52;if(c=='+')return 62;if(c=='/')return 63;return -1; }
static unsigned char *decode(const char *uri,size_t *size) {
    if(!uri) return NULL;
    const char *encoded=NULL;
    if(!strncmp(uri,"data:image/png;base64,",22) || !strncmp(uri,"data:image/bmp;base64,",22)) encoded=uri+22;
    else if(!strncmp(uri,"data:image/jpeg;base64,",23)) encoded=uri+23;
    if(!encoded) return NULL;
    size_t n=strlen(encoded);if(!n || n%4 || n/4>CE_IMAGE_MAX_BYTES/3+1) return NULL;
    size_t length=n/4*3;if(encoded[n-1]=='=')--length;if(encoded[n-2]=='=')--length;if(length>CE_IMAGE_MAX_BYTES) return NULL;
    unsigned char *bytes=malloc(length?length:1);if(!bytes)return NULL;size_t at=0;
    for(size_t i=0;i<n;i+=4) {
        int a=value(encoded[i]),b=value(encoded[i+1]),c=encoded[i+2]=='='?0:value(encoded[i+2]),d=encoded[i+3]=='='?0:value(encoded[i+3]);
        bool pad2=encoded[i+2]=='=',pad1=encoded[i+3]=='=';
        if(a<0 || b<0 || c<0 || d<0 || (pad2 && !pad1) || ((pad1||pad2) && i+4!=n) || (pad2 && (b&15)) || (pad1 && !pad2 && (c&3))) { free(bytes);return NULL; }
        unsigned v=((unsigned)a<<18)|((unsigned)b<<12)|((unsigned)c<<6)|(unsigned)d;
        if(at<length)bytes[at++]=(unsigned char)(v>>16);if(at<length)bytes[at++]=(unsigned char)(v>>8);if(at<length)bytes[at++]=(unsigned char)v;
    }
    *size=length;return bytes;
}
static bool transfer(CeImageStore *store,cJSON *node,bool embedding,unsigned depth,size_t *budget,char *error,size_t capacity) {
    if(depth>128)return fail(error,capacity,"图片剪贴板层级过深");
    cJSON *type=cJSON_GetObjectItemCaseSensitive(node,"type");
    if(cJSON_IsString(type) && !strcmp(type->valuestring,"image")) {
        cJSON *attrs=cJSON_GetObjectItemCaseSensitive(node,"attrs"),*src=cJSON_GetObjectItemCaseSensitive(attrs,"src"),*payload=cJSON_GetObjectItemCaseSensitive(attrs,PAYLOAD);
        if(!cJSON_IsString(src) || !ce_image_asset_name(src->valuestring))return fail(error,capacity,"图片不含有效的本地资源引用");
        if(embedding) {
            if(payload)return fail(error,capacity,"图片包含冲突的剪贴板保留属性");
            char *url=ce_image_publish_url(store,node,error,capacity);if(!url)return false;
            size_t payload_size=strlen(url)+64;
            if(payload_size>CE_DOCUMENT_SLICE_MAX_BYTES-*budget) { free(url); return fail(error,capacity,"图片剪贴板总量超过 16 MiB"); }
            *budget+=payload_size; bool ok=cJSON_AddStringToObject(attrs,PAYLOAD,url)!=NULL;free(url);if(!ok)return fail(error,capacity,"内存不足，未复制图片");
        } else if(payload) {
            size_t size=0;unsigned char *bytes=cJSON_IsString(payload)?decode(payload->valuestring,&size):NULL;char uri[83];
            if(!bytes)return fail(error,capacity,"剪贴板图片数据无效");
            bool ok=ce_image_store_add(store,bytes,size,uri,NULL);free(bytes);
            if(!ok)return fail(error,capacity,store->error);
            if(strcmp(uri,src->valuestring))return fail(error,capacity,"剪贴板图片校验与引用不匹配");
            cJSON_DeleteItemFromObjectCaseSensitive(attrs,PAYLOAD);
        } else {
            unsigned char *bytes=NULL;size_t size=0;bool ok=ce_image_store_read(store,src->valuestring,&bytes,&size);free(bytes);
            if(!ok)return fail(error,capacity,"目标文章库缺少此图片，请从来源重新复制");
        }
    }
    cJSON *content=cJSON_GetObjectItemCaseSensitive(node,"content"),*child;
    cJSON_ArrayForEach(child,content)if(!transfer(store,child,embedding,depth+1,budget,error,capacity))return false;
    return true;
}
bool ce_image_slice_transfer(CeImageStore *store,const CeDocumentSlice *slice,bool embedding,CeDocumentSlice **out,char *error,size_t capacity) {
    char *json=NULL;if(!ce_document_slice_json(slice,&json,error,capacity))return false;
    size_t budget=strlen(json); cJSON *root=cJSON_Parse(json);free(json);bool ok=root && transfer(store,root,embedding,0,&budget,error,capacity);
    json=ok?ce_csl_json_print_exact_with_limit(root,CE_DOCUMENT_SLICE_MAX_BYTES):NULL;cJSON_Delete(root);
    if(!json) { if(error && capacity && *error) return false; return fail(error,capacity,"无法准备图片剪贴板"); }
    ok=ce_document_slice_parse(json,strlen(json),out,error,capacity);cJSON_free(json);return ok;
}
