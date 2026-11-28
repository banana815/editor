#define SDL_MAIN_HANDLED 1
#include "../src/app.c"
#undef main
#include "image_fixtures.h"
#include <assert.h>
static size_t image_index(App *a) {
    CeBlockInfo b;
    for(size_t i=0;i<ce_document_block_count(a->doc);i++) if(ce_document_block_info(a->doc,i,&b) && b.kind==CE_BLOCK_IMAGE) { a->edit->cursor=(int)b.start;return i; }
    assert(!"Expected image block"); return 0;
}
static void unchanged(App *a,const char *expected) {
    char *actual=ce_document_export_json(a->doc);assert(actual && !strcmp(actual,expected));free(actual);
}
int main(int argc,char **argv) {
    assert(argc>=3);void *state=NULL;char *args[]={"image-test","--notes",argv[1],"--font",argv[2]};
    assert(SDL_AppInit(&state,5,args)==SDL_APP_CONTINUE);App *a=state;char error[256];
    assert(ce_document_import(a->doc,"Draft",CE_FORMAT_TEXT,error,sizeof error));
    free(a->active);a->active=copy_string("images.json");sync_view(a);a->edit->cursor=5;EditBox_SetFocus(a->edit,true);
    /* A pure-text render/export must not require writable image storage. */
    assert(!a->images.ready);char unavailable_root[2048];snprintf(unavailable_root,sizeof unavailable_root,"%s/no-assets.txt",argv[1]);
    assert(SDL_SaveFile(unavailable_root,"fixture",7));char *real_root=a->notes_root;a->notes_root=unavailable_root;
    images_attach(a,a->rich);char *plain_html=publish_prepare(a,CE_PUBLISH_HTML);assert(plain_html && !a->images.ready);free(plain_html);
    a->preview.open=true;assert(preview_prepare(a) && a->preview.valid && !a->images.ready);preview_reset(a);a->notes_root=real_root;
    char *before=ce_document_export_json(a->doc);assert(images_begin(a));ce_document_mark_saved(a->doc);
    assert(images_receive(a,image_png,sizeof image_png));size_t index=image_index(a);
    assert(ce_document_can_undo(a->doc));action(a,ACT_UNDO);unchanged(a,before);action(a,ACT_REDO);free(before);
    index=image_index(a);images_property_prompt(a,0);assert(a->prompt_mode==PROMPT_IMAGE_ALT);ce_document_mark_saved(a->doc);
    assert(images_prompt_accept(a,"中文 <alt>"));
    image_index(a);images_property_prompt(a,1);assert(images_prompt_accept(a,"A caption"));
    image_index(a);images_property_prompt(a,2);assert(images_prompt_accept(a,"320"));
    char *node=ce_document_block_json(a->doc,index);assert(node && strstr(node,"中文 <alt>") && strstr(node,"A caption") && strstr(node,"320"));free(node);
    char *html=publish_prepare(a,CE_PUBLISH_HTML);assert(html && strstr(html,"data:image/png;base64,") && strstr(html,"&lt;alt&gt;"));free(html);
    images_attach(a,a->rich);CeRichTheme theme=ce_rich_theme_default(false);
    assert(ce_rich_layout_update(a->rich,a->doc,400,&theme));assert(a->images.cache_bytes>0);
    before=ce_document_export_json(a->doc);assert(images_begin(a));
    assert(!images_receive(a,"broken",6));unchanged(a,before);free(before);
    assert(images_begin(a));assert(ce_document_insert(a->doc,0,"Changed "));
    before=ce_document_export_json(a->doc);assert(!images_receive(a,image_jpeg,sizeof image_jpeg));unchanged(a,before);free(before);
    image_index(a);images_property_prompt(a,2);before=ce_document_export_json(a->doc);
    assert(images_prompt_accept(a,"99999"));unchanged(a,before);free(before);a->prompt_mode=PROMPT_NONE;
    free(a->active);a->active=NULL;SDL_AppQuit(a,SDL_APP_SUCCESS);
    puts("Image insertion undo, metadata editing, actual rendering, snapshot guards and safe publication passed");return 0;
}
