/* Include the implementation only in this test TU to inspect the actual shaped
 * rectangles/font objects; assertions exercise public update/draw/hit APIs.
 * Do not also compile rich_layout.c into this executable. */
#include "rich_layout.h"
#include "citation_cache.h"
#include "csl_document.h"
#include "cJSON.h"
#include <assert.h>
#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static size_t allocation_count,allocation_failure;
static bool allocation_ok(void) { return ++allocation_count!=allocation_failure; }
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
static const CeCitationCacheHeader header={CE_CITATION_PROCESSOR_PROFILE,"apa","http://www.zotero.org/styles/apa",
    "0000000000000000000000000000000000000000000000000000000000000000",
    "0000000000000000000000000000000000000000","en-us",
    "0000000000000000000000000000000000000000",
    "0000000000000000000000000000000000000000000000000000000000000000"};
typedef struct Source {
    CeCitationCache *citation,*bibliography;
    CeRichCitationStatus status;
    size_t calls;
} Source;
static CeRichCitationStatus resolve(void *context,const cJSON *atom,const CeCitationCache **out,char *err,size_t cap) {
    Source *s=context;s->calls++;
    *out=!strcmp(jstr(atom,"type"),"zoteroEndnoteSection")?s->bibliography:s->citation;
    if(s->status==CE_RICH_CITATION_ERROR && err && cap) snprintf(err,cap,"Deliberate resolver failure");
    return s->status;
}
static CeDocument *document(bool bibliography,bool note) {
    CeDocument *d=ce_document_new();assert(d);
    const char *json=bibliography?"{\"type\":\"doc\",\"content\":[{\"type\":\"zoteroEndnoteSection\",\"attrs\":{\"entriesJson\":\"[{\\\"entryHtml\\\":\\\"FAKE APPROXIMATE\\\"}]\"}}]}":
        note?"{\"type\":\"doc\",\"content\":[{\"type\":\"zoteroCitationBlock\",\"attrs\":{\"citationId\":\"c1\",\"note\":\"<literal> 中文🙂\"}}]}":
        "{\"type\":\"doc\",\"content\":[{\"type\":\"zoteroCitationBlock\",\"attrs\":{\"citationId\":\"c1\",\"title\":\"FAKE APPROXIMATE\"}}]}";
    assert(ce_document_import(d,json,CE_FORMAT_JSON,message,sizeof message));return d;
}
static void add(CeCslOutput *out,const char *text,unsigned format,const char *href) {
    assert(ce_csl_output_append(out,text,strlen(text),format,href,message,sizeof message));
}
static CeCitationCache *citation(const char *text,unsigned format) {
    CeCslOutput *output=ce_csl_output_new(NULL,message,sizeof message);assert(output);
    add(output,text,format,NULL);CeCitationCache *cache=NULL;
    assert(ce_citation_cache_from_citation(&header,"c1",output,NULL,&cache,message,sizeof message));
    ce_csl_output_free(output);return cache;
}
static CeCitationCache *bibliography(const char *alignment,bool hanging,unsigned lines,unsigned spacing,const char *entries) {
    const char *prefix="{\"version\":1,\"kind\":\"bibliography\",\"processorProfile\":\"notelab-csl/1\","
        "\"style\":{\"profile\":\"apa\",\"id\":\"http://www.zotero.org/styles/apa\","
        "\"sha256\":\"0000000000000000000000000000000000000000000000000000000000000000\","
        "\"commit\":\"0000000000000000000000000000000000000000\",\"locale\":\"en-us\","
        "\"localeCommit\":\"0000000000000000000000000000000000000000\"},"
        "\"sourceDigest\":\"0000000000000000000000000000000000000000000000000000000000000000\",";
    size_t size=strlen(prefix)+strlen(entries)+512;char *json=malloc(size);assert(json);
    snprintf(json,size,"%s\"options\":{\"alignment\":\"%s\",\"hangingIndent\":%s,\"lineSpacing\":%u,\"entrySpacing\":%u},\"entries\":%s}",
        prefix,alignment,hanging?"true":"false",lines,spacing,entries);
    CeCitationCache *cache=NULL;
    if(!ce_citation_cache_decode(json,strlen(json),NULL,&cache,message,sizeof message)) { fprintf(stderr,"cache: %s\n",message);abort(); }
    free(json);return cache;
}
static void update_checked(CeRichLayout *l,CeDocument *d,float width,const CeRichTheme *theme,CeRichCitationSource *source) {
    if(!ce_rich_layout_update_citations(l,d,width,theme,source)) { fprintf(stderr,"renderer: %s\n",ce_rich_layout_error(l));abort(); }
}
static Fragment *find_fragment(CeRichLayout *l,const char *prefix,size_t occurrence) {
    for(size_t i=0;i<l->fragment_count;i++) {
        Fragment *f=&l->fragments[i];
        if(f->text && !strncmp(f->text->text,prefix,strlen(prefix)) && !occurrence--) return f;
    }
    return NULL;
}
static void selected(const SDL_FRect *r,void *data) {
    size_t *n=data;assert(r->w>0 && r->h>0);++*n;
}
static void test_citation(CeRichLayout *l,const CeRichTheme *theme) {
    CeCslOutput *out=ce_csl_output_new(NULL,message,sizeof message);assert(out);
    add(out,"Normal ",0,NULL);add(out,"Bold ",CE_CSL_BOLD,NULL);add(out,"Italic ",CE_CSL_ITALIC,NULL);
    add(out,"Under ",CE_CSL_UNDERLINE,NULL);add(out,"2 ",CE_CSL_SUPERSCRIPT,NULL);add(out,"i ",CE_CSL_SUBSCRIPT,NULL);
    add(out,"中文 e",0,NULL);add(out,"́",CE_CSL_ITALIC,NULL);add(out," 👩‍💻 <script>",0,NULL);
    Source state={.status=CE_RICH_CITATION_VALID};
    assert(ce_citation_cache_from_citation(&header,"c1",out,NULL,&state.citation,message,sizeof message));
    ce_csl_output_free(out);
    CeRichCitationSource source={1,&state,resolve};CeDocument *doc=document(false,true);
    update_checked(l,doc,1100,theme,&source);
    Fragment *normal=find_fragment(l,"Normal",0),*bold=find_fragment(l,"Bold",0),*italic=find_fragment(l,"Italic",0);
    Fragment *under=find_fragment(l,"Under",0),*sup=find_fragment(l,"2 ",0),*sub=find_fragment(l,"i ",0),*emoji=find_fragment(l,"👩‍💻",0);
    assert(normal && bold && italic && under && sup && sub && emoji);
    assert(TTF_GetFontStyle(TTF_GetTextFont(bold->text))&TTF_STYLE_BOLD);
    assert(TTF_GetFontStyle(TTF_GetTextFont(italic->text))&TTF_STYLE_ITALIC);
    float baseline=normal->rect.y+(float)TTF_GetFontAscent(TTF_GetTextFont(normal->text));
    assert(under->underline && sup->rect.y+(float)TTF_GetFontAscent(TTF_GetTextFont(sup->text))<baseline &&
           sub->rect.y+(float)TTF_GetFontAscent(TTF_GetTextFont(sub->text))>baseline);
    assert(TTF_GetFontSize(TTF_GetTextFont(sup->text))<TTF_GetFontSize(TTF_GetTextFont(normal->text)));
    TTF_SubString glyph;assert(TTF_GetTextSubString(emoji->text,0,&glyph) && (size_t)glyph.length==strlen("👩‍💻"));
    assert(find_fragment(l,"中文 é",0)); /* No split of combining cluster at the italic edge. */
    assert(find_fragment(l,"摘录：<literal> 中文",0));
    CeBlockInfo block;assert(ce_document_block_info(doc,0,&block));
    assert(l->row_count==1 && l->cluster_count==1);
    size_t count=0;ce_rich_layout_selection_rects(l,block.start,block.end,selected,&count);assert(count==1);
    SDL_FRect caret;assert(ce_rich_layout_caret(l,block.start+1,&caret) && caret.x==0);
    assert(ce_rich_layout_step(l,block.start,1)==block.end);
    assert(ce_rich_layout_step(l,block.end,-1)==block.start);
    assert(ce_rich_layout_hit_test(l,1,caret.h*.9f)==block.start);
    assert(ce_rich_layout_hit_test(l,1099,caret.h*.9f)==block.end);
    size_t generation=ce_rich_layout_generation(l),calls=state.calls;
    update_checked(l,doc,1100,theme,&source);assert(state.calls==calls && ce_rich_layout_generation(l)==generation);
    source.revision++;update_checked(l,doc,1100,theme,&source);assert(state.calls==calls+1);
    ce_citation_cache_free(state.citation);ce_document_free(doc);
    assert(ce_rich_layout_draw(l,0,0,NULL,0,0)); /* All cache and JSON owners are gone. */
}
static const char *columns="[{\"sourceItemId\":\"a\",\"number\":9,\"visible\":true,\"rows\":[{\"kind\":\"two-columns\","
    "\"left\":[{\"text\":\"[9]\",\"format\":0}],\"body\":[{\"text\":\"First long title 中文🙂 continues with enough words to wrap across the available body width.\",\"format\":2}]}]},"
    "{\"sourceItemId\":\"b\",\"number\":10,\"visible\":true,\"rows\":[{\"kind\":\"two-columns\","
    "\"left\":[{\"text\":\"[10]\",\"format\":0}],\"body\":[{\"text\":\"Second long title uses the same body column and keeps complete words.\",\"format\":0}]}]}]";
