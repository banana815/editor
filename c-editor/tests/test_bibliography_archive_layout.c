/* Inspect real SDL shaping geometry while exercising public transactional
 * update/draw/hit APIs. Do not separately compile rich_layout.c in this test. */
#include "rich_layout.h"
#include "bibliography_archive.h"
#include "csl_json.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static size_t allocation_count,allocation_failure;
static bool sticky;
static bool allocation_ok(void) {
    ++allocation_count;
    return !allocation_failure || (allocation_count!=allocation_failure && (!sticky || allocation_count<allocation_failure));
}
static void *layout_malloc(size_t n) { return allocation_ok()?malloc(n):NULL; }
static void *layout_calloc(size_t n,size_t size) { return allocation_ok()?calloc(n,size):NULL; }
static void *layout_realloc(void *p,size_t n) { return allocation_ok()?realloc(p,n):NULL; }
#define malloc layout_malloc
#define calloc layout_calloc
#define realloc layout_realloc
#include "../src/rich_layout.c"
#undef malloc
#undef calloc
#undef realloc
static char message[512];
typedef struct Source { CeRichCitationStatus status; const CeCitationCache *cache; size_t calls; } Source;
static CeRichCitationStatus resolve(void *context,const cJSON *node,const CeCitationCache **cache,char *error,size_t cap) {
    (void)node; (void)error; (void)cap; Source *s=context; ++s->calls; *cache=s->cache; return s->status;
}
static CeDocument *document(const char *entries,const char *mode) {
    cJSON *root=cJSON_CreateObject(),*content=cJSON_CreateArray(),*node=cJSON_CreateObject(),*attrs=cJSON_CreateObject();
    assert(root && content && node && attrs);
    assert(cJSON_AddStringToObject(root,"type","doc") && cJSON_AddItemToObject(root,"content",content));
    assert(cJSON_AddItemToArray(content,node) && cJSON_AddStringToObject(node,"type","zoteroEndnoteSection"));
    assert(cJSON_AddItemToObject(node,"attrs",attrs) && cJSON_AddStringToObject(attrs,"entriesJson",entries));
    assert(cJSON_AddStringToObject(attrs,"cslRenderJson","FORGED STANDARD OUTPUT"));
    assert(cJSON_AddStringToObject(attrs,"custom","unknown metadata must remain"));
    if(mode) assert(cJSON_AddStringToObject(attrs,"bibliographyMode",mode));
    char *json=ce_csl_json_print_exact(root); assert(json); CeDocument *doc=ce_document_new(); assert(doc);
    assert(ce_document_import(doc,json,CE_FORMAT_JSON,message,sizeof message)); cJSON_free(json); cJSON_Delete(root); return doc;
}
static Fragment *find(CeRichLayout *l,const char *prefix) {
    for(size_t i=0;i<l->fragment_count;i++) {
        Fragment *f=&l->fragments[i];
        if(f->text && f->text->text && !strncmp(f->text->text,prefix,strlen(prefix))) return f;
    }
    return NULL;
}
static void update(CeRichLayout *l,CeDocument *doc,float width,const CeRichTheme *theme,CeRichCitationSource *source) {
    if(!ce_rich_layout_update_citations(l,doc,width,theme,source)) { fprintf(stderr,"layout: %s\n",ce_rich_layout_error(l)); abort(); }
}
static const char *entries="[{\"number\":7,\"entryHtml\":\"<div class='csl-entry'><div class='csl-left-margin'>A.</div><div class='csl-right-inline'><i>Body title 中文🙂</i> has enough words to wrap at narrow widths while retaining every word. <a href='https://example.invalid/'>Safe link</a></div><div class='csl-indent'>Indented detail.</div></div>\"},{\"number\":8,\"entryHtml\":\"<b>Second</b> book é 👩‍💻 and <sup>2</sup> more text\"}]";
static void test_layout(CeRichLayout *l,SDL_Renderer *renderer,const CeRichTheme *theme) {
    Source state={CE_RICH_CITATION_ARCHIVED,NULL,0}; CeRichCitationSource source={1,&state,resolve};
    CeDocument *doc=document(entries,NULL); char *before=ce_document_export_json(doc); assert(before);
    update(l,doc,600,theme,&source);
    Fragment *body=find(l,"Body"),*left=find(l,"A."),*label=find(l,"[7]"),*title=find(l,"参考文献");
    assert(body && left && label && title && !find(l,"FORGED"));
    assert(label->rect.x<left->rect.x && left->rect.x<body->rect.x &&
        (TTF_GetFontStyle(TTF_GetTextFont(body->text))&TTF_STYLE_ITALIC));
    float wide=ce_rich_layout_height(l); size_t generation=ce_rich_layout_generation(l),calls=state.calls;
    update(l,doc,600,theme,&source); assert(generation==ce_rich_layout_generation(l) && calls==state.calls);
    update(l,doc,230,theme,&source); assert(ce_rich_layout_height(l)>wide);
    for(size_t i=0;i<l->fragment_count;i++) {
        Fragment *f=&l->fragments[i];
        if(f->text) assert(isfinite(f->rect.x) && isfinite(f->rect.y) && f->rect.x>=0 && f->rect.x+f->rect.w<=230.1f);
    }
    assert(l->row_count==1 && l->cluster_count==1);
    CeBlockInfo block; assert(ce_document_block_info(doc,0,&block));
    assert(ce_rich_layout_step(l,block.start,1)==block.end && ce_rich_layout_step(l,block.end,-1)==block.start);
    assert(ce_rich_layout_hit_test(l,0,20)==block.start && ce_rich_layout_hit_test(l,230,20)==block.end);
    char *after=ce_document_export_json(doc); assert(after && !strcmp(before,after)); free(before); free(after);
    assert(SDL_SetRenderDrawColor(renderer,255,255,255,255) && SDL_RenderClear(renderer));
    assert(ce_rich_layout_draw(l,20,20,NULL,0,0));
    const char *snapshot=getenv("CE_ARCHIVE_SNAPSHOT");
    if(snapshot && *snapshot) { SDL_Surface *pixels=SDL_RenderReadPixels(renderer,NULL); assert(pixels && SDL_SaveBMP(pixels,snapshot)); SDL_DestroySurface(pixels); }
    ce_document_free(doc); assert(ce_rich_layout_draw(l,0,0,NULL,0,0)); /* No snapshot/archive borrowed pointers remain. */
}
static void test_failures(CeRichLayout *l,const CeRichTheme *theme) {
    Source state={CE_RICH_CITATION_ARCHIVED,NULL,0}; CeRichCitationSource source={30,&state,resolve}; CeDocument *doc=document(entries,NULL);
    update(l,doc,420,theme,&source); size_t generation=ce_rich_layout_generation(l); float height=ce_rich_layout_height(l);
    const char *modes[]={"citations","selected","future",""};
    for(size_t i=0;i<sizeof modes/sizeof *modes;i++) {
        CeDocument *marked=document(entries,modes[i]); ++source.revision;
        assert(!ce_rich_layout_update_citations(l,marked,420,theme,&source));
        assert(generation==ce_rich_layout_generation(l) && height==ce_rich_layout_height(l)); ce_document_free(marked);
    }
    state.cache=(const CeCitationCache *)(uintptr_t)1; ++source.revision;
    assert(!ce_rich_layout_update_citations(l,doc,420,theme,&source)); state.cache=NULL;
    CeDocument *malformed=document("broken json",NULL); ++source.revision;
    assert(!ce_rich_layout_update_citations(l,malformed,420,theme,&source)); ce_document_free(malformed);
    assert(generation==ce_rich_layout_generation(l) && find(l,"Body"));
    state.status=CE_RICH_CITATION_STALE; ++source.revision; update(l,doc,420,theme,&source);
    assert(find(l,"参考文献已过期") && !find(l,"Body"));
    state.status=CE_RICH_CITATION_ARCHIVED; ++source.revision; update(l,doc,420,theme,&source);
    generation=ce_rich_layout_generation(l); height=ce_rich_layout_height(l);
    allocation_count=0; ++source.revision; update(l,doc,420,theme,&source); size_t points=allocation_count;
    generation=ce_rich_layout_generation(l); height=ce_rich_layout_height(l); assert(points>10);
    for(size_t mode=0;mode<2;mode++) for(size_t point=1;point<=points;point++) {
        sticky=mode!=0; allocation_count=0; allocation_failure=point; ++source.revision;
        assert(!ce_rich_layout_update_citations(l,doc,420,theme,&source)); allocation_failure=0;
        assert(generation==ce_rich_layout_generation(l) && height==ce_rich_layout_height(l) && find(l,"Body"));
        assert(ce_rich_layout_draw(l,0,0,NULL,0,0));
    }
    sticky=false; printf("archive layout: %zu allocation failpoints, sparse/sticky preserve previous layout\n",points);
    ce_document_free(doc);
    doc=document("[{\"entryHtml\":\"<script>alert(1)</script><img src='https://example.invalid/'>\",\"entryText\":\"Readable original archive\"}]",NULL);
    ++source.revision; update(l,doc,500,theme,&source); assert(find(l,"Readable") && !find(l,"<script>")); ce_document_free(doc);
    doc=document("[{\"entryHtml\":\"<script>alert(1)</script>\"}]",NULL);
    ++source.revision; update(l,doc,500,theme,&source); assert(find(l,"<script>alert(1)</script>")); ce_document_free(doc);
    doc=document("[]",NULL); ++source.revision; update(l,doc,180,theme,&source); assert(find(l,"参考文献")); ce_document_free(doc);
}
int main(int argc,char **argv) {
    assert(argc==3); assert(SDL_SetEnvironmentVariable(SDL_GetEnvironment(),"SDL_VIDEODRIVER","dummy",true));
    assert(SDL_Init(SDL_INIT_VIDEO) && TTF_Init());
    SDL_Window *window=SDL_CreateWindow("archived bibliography",650,1000,0); assert(window);
    SDL_Renderer *renderer=SDL_CreateRenderer(window,"software"); assert(renderer);
    TTF_Font *font=TTF_OpenFont(argv[1],20),*emoji=TTF_OpenFont(argv[2],20); assert(font && emoji);
    CeRichLayout *l=ce_rich_layout_create(renderer,font); assert(l); ce_rich_layout_set_fallback(l,emoji);
    CeRichTheme theme=ce_rich_theme_default(false); test_layout(l,renderer,&theme); test_failures(l,&theme);
    ce_rich_layout_destroy(l); TTF_CloseFont(font); TTF_CloseFont(emoji); SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window); TTF_Quit(); SDL_Quit(); puts("archived bibliography real font/layout/ownership tests PASS"); return 0;
}
