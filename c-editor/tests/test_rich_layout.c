#include "rich_layout.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static CeDocument *load(const char *s,CeFormat format) {
    CeDocument *d=ce_document_new(); char message[256]; assert(d);
    if(!ce_document_import(d,s,format,message,sizeof message)) { fprintf(stderr,"%s\n",message); abort(); }
    return d;
}
static void update(CeRichLayout *l,CeDocument *d,float width,const CeRichTheme *theme) {
    if(!ce_rich_layout_update(l,d,width,theme)) { fprintf(stderr,"Layout failed: %s\n",ce_rich_layout_error(l)); abort(); }
}
static void count_rect(const SDL_FRect *r,void *context) {
    size_t *count=context; assert(isfinite(r->x) && isfinite(r->y) && r->w>0 && r->h>0); (*count)++;
}
static void test_geometry(CeRichLayout *l,const CeRichTheme *theme) {
    CeDocument *d=load("<h1>Heading</h1><p>A e\xcc\x81 中文 <strong>bold</strong> "
                      "<em>italic</em> <u>underlined</u> tail words to wrap and wrap again</p>",CE_FORMAT_HTML);
    update(l,d,180,theme);
    SDL_FRect heading,body,base,combining;
    assert(ce_rich_layout_caret(l,0,&heading));
    assert(ce_rich_layout_caret(l,8,&body));
    assert(heading.h>body.h && body.y>heading.y);
    assert(ce_rich_layout_caret(l,10,&base));
    assert(ce_rich_layout_caret(l,11,&combining));
    assert(base.x==combining.x && base.y==combining.y);
    assert(ce_rich_layout_hit_test(l,base.x+.2f,base.y+base.h/2)==10);
    assert(ce_rich_layout_hit_test(l,-100,-100)==0);
    assert(!ce_rich_layout_caret(l,ce_document_length(d)+1,&body));
    size_t rects=0;
    ce_rich_layout_selection_rects(l,8,ce_document_length(d),count_rect,&rects);
    assert(rects>1);
    float narrow=ce_rich_layout_height(l);
    size_t generation=ce_rich_layout_generation(l);
    update(l,d,180,theme);
    assert(generation==ce_rich_layout_generation(l));
    update(l,d,600,theme);
    assert(ce_rich_layout_height(l)<narrow && ce_rich_layout_generation(l)==generation+1);
    CeDocument *bad=load("مرحبا",CE_FORMAT_TEXT);
    generation=ce_rich_layout_generation(l);
    assert(!ce_rich_layout_update(l,bad,600,theme));
    assert(strstr(ce_rich_layout_error(l),"bidirectional"));
    assert(ce_rich_layout_generation(l)==generation);
    assert(ce_rich_layout_caret(l,10,&base));
    ce_document_free(bad);
    ce_document_free(d);
}
static void test_wrap_and_empty(CeRichLayout *l,const CeRichTheme *theme) {
    CeDocument *d=load("",CE_FORMAT_TEXT); update(l,d,200,theme);
    SDL_FRect p; assert(ce_rich_layout_caret(l,0,&p)); assert(p.h>0);
    assert(ce_rich_layout_hit_test(l,1000,1000)==0);
    ce_document_free(d);
    char text[10001]; memset(text,'x',sizeof text-1); text[sizeof text-1]=0;
    d=load(text,CE_FORMAT_TEXT); update(l,d,100,theme);
    assert(ce_rich_layout_caret(l,5000,&p));
    assert(p.y>0 && isfinite(p.y) && p.x<110);
    assert(ce_rich_layout_caret(l,10000,&p));
    assert(p.y+p.h<=ce_rich_layout_height(l));
    assert(ce_rich_layout_hit_test(l,1000,1e9f)==10000);
    ce_document_free(d);
    d=load("<p>a<br>b<br></p><p></p><p>中文标点，测试。</p>",CE_FORMAT_HTML);
    update(l,d,70,theme);
    SDL_FRect a,b;
    assert(ce_rich_layout_caret(l,0,&a) && ce_rich_layout_caret(l,2,&b) && b.y>a.y);
    assert(ce_rich_layout_caret(l,4,&a) && a.y>b.y);
    ce_document_free(d);
}
static void test_lists_atoms_alignment(CeRichLayout *l,const CeRichTheme *theme) {
    CeDocument *d=load("<p style=\"text-align: right\">right</p><p style=\"text-align: center\">center</p>"
                      "<ol start=\"7\"><li><p>item</p><ul><li><p>nested</p></li></ul></li></ol>"
                      "<div data-type=\"zoteroCitationBlock\" endnotenumber=\"1\" author=\"Author\" title=\"中文 Title\" note=\"Quoted text\"></div>"
                      "<div data-type=\"zoteroEndnoteSection\" style=\"apa\" entriesjson=\"[{&quot;title&quot;:&quot;Book title&quot;}]\"></div>",CE_FORMAT_HTML);
    update(l,d,300,theme);
    SDL_FRect right,center,item,nested,atom;
    assert(ce_rich_layout_caret(l,0,&right) && right.x>150);
    assert(ce_rich_layout_caret(l,6,&center) && center.x>50 && center.x<right.x);
    assert(ce_rich_layout_caret(l,13,&item));
    assert(ce_rich_layout_caret(l,18,&nested) && nested.x>item.x);
    CeBlockInfo info;
    assert(ce_document_block_info(d,4,&info));
    assert(ce_rich_layout_caret(l,info.start,&atom) && atom.h>nested.h);
    assert(ce_rich_layout_hit_test(l,atom.x+2,atom.y+2)==info.start);
    assert(ce_rich_layout_hit_test(l,299,atom.y+2)==info.end);
    size_t selected=0;
    ce_rich_layout_selection_rects(l,info.start,info.end,count_rect,&selected);
    assert(selected==1);
    ce_document_free(d); /* The cached layout must not retain borrowed strings. */
}
static void test_pixels(CeRichLayout *l,SDL_Renderer *renderer,const CeRichTheme *theme) {
    CeDocument *d=load("<h1>Heading</h1><p><strong>Bold</strong> <em>Italic</em> <u>Underline</u></p>"
                      "<p><span style=\"color: #ff0000\">RED</span> <a href=\"https://example.com\">LINK</a> "
                      "<mark data-color=\"#ffff00\">HIGHLIGHT</mark></p>",CE_FORMAT_HTML);
    update(l,d,580,theme);
    SDL_SetRenderDrawColor(renderer,255,255,255,255); assert(SDL_RenderClear(renderer));
    SDL_Rect clip={0,0,620,450}; assert(SDL_SetRenderClipRect(renderer,&clip));
    SDL_SetRenderDrawColor(renderer,1,2,3,4);
    SDL_FRect viewport={5,5,600,420};
    assert(ce_rich_layout_draw(l,10,10,&viewport,0,0));
    SDL_Rect restored; assert(SDL_GetRenderClipRect(renderer,&restored));
    assert(!memcmp(&clip,&restored,sizeof clip));
    Uint8 r,g,b,a; assert(SDL_GetRenderDrawColor(renderer,&r,&g,&b,&a));
    assert(r==1 && g==2 && b==3 && a==4);
    SDL_Surface *surface=SDL_RenderReadPixels(renderer,NULL); assert(surface);
    size_t red=0,yellow=0,blue=0,ink=0;
    for(int y=0;y<surface->h;y++) for(int x=0;x<surface->w;x++) {
        assert(SDL_ReadSurfacePixel(surface,x,y,&r,&g,&b,&a));
        if(r>200 && g<80 && b<80) red++;
        if(r>230 && g>210 && b<90) yellow++;
        if(b>120 && b>r+40 && b>g+30) blue++;
        if(r<100 && g<100 && b<100) ink++;
    }
    assert(red>10 && yellow>100 && blue>10 && ink>100);
    const char *plain=ce_document_text(d), *under=strstr(plain,"Underline");
    assert(under);
    SDL_FRect left,right;
    assert(ce_rich_layout_caret(l,(size_t)(under-plain),&left));
    assert(ce_rich_layout_caret(l,(size_t)(under-plain)+9,&right));
    size_t longest=0;
    for(int py=(int)left.y+10;py<(int)(left.y+left.h)+10;py++) {
        size_t colored=0;
        for(int px=(int)left.x+10;px<(int)right.x+10;px++) {
            assert(SDL_ReadSurfacePixel(surface,px,py,&r,&g,&b,&a));
            if(r<100 && g<100 && b<100) colored++;
        }
        if(colored>longest) longest=colored;
    }
    assert((float)longest>(right.x-left.x)*.7f); /* Actual underline pixels. */
    const char *snapshot=getenv("CE_RICH_TEST_SNAPSHOT");
    if(snapshot && *snapshot) assert(SDL_SaveBMP(surface,snapshot));
    SDL_DestroySurface(surface); ce_document_free(d);
}
static void test_emoji(CeRichLayout *layout,TTF_Font *emoji,const CeRichTheme *theme) {
    const char *samples[]={"🙂","👍🏽","👩‍💻","👨‍👩‍👧‍👦","🇨🇳"};
    assert(TTF_FontHasGlyph(emoji,0x1f642));
    for(size_t i=0;i<sizeof samples/sizeof samples[0];i++) {
        CeDocument *doc=load(samples[i],CE_FORMAT_TEXT);
        update(layout,doc,300,theme);
        size_t bytes=strlen(samples[i]);
        assert(ce_rich_layout_step(layout,0,1)==bytes);
        assert(ce_rich_layout_step(layout,bytes,-1)==0);
        SDL_FRect left,right;
        assert(ce_rich_layout_caret(layout,0,&left) && ce_rich_layout_caret(layout,bytes,&right));
        /* A full ZWJ family must form one emoji, not four separated members. */
        assert(right.x-left.x>10 && right.x-left.x<40);
        assert(ce_document_style(doc,0,bytes,CE_MARK_BOLD,NULL,true));
        update(layout,doc,300,theme);
        assert(ce_rich_layout_caret(layout,bytes,&right) && right.x-left.x<40);
        assert(ce_rich_layout_draw(layout,0,0,NULL,0,bytes));
        ce_document_free(doc);
    }
}
int main(int argc,char **argv) {
    assert(argc==2 || argc==3);
    assert(SDL_SetEnvironmentVariable(SDL_GetEnvironment(),"SDL_VIDEODRIVER","dummy",true));
    assert(SDL_Init(SDL_INIT_VIDEO)); assert(TTF_Init());
    SDL_Window *window=SDL_CreateWindow("rich layout tests",640,480,0); assert(window);
    SDL_Renderer *renderer=SDL_CreateRenderer(window,"software"); assert(renderer);
    TTF_Font *font=TTF_OpenFont(argv[1],20); assert(font);
    CeRichLayout *layout=ce_rich_layout_create(renderer,font); assert(layout);
    CeRichTheme theme=ce_rich_theme_default(false);
    TTF_Font *emoji=argc==3?TTF_OpenFont(argv[2],20):NULL;
    if(argc==3) { assert(emoji); ce_rich_layout_set_fallback(layout,emoji); test_emoji(layout,emoji,&theme); }
    test_geometry(layout,&theme);
    test_wrap_and_empty(layout,&theme);
    test_lists_atoms_alignment(layout,&theme);
    assert(ce_rich_layout_draw(layout,0,0,NULL,0,0));
    test_pixels(layout,renderer,&theme);
    assert(TTF_GetFontStyle(font)==TTF_STYLE_NORMAL && TTF_GetFontSize(font)==20);
    ce_rich_layout_destroy(layout); TTF_CloseFont(font); if(emoji) TTF_CloseFont(emoji);
    SDL_DestroyRenderer(renderer); SDL_DestroyWindow(window); TTF_Quit(); SDL_Quit();
    puts("rich layout tests passed");
    return 0;
}