static const char *flow="[{\"sourceItemId\":\"a\",\"number\":1,\"visible\":true,\"rows\":[{\"kind\":\"flow\","
    "\"body\":[{\"text\":\"Hanging title words continue through multiple long lines and preserve every space and 中文 character in this reference entry.\",\"format\":0}]}]}]";
static void test_bibliography(CeRichLayout *l,const CeRichTheme *theme) {
    Source state={.status=CE_RICH_CITATION_VALID,.bibliography=bibliography("flush",false,1,0,columns)};
    CeRichCitationSource source={10,&state,resolve};CeDocument *doc=document(true,false);
    update_checked(l,doc,340,theme,&source);
    Fragment *first=find_fragment(l,"First",0),*second=find_fragment(l,"Second",0),*nine=find_fragment(l,"[9]",0),*ten=find_fragment(l,"[10]",0);
    assert(first && second && nine && ten && first->rect.x==second->rect.x && nine->rect.x==ten->rect.x);
    assert(first->rect.x>ten->rect.x+ten->rect.w);
    float compact=ce_rich_layout_height(l),distance=second->rect.y-first->rect.y;
    ce_citation_cache_free(state.bibliography);state.bibliography=bibliography("flush",false,2,1,columns);source.revision++;
    update_checked(l,doc,340,theme,&source);
    first=find_fragment(l,"First",0);second=find_fragment(l,"Second",0);
    assert(first && second && second->rect.y-first->rect.y>distance*2 && ce_rich_layout_height(l)>compact*1.8f);
    ce_citation_cache_free(state.bibliography);state.bibliography=bibliography("none",true,1,0,flow);source.revision++;
    update_checked(l,doc,260,theme,&source);
    first=find_fragment(l,"Hanging",0);assert(first && first->rect.x==8);
    size_t lines=0;
    for(size_t i=0;i<l->fragment_count;i++) if(l->fragments[i].text) {
        Fragment *f=&l->fragments[i];if(lines++) assert(f->rect.x==48);
    }
    assert(lines>2 && l->cluster_count==1);
    float hanging_height=ce_rich_layout_height(l);
    source.revision++;update_checked(l,doc,900,theme,&source);assert(ce_rich_layout_height(l)<hanging_height);
    ce_citation_cache_free(state.bibliography);ce_document_free(doc);
}
static void test_status_and_atomic(CeRichLayout *l,const CeRichTheme *theme) {
    Source state={.citation=citation("Visible current citation 中文🙂",0),.status=CE_RICH_CITATION_VALID};
    CeRichCitationSource source={50,&state,resolve};CeDocument *doc=document(false,false);
    update_checked(l,doc,500,theme,&source);size_t generation=ce_rich_layout_generation(l);float height=ce_rich_layout_height(l);
    state.status=CE_RICH_CITATION_ERROR;source.revision++;
    assert(!ce_rich_layout_update_citations(l,doc,500,theme,&source));
    assert(strstr(ce_rich_layout_error(l),"Deliberate") && ce_rich_layout_generation(l)==generation && ce_rich_layout_height(l)==height);
    state.status=CE_RICH_CITATION_STALE;source.revision++;update_checked(l,doc,500,theme,&source);
    assert(find_fragment(l,"引用已过期",0) && !find_fragment(l,"FAKE",0) && !find_fragment(l,"Visible",0));
    state.status=CE_RICH_CITATION_ABSENT;source.revision++;update_checked(l,doc,500,theme,&source);assert(find_fragment(l,"引用尚未生成",0));
    ce_citation_cache_free(state.citation);state.citation=citation("Unsupported",CE_CSL_SMALL_CAPS);state.status=CE_RICH_CITATION_VALID;
    source.revision++;update_checked(l,doc,500,theme,&source);assert(find_fragment(l,"此引用的格式暂不支持",0));
    ce_citation_cache_free(state.citation);state.citation=citation("New body bold 中文🙂",CE_CSL_BOLD);source.revision++;
    allocation_count=0;allocation_failure=0;update_checked(l,doc,500,theme,&source);size_t points=allocation_count;
    assert(points>5);generation=ce_rich_layout_generation(l);height=ce_rich_layout_height(l);
    for(size_t failpoint=1;failpoint<=points;failpoint++) {
        source.revision++;allocation_count=0;allocation_failure=failpoint;
        assert(!ce_rich_layout_update_citations(l,doc,500,theme,&source));
        allocation_failure=0;
        assert(ce_rich_layout_generation(l)==generation && ce_rich_layout_height(l)==height);
        assert(find_fragment(l,"New body bold",0) && ce_rich_layout_draw(l,0,0,NULL,0,0));
    }
    printf("controlled citation layout: %zu owned allocation failures preserved prior layout\n",points);
    ce_citation_cache_free(state.citation);ce_document_free(doc);
    doc=document(true,false);state.bibliography=bibliography("flush",false,UINT_MAX,UINT_MAX,columns);source.revision++;
    assert(!ce_rich_layout_update_citations(l,doc,500,theme,&source));assert(ce_rich_layout_generation(l)==generation);
    ce_citation_cache_free(state.bibliography);
    state.bibliography=bibliography("flush",false,1,0,columns);source.revision++;
    allocation_count=0;update_checked(l,doc,340,theme,&source);points=allocation_count;
    generation=ce_rich_layout_generation(l);height=ce_rich_layout_height(l);
    for(size_t failpoint=1;failpoint<=points;failpoint++) {
        source.revision++;allocation_count=0;allocation_failure=failpoint;
        assert(!ce_rich_layout_update_citations(l,doc,340,theme,&source));allocation_failure=0;
        assert(ce_rich_layout_generation(l)==generation && ce_rich_layout_height(l)==height);
        assert(find_fragment(l,"First",0) && ce_rich_layout_draw(l,0,0,NULL,0,0));
    }
    printf("controlled bibliography layout: %zu owned allocation failures preserved prior layout\n",points);
    ce_citation_cache_free(state.bibliography);ce_document_free(doc);
}
static void test_pixels(CeRichLayout *l,SDL_Renderer *renderer,const CeRichTheme *theme) {
    Source state={.bibliography=bibliography("flush",false,1,1,columns),.status=CE_RICH_CITATION_VALID};
    CeRichCitationSource source={100,&state,resolve};CeDocument *doc=document(true,false);
    update_checked(l,doc,570,theme,&source);
    SDL_SetRenderDrawColor(renderer,255,255,255,255);assert(SDL_RenderClear(renderer));
    assert(ce_rich_layout_draw(l,20,20,NULL,0,0));
    SDL_Surface *pixels=SDL_RenderReadPixels(renderer,NULL);assert(pixels);size_t ink=0;
    for(int y=0;y<pixels->h;y++) for(int x=0;x<pixels->w;x++) {
        Uint8 r,g,b,a;assert(SDL_ReadSurfacePixel(pixels,x,y,&r,&g,&b,&a));
        if(r<90 && g<90 && b<90) ink++;
    }
    assert(ink>300);
    const char *path=getenv("CE_RICH_CITATION_SNAPSHOT");if(path && *path) assert(SDL_SaveBMP(pixels,path));
    SDL_DestroySurface(pixels);ce_citation_cache_free(state.bibliography);ce_document_free(doc);
}
static void test_italic_pixels(CeRichLayout *l,SDL_Renderer *renderer,const CeRichTheme *theme) {
    CeDocument *doc=ce_document_new();assert(doc);
    assert(ce_document_import(doc,"<p><em>Readable italic overhang f j</em></p>",CE_FORMAT_HTML,message,sizeof message));
    assert(ce_rich_layout_update(l,doc,900,theme));
    Fragment *f=find_fragment(l,"Readable",0);assert(f && f->texture);
    SDL_FRect first,last;assert(ce_rich_layout_caret(l,0,&first));
    assert(ce_rich_layout_caret(l,ce_document_length(doc),&last) && last.x>first.x);
    assert(ce_rich_layout_hit_test(l,first.x+.1f,first.y+first.h*.5f)==0);
    SDL_SetRenderDrawColor(renderer,255,255,255,255);assert(SDL_RenderClear(renderer));
    assert(ce_rich_layout_draw(l,20,20,NULL,0,0));
    SDL_Surface *reference=TTF_RenderText_Blended(TTF_GetTextFont(f->text),f->text->text,strlen(f->text->text),theme->text);assert(reference);
    assert(reference->w==(int)f->rect.w && reference->h==(int)f->rect.h);
    SDL_Texture *texture=SDL_CreateTextureFromSurface(renderer,reference);assert(texture);
    SDL_FRect where={20+f->rect.x,120+f->rect.y,f->rect.w,f->rect.h};assert(SDL_RenderTexture(renderer,texture,NULL,&where));
    SDL_Surface *pixels=SDL_RenderReadPixels(renderer,NULL);assert(pixels);size_t ink=0;
    for(int y=0;y<reference->h;y++) for(int x=0;x<reference->w;x++) {
        Uint8 r,g,b,a,rr,gg,bb,aa;
        assert(SDL_ReadSurfacePixel(pixels,20+(int)f->rect.x+x,20+(int)f->rect.y+y,&r,&g,&b,&a));
        assert(SDL_ReadSurfacePixel(pixels,20+(int)f->rect.x+x,120+(int)f->rect.y+y,&rr,&gg,&bb,&aa));
        assert(r==rr && g==gg && b==bb && a==aa);ink+=r<100;
    }
    assert(ink>100);SDL_DestroySurface(pixels);SDL_DestroyTexture(texture);SDL_DestroySurface(reference);ce_document_free(doc);
}
static void test_display_rows(CeRichLayout *l,const CeRichTheme *theme) {
    const char *entries="[{\"sourceItemId\":\"a\",\"number\":1,\"visible\":true,\"rows\":["
        "{\"kind\":\"block\",\"body\":[{\"text\":\"Block heading\",\"format\":1}]},"
        "{\"kind\":\"indent\",\"body\":[{\"text\":\"Indented annotation\",\"format\":0}]},"
        "{\"kind\":\"two-columns\",\"left\":[{\"text\":\"[100]\",\"format\":0}],\"body\":[{\"text\":\"Column body\",\"format\":0}]}]}]";
    Source state={.bibliography=bibliography("none",false,1,0,entries),.status=CE_RICH_CITATION_VALID};
    CeRichCitationSource source={150,&state,resolve};CeDocument *doc=document(true,false);update_checked(l,doc,400,theme,&source);
    Fragment *block=find_fragment(l,"Block",0),*indent=find_fragment(l,"Indented",0),*body=find_fragment(l,"Column",0);
    assert(block && indent && body && indent->rect.x-block->rect.x==40 && body->rect.y>indent->rect.y);
    ce_citation_cache_free(state.bibliography);state.bibliography=bibliography("margin",false,1,0,entries);source.revision++;
    update_checked(l,doc,400,theme,&source);block=find_fragment(l,"Block",0);body=find_fragment(l,"Column",0);
    assert(block && body && block->rect.x==body->rect.x); /* Full-width text starts at margin-mode body origin. */
    ce_citation_cache_free(state.bibliography);ce_document_free(doc);
}

