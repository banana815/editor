#include "image_store.h"
#include "citation_digest.h"
#include "cJSON.h"
#include "csl_json.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static bool fail(CeImageStore *s,const char *message) { snprintf(s->error,sizeof s->error,"%s",message); return false; }
const char *ce_image_asset_name(const char *uri) {
    return uri && !strncmp(uri,"notelab-asset:",14) && note_store_valid_asset_name(uri+14)?uri+14:NULL;
}
bool ce_image_store_open(CeImageStore *s,const char *root) {
    if(!s || !root || s->files.internal) return false;
    size_t n=strlen(root); char *path=malloc(n+40); if(!path) return fail(s,"内存不足");
    snprintf(path,n+40,"%s/.notelab-writing",root);
    NoteStore parent={0}; NoteResult r=note_store_init(&parent,path); note_store_destroy(&parent);
    if(r==NOTE_OK) { strcat(path,"/assets"); r=note_store_init(&s->files,path); }
    free(path); if(r!=NOTE_OK) return fail(s,"无法打开图片资源目录");
    s->revision++; *s->error=0; return true;
}
void ce_image_store_close(CeImageStore *s) { if(s) { note_store_destroy(&s->files); memset(s,0,sizeof *s); } }
static bool digest(const void *bytes,size_t size,char hex[65]) {
    CeCitationDigest d; ce_citation_digest_init(&d);
    return ce_citation_digest_update(&d,bytes,size,NULL,0) && ce_citation_digest_final_hex(&d,hex,NULL,0);
}
bool ce_image_store_read(CeImageStore *s,const char *uri,unsigned char **bytes,size_t *size) {
    if(!s || !bytes || !size) return false;
    const char *name=ce_image_asset_name(uri); if(!name) return fail(s,"图片不是本地受管资源");
    char *data=NULL; size_t length=0;
    if(note_store_read_asset(&s->files,name,&data,&length)!=NOTE_OK) return fail(s,s->files.error);
    char hex[65]; if(!digest(data,length,hex) || strncmp(name,hex,64)) { free(data); return fail(s,"图片资源校验失败"); }
    *bytes=(unsigned char*)data; *size=length; *s->error=0; return true;
}
bool ce_image_store_add(CeImageStore *s,const void *bytes,size_t size,char uri[83],CeImageInfo *info) {
    if(!s || !uri) return false;
    CeImageInfo result; if(!ce_image_decode(bytes,size,&result,NULL,s->error,sizeof s->error)) return false;
    char hex[65],name[69],source[83]; if(!digest(bytes,size,hex)) return fail(s,"无法校验图片");
    snprintf(name,sizeof name,"%s.%s",hex,result.extension); snprintf(source,sizeof source,"notelab-asset:%s",name);
    NoteResult r=note_store_create_asset(&s->files,name,bytes,size);
    if(r==NOTE_EXISTS) {
        unsigned char *existing=NULL; size_t count=0;
        bool same=ce_image_store_read(s,source,&existing,&count) && count==size && !memcmp(existing,bytes,size);
        free(existing); if(!same) return fail(s,"已有图片资源损坏，未覆盖");
    } else if(r!=NOTE_OK) return fail(s,s->files.error);
    memcpy(uri,source,sizeof source); if(info) *info=result; s->revision++; *s->error=0; return true;
}
char *ce_image_node_json(const char *uri,const char *alt,const char *caption,unsigned width) {
    if(!ce_image_asset_name(uri) || !alt || !caption || width>2400 || strlen(alt)>4096 || strlen(caption)>4096) return NULL;
    cJSON *node=cJSON_CreateObject(),*attrs=cJSON_CreateObject(); char *json=NULL;
    if(node && attrs && cJSON_AddStringToObject(node,"type","image") &&
       cJSON_AddStringToObject(attrs,"src",uri) && cJSON_AddStringToObject(attrs,"alt",alt) &&
       cJSON_AddStringToObject(attrs,"caption",caption) && cJSON_AddNumberToObject(attrs,"width",width) &&
       cJSON_AddItemToObject(node,"attrs",attrs)) { attrs=NULL; char *exact=ce_csl_json_print_exact_with_limit(node,16u*1024u*1024u);
        if(exact) { size_t n=strlen(exact)+1; json=malloc(n); if(json) memcpy(json,exact,n); cJSON_free(exact); } }
    cJSON_Delete(attrs); cJSON_Delete(node); return json;
}
