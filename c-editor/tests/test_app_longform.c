#define SDL_MAIN_HANDLED 1
#include "../src/app.c"
#undef main
#include "image_fixtures.h"
#include <assert.h>
#ifdef _WIN32
#include <crtdbg.h>
#endif
static void pointer(App *a,Uint32 type,float x,float y) {
    SDL_Event event={0}; event.type=type;
    if(type==SDL_EVENT_MOUSE_MOTION) { event.motion.x=x; event.motion.y=y; event.motion.state=SDL_BUTTON_LMASK; }
    else { event.button.x=x; event.button.y=y; event.button.button=SDL_BUTTON_LEFT; }
    assert(SDL_AppEvent(a,&event)==SDL_APP_CONTINUE);
}
static void click(App *a,float x,float y) { pointer(a,SDL_EVENT_MOUSE_BUTTON_DOWN,x,y); pointer(a,SDL_EVENT_MOUSE_BUTTON_UP,x,y); }
static void screenshot(App *a,const char *root,const char *name) {
    char path[2048]; SDL_snprintf(path,sizeof path,"%s/%s.bmp",root,name);
    a->screenshot=path; a->frames=2; draw(a); a->screenshot=NULL;
    size_t size=0; void *bytes=SDL_LoadFile(path,&size); assert(bytes && size>1000); SDL_free(bytes);
}
static void preview_regressions(App *a,const char *root,const char *uri) {
    /* Link coordinates must be transformed through phone scaling and scrolling;
     * blank space on the same row is not a link hit. */
    size_t start,end; assert(ce_document_find(a->preview.document,"查看文章链接",0,false,&start,&end));
    SDL_FRect caret; assert(ce_rich_layout_caret(a->preview.layout,start,&caret));
    a->preview.scroll=SDL_max(0,caret.y-100); draw(a);
    float x=a->preview.viewport.x+(caret.x+29)*a->preview.scale;
    float y=a->preview.viewport.y+(caret.y+24-a->preview.scroll+caret.h*.5f)*a->preview.scale;
    assert(contains(a->preview.viewport,x,y)); click(a,x,y); assert(strstr(a->status,"https://example.com/article"));
    message(a,"blank preview space"); click(a,a->preview.viewport.x+a->preview.viewport.w-2,y);
    assert(!strcmp(a->status,"blank preview space"));
    SDL_Texture *texture=a->preview.texture; float scale=a->preview.scale;
    a->preview.scale=0.000001f; preview_draw(a); assert(a->preview.texture==texture); a->preview.scale=scale;
    SDL_Event right={0}; right.type=SDL_EVENT_MOUSE_BUTTON_DOWN; right.button.button=SDL_BUTTON_RIGHT;
    right.button.x=a->preview.buttons[3].x+5; right.button.y=a->preview.buttons[3].y+5;
    assert(preview_event(a,&right) && a->preview.open);

    char *node_json=ce_image_node_json(uri,"alt","caption",300); assert(node_json);
    cJSON *node=cJSON_Parse(node_json); free(node_json); char error[256];
    char *url=preview_image_url(&a->images.store,node,error,sizeof error);
    assert(url && !strncmp(url,"assets/",7) && strlen(url)<80 && !strstr(url,"base64")); free(url);
    cJSON_Delete(node);

    /* Same document identity and revision, different asset root. A previous
     * successful preview must not hide missing assets in the new workspace. */
    char *old_root=a->notes_root; char other[2200]; SDL_snprintf(other,sizeof other,"%s/preview-other-library",root);
    images_destroy(a); a->notes_root=copy_string(other); assert(a->notes_root);
    uint64_t revision=ce_document_revision(a->doc);
    assert(!preview_prepare(a) && !a->preview.valid && *a->preview.error);
    assert(ce_document_revision(a->doc)==revision);
    char added[83]; assert(ce_image_store_add(&a->images.store,image_png,sizeof image_png,added,NULL) && !strcmp(added,uri));
    assert(preview_prepare(a) && a->preview.valid); /* resource revision retries cached failure */
    images_destroy(a); free(a->notes_root); a->notes_root=old_root;
    assert(preview_prepare(a) && a->preview.valid);
    float old_size=TTF_GetFontSize(a->font); assert(TTF_SetFontSize(a->font,old_size+1));
    assert(preview_prepare(a) && a->preview.font_size==old_size+1);
    assert(TTF_SetFontSize(a->font,old_size)); assert(preview_prepare(a));
}
int main(int argc,char **argv) {
#ifdef _WIN32
    _CrtSetReportMode(_CRT_ASSERT,_CRTDBG_MODE_FILE); _CrtSetReportFile(_CRT_ASSERT,_CRTDBG_FILE_STDERR);
#endif
    assert(argc>=3); char root[2048]; SDL_snprintf(root,sizeof root,"%s/run-%llu",argv[1],(unsigned long long)SDL_GetPerformanceCounter());
    void *state=NULL; char *args[]={"longform-test","--notes",root,"--font",argv[2]};
    assert(SDL_AppInit(&state,5,args)==SDL_APP_CONTINUE); App *a=state; char error[256];
    assert(ce_document_import(a->doc,"<h1>First chapter</h1><p>first body</p><h2>Detail</h2><p>detail body</p><h1>Second chapter</h1><p>second body</p>",CE_FORMAT_HTML,error,sizeof error));
    a->active=copy_string("longform.json"); sync_view(a); a->outline=a->sidebar=true; a->sidebar_manual=true; layout(a);
    assert(outline_count(a)==3); char *original=ce_document_export_json(a->doc);
    click(a,18,a->sidebar_area.y+56); assert(outline_count(a)==2);
    char *folded=ce_document_export_json(a->doc); assert(!strcmp(original,folded) && !ce_document_can_undo(a->doc)); free(folded);
    float x=a->sidebar_area.w-18,y=a->sidebar_area.y+56;
    pointer(a,SDL_EVENT_MOUSE_BUTTON_DOWN,x,y);
    pointer(a,SDL_EVENT_MOUSE_MOTION,x,y+62);
    pointer(a,SDL_EVENT_MOUSE_BUTTON_UP,x,y+62);
    assert(!strcmp(ce_document_text(a->doc),"Second chapter\nsecond body\nFirst chapter\nfirst body\nDetail\ndetail body"));
    assert(outline_count(a)==2); assert(ce_document_undo(a->doc)); sync_view(a);
    char *restored=ce_document_export_json(a->doc); assert(!strcmp(original,restored)); free(restored); free(original);
    screenshot(a,root,"outline-folded");
    /* A changed revision cancels an in-flight section drag. */
    outline_count(a); pointer(a,SDL_EVENT_MOUSE_BUTTON_DOWN,x,y);
    assert(ce_document_insert(a->doc,ce_document_length(a->doc),"!")); sync_view(a);
    pointer(a,SDL_EVENT_MOUSE_MOTION,x,y+62); pointer(a,SDL_EVENT_MOUSE_BUTTON_UP,x,y+62);
    assert(!strncmp(ce_document_text(a->doc),"First chapter",13));
    outline_count(a); pointer(a,SDL_EVENT_MOUSE_BUTTON_DOWN,x,y);
    char *old_name=a->active; a->active=copy_string("another-note.json"); assert(a->active);
    pointer(a,SDL_EVENT_MOUSE_MOTION,x,y+62); assert(!a->outline_view.pointer);
    pointer(a,SDL_EVENT_MOUSE_BUTTON_UP,x,y+62); assert(!strncmp(ce_document_text(a->doc),"First chapter",13));
    free(a->active); a->active=old_name;

    assert(images_ready(a)); char uri[83]; assert(ce_image_store_add(&a->images.store,image_png,sizeof image_png,uri,NULL));
    const char *article="<h1>给文字留出空间</h1><p>这是发布前的检查：图片、链接和代码都应该出现在最终文章里。A long article needs a readable line length and a clear hierarchy. The preview lets us inspect the same article at desktop and phone widths before export.</p><blockquote><p>先把想法写下来，再慢慢整理。</p></blockquote><pre><code>int main(void) {\n    return 0;\n}</code></pre><p><a href=\"https://example.com/article\">查看文章链接</a></p>";
    assert(ce_document_import(a->doc,article,CE_FORMAT_HTML,error,sizeof error));
    char *node=ce_image_node_json(uri,"红绿相间的色块","图片说明会随文章一起导出",300); assert(node);
    assert(ce_document_insert_node(a->doc,ce_document_length(a->doc),node)); free(node);
    ce_document_mark_saved(a->doc); citation_refresh(a); sync_view(a);
    assert(SDL_SetWindowSize(a->window,1400,900)); a->outline=false; layout(a);
    original=ce_document_export_json(a->doc); uint64_t revision=ce_document_revision(a->doc);
    action(a,ACT_PREVIEW); draw(a); assert(a->preview.open && a->preview.valid);
    assert(a->editor_area.x+a->editor_area.w<a->preview.area.x);
    assert(a->preview.images==1 && a->preview.code==1 && a->preview.links==1);
    assert(a->preview.width==720 && a->preview.texture);
    float desktop_height=ce_rich_layout_height(a->preview.layout);
    screenshot(a,root,"publish-desktop");
    click(a,a->preview.buttons[1].x+10,a->preview.buttons[1].y+10); draw(a);
    assert(a->preview.mobile && a->preview.width==390 && a->preview.valid);
    assert(a->preview.viewport.w==390 && a->preview.scale==1.0f);
    assert(ce_rich_layout_height(a->preview.layout)>desktop_height);
    screenshot(a,root,"publish-mobile-width");
    assert(ce_document_revision(a->doc)==revision);
    restored=ce_document_export_json(a->doc); assert(!strcmp(original,restored)); free(restored); free(original);
    assert(SDL_SetWindowSize(a->window,390,844)); layout(a); draw(a);
    assert(!a->edit->has_focus && !a->sidebar && a->preview.area.x==0 && a->preview.viewport.w<=390);
    screenshot(a,root,"publish-phone");
    preview_regressions(a,root,uri);
    click(a,a->preview.buttons[3].x+10,a->preview.buttons[3].y+10); assert(!a->preview.open && a->edit->has_focus);
    assert(ce_document_import(a->doc,"{\"type\":\"doc\",\"content\":[{\"type\":\"unsupportedForPublish\"}]}",CE_FORMAT_JSON,error,sizeof error));
    sync_view(a); action(a,ACT_PREVIEW); assert(!preview_prepare(a) && !a->preview.valid && *a->preview.error);
    SDL_AppQuit(a,SDL_APP_SUCCESS); return 0;
}