static char *resource(const char *path) {
    FILE *f=fopen(path,"rb");assert(f);assert(!fseek(f,0,SEEK_END));long size=ftell(f);assert(size>=0);rewind(f);
    char *text=malloc((size_t)size+1);assert(text);assert(fread(text,1,(size_t)size,f)==(size_t)size);assert(!fclose(f));
    text[size]=0;return text;
}
static void test_real_styles(CeRichLayout *l,SDL_Renderer *renderer,const CeRichTheme *theme,char **paths) {
    char *en=resource(paths[3]),*zh=resource(paths[4]);
    CeCslXml *en_xml=ce_csl_xml_parse(en,strlen(en),message,sizeof message),*zh_xml=ce_csl_xml_parse(zh,strlen(zh),message,sizeof message);
    assert(en_xml && zh_xml);free(en);free(zh);const CeCslXmlNode *roots[]={en_xml->root,zh_xml->root};
    CeCslLocaleCatalog catalog={roots,2,NULL,0};
    const char *json="{\"items\":[{\"id\":\"a\",\"type\":\"book\",\"title\":\"A long book title about 中文🙂 and readable references that wraps on a narrow screen\","
        "\"author\":[{\"family\":\"Doe\",\"given\":\"Jane\"}],\"issued\":{\"date-parts\":[[2024]]},\"publisher\":\"Example Publisher\"}],"
        "\"citations\":[{\"citationID\":\"c1\",\"citationItems\":[{\"id\":\"a\"}]}]}";
    CeCslInput *input=NULL;assert(ce_csl_input_load(&input,json,strlen(json),message,sizeof message));
    CeDocument *doc=ce_document_new();assert(doc);
    const char *atoms="{\"type\":\"doc\",\"content\":[{\"type\":\"zoteroCitationBlock\",\"attrs\":{\"citationId\":\"c1\"}},{\"type\":\"zoteroEndnoteSection\"}]}";
    assert(ce_document_import(doc,atoms,CE_FORMAT_JSON,message,sizeof message));
    for(size_t i=0;i<3;i++) {
        char *xml=resource(paths[i]);CeCslStyle *style=NULL;
        assert(ce_csl_style_load(&style,xml,strlen(xml),message,sizeof message));free(xml);
        CeCslLocale *locale=ce_csl_locale_new(ce_csl_style_root(style),&catalog,NULL,NULL,message,sizeof message);assert(locale);
        CeCslInput *article=NULL;
        if(i==1) {
            const char *data="{\"items\":[{\"id\":\"a\",\"type\":\"article-journal\",\"title\":\"浏览器导入文献🙂\",\"author\":[{\"family\":\"张\",\"given\":\"三\"}],\"issued\":{\"date-parts\":[[2024]]},\"DOI\":\"10.1000/browser\",\"container-title\":\"测试期刊\",\"volume\":\"12\",\"page\":\"10-20\"}],\"citations\":[{\"citationID\":\"c1\",\"citationItems\":[{\"id\":\"a\"}]}]}";
            assert(ce_csl_input_load(&article,data,strlen(data),message,sizeof message));
        }
        CeCslDocument *rendered=NULL;
        if(!ce_csl_document_build(style,locale,article?article:input,NULL,NULL,&rendered,message,sizeof message)) { fprintf(stderr,"real style: %s\n",message);abort(); }
        Source state={.status=CE_RICH_CITATION_VALID};
        assert(ce_citation_cache_from_citation(&header,"c1",ce_csl_document_citation(rendered,0)->output,NULL,&state.citation,message,sizeof message));
        const CeCslDocumentBibliography *bib=ce_csl_document_bibliography(rendered,0);assert(bib);
        CeCitationCacheBibliographySource item={"a",bib->citation_number,bib->visible,bib->entry};
        assert(ce_citation_cache_from_bibliography(&header,ce_csl_bibliography_entry_options(bib->entry),&item,1,NULL,&state.bibliography,message,sizeof message));
        CeRichCitationSource source={200+i,&state,resolve};float width=i==1?812:350;update_checked(l,doc,width,theme,&source);
        assert(l->row_count==2 && !find_fragment(l,"此引用",0) && !find_fragment(l,"此参考文献",0));
        if(i==0) {
            bool italic=false;for(size_t j=0;j<l->fragment_count;j++)
                if(l->fragments[j].text) italic|=(TTF_GetFontStyle(TTF_GetTextFont(l->fragments[j].text))&TTF_STYLE_ITALIC)!=0;
            assert(italic && ce_csl_bibliography_entry_options(bib->entry)->hanging_indent);
        }
        if(i==1) {
            const CeCitationCacheEntry *entry=ce_citation_cache_entry(state.bibliography,0);assert(entry && entry->row_count==1);
            CeCslOutputString expected={0};assert(ce_csl_output_plain(entry->rows[0].body,&expected,message,sizeof message));
            const float widths[]={812,600,350};
            for(size_t k=0;k<SDL_arraysize(widths);k++) {
                width=widths[k];update_checked(l,doc,width,theme,&source);
                char *drawn=calloc(expected.length+1,1);assert(drawn);size_t used=0;bool wrapped=false;
                float top=l->rows[1].rect.y;
                for(size_t j=0;j<l->fragment_count;j++) {
                    Fragment *f=&l->fragments[j];if(!f->text || f->rect.y<top)continue;
                    assert(f->rect.x+f->rect.w<=width+.01f); /* The browser DOI used to extend past the card. */
                    if(f->rect.x>8.01f) {
                        size_t bytes=strlen(f->text->text);assert(bytes<=expected.length-used);
                        memcpy(drawn+used,f->text->text,bytes);used+=bytes;
                        wrapped|=f->rect.y>top+citation_baseline(l);
                    }
                }
                assert((wrapped || width>600) && used==expected.length && !strcmp(drawn,expected.data));free(drawn);
            }
            ce_csl_output_string_free(&expected);
        }
        if(i==2) {
            Fragment *citation_fragment=NULL;
            for(size_t j=0;j<l->fragment_count;j++) if(l->fragments[j].text) { citation_fragment=&l->fragments[j];break; }
            assert(citation_fragment && TTF_GetFontSize(TTF_GetTextFont(citation_fragment->text))<20);
        }
        ce_citation_cache_free(state.citation);ce_citation_cache_free(state.bibliography);
        ce_csl_document_free(rendered);ce_csl_input_free(article);ce_csl_locale_free(locale);ce_csl_style_free(style);
        SDL_SetRenderDrawColor(renderer,255,255,255,255);assert(SDL_RenderClear(renderer));
        assert(ce_rich_layout_draw(l,20,20,NULL,0,0));
        const char *prefix=getenv("CE_RICH_CITATION_STYLE_SNAPSHOT");
        if(prefix && *prefix) {
            char path[1024];snprintf(path,sizeof path,"%s-%zu.bmp",prefix,i);
            SDL_Surface *pixels=SDL_RenderReadPixels(renderer,NULL);assert(pixels);assert(SDL_SaveBMP(pixels,path));SDL_DestroySurface(pixels);
        }
    }
    ce_document_free(doc);ce_csl_input_free(input);ce_csl_xml_free(en_xml);ce_csl_xml_free(zh_xml);
    puts("actual APA/IEEE/GB document -> controlled cache -> native renderer passed");
}

