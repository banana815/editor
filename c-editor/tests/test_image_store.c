#include "image_store.h"
#include "image_publish.h"
#include "image_clipboard.h"
#include "bundle_archive.h"
#include "cJSON.h"
#include "image_fixtures.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static char error[256];
static void codec(const void *data,size_t size,const char *ext) {
    CeImageInfo info; unsigned char *pixels=NULL;
    assert(ce_image_decode(data,size,&info,&pixels,error,sizeof error));
    assert(info.width==2 && info.height==1 && !strcmp(info.extension,ext)); free(pixels);
    assert(!ce_image_decode(data,8,&info,NULL,error,sizeof error));
}
int main(int argc,char **argv) {
    assert(argc==2); codec(image_png,sizeof image_png,"png"); codec(image_jpeg,sizeof image_jpeg,"jpg"); codec(image_bmp,sizeof image_bmp,"bmp");
    unsigned char bomb[sizeof image_png]; memcpy(bomb,image_png,sizeof bomb); memset(bomb+16,255,8);
    assert(!ce_image_decode(bomb,sizeof bomb,NULL,NULL,error,sizeof error));
    assert(!ce_image_decode(image_png,CE_IMAGE_MAX_BYTES+1,NULL,NULL,error,sizeof error));
    CeImageStore store={0}; assert(ce_image_store_open(&store,argv[1])); char uri[83],same[83];
    assert(ce_image_store_add(&store,image_png,sizeof image_png,uri,NULL));
    assert(ce_image_store_add(&store,image_png,sizeof image_png,same,NULL) && !strcmp(uri,same));
    unsigned char *read=NULL; size_t size=0; assert(ce_image_store_read(&store,uri,&read,&size));
    assert(size==sizeof image_png && !memcmp(read,image_png,size)); free(read);
    assert(!ce_image_asset_name("https://example.com/image.png"));
    assert(!ce_image_asset_name("notelab-asset:../outside.png"));
    assert(!note_store_valid_name(ce_image_asset_name(uri)));
    assert(note_store_create_asset(&store.files,"../outside.png","bad",3)==NOTE_INVALID_NAME);
    char *node=ce_image_node_json(uri,"<alt> & 中文","Caption <script>",320); assert(node);
    CeDocument *doc=ce_document_new(); assert(doc && ce_document_insert_node(doc,0,node)); free(node);
    char *before=ce_document_export_json(doc);
    assert(!ce_document_publish(doc,CE_PUBLISH_HTML,NULL,NULL,error,sizeof error));
    char *html=ce_document_publish_images(doc,CE_PUBLISH_HTML,NULL,NULL,ce_image_publish_url,&store,error,sizeof error);
    if(!html) fprintf(stderr,"%s\n",error); assert(html);
    assert(strstr(html,"data:image/png;base64,") && strstr(html,"&lt;alt&gt; &amp; 中文") && strstr(html,"Caption &lt;script&gt;") && strstr(html,"width=\"320\"")); free(html);
    for(int format=0;format<2;format++) {
        unsigned char *zip=NULL; size_t zip_size=0; assert(ce_image_publish_bundle(doc,(CePublishFormat)format,NULL,NULL,&store,&zip,&zip_size,error,sizeof error));
        CeBundle bundle={0}; assert(ce_bundle_read(zip,zip_size,&bundle,error,sizeof error)); assert(bundle.count==2);
        bool article=false,asset=false;
        for(size_t i=0;i<bundle.count;i++) {
            const CeBundleEntry *entry=&bundle.entries[i];
            if(!strncmp(entry->name,"assets/",7)) { assert(entry->size==sizeof image_png && !memcmp(entry->data,image_png,entry->size)); asset=true; }
            else { char *text=malloc(entry->size+1); assert(text); memcpy(text,entry->data,entry->size);text[entry->size]=0;assert(strstr(text,"assets/") && !strstr(text,"notelab-asset:"));free(text);article=true; }
        }
        assert(article && asset); ce_bundle_free(&bundle); free(zip);
    }
    CeDocumentSlice *slice=NULL,*embedded=NULL,*local=NULL;
    assert(ce_document_slice_capture(doc,0,ce_document_length(doc),&slice,NULL,error,sizeof error));
    assert(ce_image_slice_transfer(&store,slice,true,&embedded,error,sizeof error));
    char target[2048];snprintf(target,sizeof target,"%s/target",argv[1]);CeImageStore destination={0};assert(ce_image_store_open(&destination,target));
    assert(ce_image_slice_transfer(&destination,embedded,false,&local,error,sizeof error));
    char *local_json=NULL;assert(ce_document_slice_json(local,&local_json,error,sizeof error));
    assert(!strstr(local_json,"notelabClipboardImageData"));free(local_json);
    assert(ce_image_store_read(&destination,uri,&read,&size) && size==sizeof image_png && !memcmp(read,image_png,size));free(read);
    ce_document_slice_free(slice);ce_document_slice_free(embedded);ce_document_slice_free(local);ce_image_store_close(&destination);
    char *after=ce_document_export_json(doc); assert(!strcmp(before,after));free(before);free(after);ce_document_free(doc);
    ce_image_store_close(&store); assert(ce_image_store_open(&store,argv[1]));assert(ce_image_store_read(&store,uri,&read,&size));free(read);
    char damaged[83];strcpy(damaged,uri);damaged[14]=damaged[14]=='0'?'1':'0';
    const char *name=ce_image_asset_name(damaged); NoteResult created=note_store_create_asset(&store.files,name,"bad",3);assert(created==NOTE_OK || created==NOTE_EXISTS);
    assert(!ce_image_store_read(&store,damaged,&read,&size));ce_image_store_close(&store);
    puts("Image codec, managed immutable assets, persistence, escaped publishing and complete ZIP round trips passed");return 0;
}