int main(int argc,char **argv) {
    assert(argc==3 || argc==8);assert(SDL_SetEnvironmentVariable(SDL_GetEnvironment(),"SDL_VIDEODRIVER","dummy",true));
    assert(SDL_Init(SDL_INIT_VIDEO));assert(TTF_Init());
    SDL_Window *window=SDL_CreateWindow("controlled citation renderer",1200,1000,0);assert(window);
    SDL_Renderer *renderer=SDL_CreateRenderer(window,"software");assert(renderer);
    TTF_Font *font=TTF_OpenFont(argv[1],20),*emoji=TTF_OpenFont(argv[2],20);assert(font && emoji);
    CeRichLayout *layout=ce_rich_layout_create(renderer,font);assert(layout);ce_rich_layout_set_fallback(layout,emoji);
    CeRichTheme theme=ce_rich_theme_default(false);
    test_citation(layout,&theme);test_bibliography(layout,&theme);test_status_and_atomic(layout,&theme);test_pixels(layout,renderer,&theme);
    test_italic_pixels(layout,renderer,&theme);test_display_rows(layout,&theme);
    if(argc==8) test_real_styles(layout,renderer,&theme,argv+3);
    ce_rich_layout_destroy(layout);TTF_CloseFont(font);TTF_CloseFont(emoji);
    SDL_DestroyRenderer(renderer);SDL_DestroyWindow(window);TTF_Quit();SDL_Quit();
    puts("controlled citation renderer: fonts, Unicode, columns, hanging/spacing, atom geometry, ownership and pixels passed");return 0;
}
