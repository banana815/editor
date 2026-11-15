#include "rich_layout.h"
#include "citation_cache.h"
#include "bibliography_archive.h"
#include "cJSON.h"
#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Layout uses SDL_ttf/HarfBuzz clusters, never one byte per glyph. Shaped runs
 * are re-shaped at their final line boundaries, then the same TTF_Text objects
 * supply rendering and hit geometry. */
typedef struct {
    TTF_Text *text;
    SDL_Texture *texture;
    SDL_FRect rect;
    SDL_Color background;
    bool highlighted, atom;
    bool underline;
    SDL_Color foreground;
    float underline_y;
} Fragment;
typedef struct { size_t start, end; SDL_FRect rect; size_t row; } Cluster;
typedef struct { size_t start, end; SDL_FRect rect; } Row;
typedef struct { size_t start, end; unsigned font; SDL_Color color, background; bool highlight; float width, shift; } Unit;
typedef struct { char *marker, *label; const cJSON *citation; } BlockLabel;
#define FONT_COUNT 128
#define CITATION_MAX_WORK ((size_t)1048576)
#define CITATION_MAX_FRAGMENTS ((size_t)131072)
#define CITATION_MAX_HEIGHT 10000000.0f
#define MAX_TEXTURE_BYTES ((size_t)128 * 1024 * 1024)
struct CeRichLayout {
    SDL_Renderer *renderer;
    TTF_Font *base, *fallback, *fonts[FONT_COUNT], *fallbacks[FONT_COUNT];
    TTF_TextEngine *engine;
    Fragment *fragments;
    size_t fragment_count, fragment_cap;
    Cluster *clusters;
    size_t cluster_count, cluster_cap;
    Row *rows;
    size_t row_count, row_cap;
    char *snapshot;
    size_t text_length, generation;
    size_t citation_work;
    size_t texture_bytes;
    uint64_t citation_revision;
    bool controlled_citations, citation_source;
    float width, height, font_size;
    CeRichTheme theme;
    char error[256];
};
static const char *jstr(const cJSON *n, const char *key) {
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(n, key);
    return cJSON_IsString(v) ? v->valuestring : "";
}
static cJSON *jfield(const cJSON *n, const char *key) { return cJSON_GetObjectItemCaseSensitive(n,key); }
static char *dupstr(const char *s) {
    size_t n = strlen(s) + 1;
    char *p = malloc(n);
    if (p) memcpy(p,s,n);
    return p;
}
static bool reserve(void **p, size_t *cap, size_t count, size_t size) {
    if (count <= *cap) return true;
    size_t next = *cap ? *cap * 2 : 32;
    if (next < count) next = count;
    if (next > SIZE_MAX / size) return false;
    void *q = realloc(*p, next * size);
    if (!q) return false;
    *p=q; *cap=next;
    return true;
}
static bool error(CeRichLayout *l, const char *message) {
    snprintf(l->error,sizeof l->error,"%s",message && *message ? message : "Rich layout allocation failed");
    return false;
}
static bool color_eq(SDL_Color a, SDL_Color b) { return a.r==b.r && a.g==b.g && a.b==b.b && a.a==b.a; }
static bool theme_eq(const CeRichTheme *a, const CeRichTheme *b) {
    return color_eq(a->text,b->text) && color_eq(a->link,b->link) && color_eq(a->highlight,b->highlight) &&
        color_eq(a->selection,b->selection) && color_eq(a->atom_background,b->atom_background) && color_eq(a->atom_border,b->atom_border);
}
CeRichTheme ce_rich_theme_default(bool dark) {
    if (dark) return (CeRichTheme){{228,231,238,255},{112,171,255,255},{111,91,20,255},{63,101,164,160},{34,41,55,255},{82,98,123,255}};
    return (CeRichTheme){{30,36,48,255},{31,95,198,255},{255,230,125,255},{99,156,235,120},{239,243,250,255},{155,174,203,255}};
}
static int hex(unsigned char c) {
    if (c>='0' && c<='9') return c-'0';
    if (c>='a' && c<='f') return c-'a'+10;
    if (c>='A' && c<='F') return c-'A'+10;
    return -1;
}
static SDL_Color css_color(const char *s, SDL_Color fallback) {
    if (!s || !*s) return fallback;
    if (!strcmp(s,"red")) return (SDL_Color){220,45,45,255};
    if (!strcmp(s,"yellow")) return (SDL_Color){255,230,125,255};
    if (!strcmp(s,"blue")) return (SDL_Color){45,85,220,255};
    if (!strcmp(s,"black")) return (SDL_Color){0,0,0,255};
    if (!strcmp(s,"white")) return (SDL_Color){255,255,255,255};
    if (!strcmp(s,"transparent")) return (SDL_Color){0,0,0,0};
    if (!strcmp(s,"green")) return (SDL_Color){0,128,0,255};
    if (!strcmp(s,"gray") || !strcmp(s,"grey")) return (SDL_Color){128,128,128,255};
    if (!strncmp(s,"rgb(",4) || !strncmp(s,"rgba(",5)) {
        bool alpha=s[3]=='a'; const char *p=s+(alpha?5:4); unsigned char channels[4]={0,0,0,255};
        for(int i=0;i<(alpha?4:3);i++) {
            char *end; double value=strtod(p,&end);
            if(end==p || !isfinite(value)) return fallback;
            p=end;
            while(*p==' ' || *p=='\t') p++;
            if(*p=='%') { value*=i==3?0.01:2.55; p++; }
            if(i==3) value*=255;
            if(value<0 || value>255) return fallback;
            channels[i]=(unsigned char)floor(value+0.5);
            while(*p==' ' || *p=='\t') p++;
            if(i+1<(alpha?4:3)) { if(*p++!=',') return fallback; }
            else if(*p++!=')') return fallback;
        }
        while(*p==' ' || *p=='\t') p++;
        if(*p) return fallback;
        return (SDL_Color){channels[0],channels[1],channels[2],channels[3]};
    }
    size_t n=strlen(s);
    if (*s!='#' || (n!=4 && n!=7 && n!=9)) return fallback;
    unsigned char v[4]={0,0,0,255};
    for (size_t i=0;i<(n==9 ? 4u:3u);i++) {
        int a=hex((unsigned char)s[1+(n==4 ? i:2*i)]),b=n==4 ? a:hex((unsigned char)s[2+2*i]);
        if (a<0 || b<0) return fallback;
        v[i]=(unsigned char)(a*16+b);
    }
    return (SDL_Color){v[0],v[1],v[2],v[3]};
}
static void clear(CeRichLayout *l) {
    for(size_t i=0;i<l->fragment_count;i++) {
        if(l->fragments[i].texture) SDL_DestroyTexture(l->fragments[i].texture);
        TTF_DestroyText(l->fragments[i].text);
    }
    for(size_t i=0;i<FONT_COUNT;i++) if(l->fonts[i]) TTF_CloseFont(l->fonts[i]);
    for(size_t i=0;i<FONT_COUNT;i++) if(l->fallbacks[i]) TTF_CloseFont(l->fallbacks[i]);
    free(l->fragments); free(l->clusters); free(l->rows); free(l->snapshot);
    if(l->engine) TTF_DestroyRendererTextEngine(l->engine);
}
CeRichLayout *ce_rich_layout_create(SDL_Renderer *renderer, TTF_Font *font) {
    if(!renderer || !font) return NULL;
    CeRichLayout *l=calloc(1,sizeof *l);
    if(l) { l->renderer=renderer; l->base=font; l->theme=ce_rich_theme_default(false); }
    return l;
}
void ce_rich_layout_destroy(CeRichLayout *l) { if(l) { clear(l); free(l); } }
void ce_rich_layout_set_fallback(CeRichLayout *l,TTF_Font *fallback) {
    if(l && l->fallback!=fallback) { l->fallback=fallback; free(l->snapshot); l->snapshot=NULL; }
}
const char *ce_rich_layout_error(const CeRichLayout *l) { return l ? l->error : "No rich layout"; }
float ce_rich_layout_height(const CeRichLayout *l) { return l ? l->height:0; }
size_t ce_rich_layout_generation(const CeRichLayout *l) { return l ? l->generation:0; }
static TTF_Font *font_for(CeRichLayout *l,unsigned key) {
    if(key>=FONT_COUNT) return NULL;
    if(!l->fonts[key]) {
        static const float scale[]={1,1.75f,1.48f,1.27f,1.12f,1.04f,1};
        bool reduced=key>=112;
        bool emoji=reduced?key>=120:key>=56;
        unsigned style_key=reduced?key%8:key%56;
        float factor=reduced?.72f:scale[style_key/8];
        TTF_Font *source=emoji && l->fallback?l->fallback:l->base;
        TTF_Font *secondary=emoji?l->base:l->fallback;
        TTF_Font *f=TTF_CopyFont(source);
        if(!f) return NULL;
        if(!TTF_SetFontSize(f,l->font_size*factor)) { TTF_CloseFont(f); return NULL; }
        unsigned bits=key%8;
        /* Decorations are drawn explicitly: this keeps underline placement
         * consistent across renderer text engines and derived font metrics. */
        TTF_SetFontStyle(f, ((bits&1)?TTF_STYLE_BOLD:0)|((bits&2)?TTF_STYLE_ITALIC:0));
        if(secondary) {
            TTF_Font *alternative=TTF_CopyFont(secondary);
            if(alternative) TTF_SetFontStyle(alternative,TTF_GetFontStyle(f));
            if(!alternative || !TTF_SetFontSize(alternative,l->font_size*factor) || !TTF_AddFallbackFont(f,alternative)) {
                if(alternative) TTF_CloseFont(alternative);
                TTF_CloseFont(f); return NULL;
            }
            l->fallbacks[key]=alternative;
        }
        l->fonts[key]=f;
    }
    return l->fonts[key];
}
static TTF_Text *make_text(CeRichLayout *l, unsigned key,const char *s,size_t n,SDL_Color color) {
    TTF_Font *f=font_for(l,key);
    if(!f || n>INT_MAX) return NULL;
    TTF_Text *t=TTF_CreateText(l->engine,f,s,n);
    if(t && !TTF_SetTextColor(t,color.r,color.g,color.b,color.a)) { TTF_DestroyText(t); return NULL; }
    return t;
}
static bool fragment(CeRichLayout *l,TTF_Text *t,SDL_FRect r,bool hi,SDL_Color bg,bool atom) {
    SDL_Texture *texture=NULL;
    if(t && (TTF_GetFontStyle(TTF_GetTextFont(t))&TTF_STYLE_ITALIC)) {
        /* SDL_ttf 3.2.2's renderer atlas produces corrupted synthetic italic
         * glyphs on the software renderer. The surface path has the same
         * HarfBuzz metrics and correct glyphs. Cache its texture once, keeping
         * TTF_Text for the identical shaped geometry (including caret hits). */
        SDL_Color color; int w,h;
        if(!TTF_GetTextColor(t,&color.r,&color.g,&color.b,&color.a) || !TTF_GetTextSize(t,&w,&h)) goto failed;
        SDL_Surface *surface=TTF_RenderText_Blended(TTF_GetTextFont(t),t->text,strlen(t->text),color);
        if(!surface) goto failed;
        if(surface->w!=w || surface->h!=h || surface->pitch<0 || surface->h<0 ||
           (size_t)surface->h>MAX_TEXTURE_BYTES/(size_t)(surface->pitch?surface->pitch:1) ||
           (size_t)surface->pitch*(size_t)surface->h>MAX_TEXTURE_BYTES-l->texture_bytes) {
            SDL_DestroySurface(surface); error(l,"Italic surface metrics or texture budget do not match layout"); goto failed;
        }
        size_t bytes=(size_t)surface->pitch*(size_t)surface->h;
        texture=SDL_CreateTextureFromSurface(l->renderer,surface);SDL_DestroySurface(surface);
        if(!texture) goto failed;
        l->texture_bytes+=bytes;
    }
    if(!reserve((void**)&l->fragments,&l->fragment_cap,l->fragment_count+1,sizeof(Fragment))) goto failed;
    l->fragments[l->fragment_count++]=(Fragment){.text=t,.texture=texture,.rect=r,.background=bg,.highlighted=hi,.atom=atom};
    return true;
failed:
    if(texture) SDL_DestroyTexture(texture);TTF_DestroyText(t);return false;
}
static bool cluster(CeRichLayout *l,size_t a,size_t b,SDL_FRect r,size_t row) {
    if(!reserve((void**)&l->clusters,&l->cluster_cap,l->cluster_count+1,sizeof(Cluster))) return false;
    l->clusters[l->cluster_count++]=(Cluster){a,b,r,row}; return true;
}
static bool row(CeRichLayout *l,size_t a,size_t b,SDL_FRect rect) {
    if(!reserve((void**)&l->rows,&l->row_cap,l->row_count+1,sizeof(Row))) return false;
    l->rows[l->row_count++]=(Row){a,b,rect}; return true;
}
static bool same_unit(const Unit *a,const Unit *b) {
    return a->font==b->font && a->shift==b->shift && color_eq(a->color,b->color) && a->highlight==b->highlight && color_eq(a->background,b->background);
}
static bool supported_direction(const char *s) {
    const unsigned char *p=(const unsigned char*)s;
    while(*p) {
        Uint32 c=*p++;
        if(c>=0xf0) { c=(c&7)<<18; c|=(*p++&63)<<12; c|=(*p++&63)<<6; c|=*p++&63; }
        else if(c>=0xe0) { c=(c&15)<<12; c|=(*p++&63)<<6; c|=*p++&63; }
        else if(c>=0xc0) { c=(c&31)<<6; c|=*p++&63; }
        if((c>=0x590 && c<=0x8ff) || (c>=0xfb1d && c<=0xfdff) || (c>=0xfe70 && c<=0xfeff) ||
           (c>=0x10800 && c<=0x10fff) || (c>=0x1e800 && c<=0x1eeff) ||
           (c>=0x202a && c<=0x202e) || (c>=0x2066 && c<=0x2069) || c==0x200f || c==0x061c) return false;
    }
    return true;
}
/* Shape emoji sequences with the emoji font as primary. A generic fallback
 * chain can split a ZWJ sequence because the CJK font itself contains ZWJ. */
static bool emoji_cluster(const char *text,size_t length) {
    const char *end=text+length; bool emoji=false;
    while(text<end) {
        Uint32 cp=SDL_StepUTF8(&text,NULL);
        if(cp==0xfe0e) return false;
        if((cp>=0x1f000 && cp<=0x1faff) || (cp>=0x2600 && cp<=0x27bf) || cp==0xfe0f || cp==0x20e3) emoji=true;
    }
    return emoji;
}
static bool build_units(CeRichLayout *l,const CeDocument *doc,const CeBlockInfo *b,const char *s,
                         size_t start,size_t end,Unit **out,size_t *count) {
    *out=NULL; *count=0; size_t cap=0;
    unsigned heading=b->kind==CE_BLOCK_HEADING ? (b->level>6?6:b->level):0;
    TTF_Text *shape=make_text(l,heading*8+(heading?1:0),s+start,end-start,l->theme.text);
    if(!shape) return false;
    size_t at=start;
    while(at<end) {
        TTF_SubString sub;
        if(!TTF_GetTextSubString(shape,(int)(at-start),&sub) || sub.length<=0) goto bad;
        size_t next=start+(size_t)sub.offset+(size_t)sub.length;
        if(next<=at || next>end) goto bad;
        CeStyle st=ce_document_style_at(doc,at);
        unsigned flags=((st.marks&CE_MARK_BOLD)||heading?1:0)|((st.marks&CE_MARK_ITALIC)?2:0)|((st.marks&(CE_MARK_UNDERLINE|CE_MARK_LINK))?4:0);
        SDL_Color color=st.marks&CE_MARK_LINK?l->theme.link:l->theme.text;
        if(st.marks&CE_MARK_COLOR) color=css_color(st.color,color);
        if(!reserve((void**)out,&cap,*count+1,sizeof(Unit))) goto bad;
        unsigned font=heading*8+flags;
        if(l->fallback && emoji_cluster(s+at,next-at)) font+=56;
        (*out)[(*count)++]=(Unit){at,next,font,color,css_color(st.highlight,l->theme.highlight),(st.marks&CE_MARK_HIGHLIGHT)!=0,0,0};
        at=next;
    }
    TTF_DestroyText(shape);
    shape=NULL;
    /* Shape again with each actual derived font. Bold/italic can have different
     * ligature boundaries from the neutral run used to normalize mark edges. */
    Unit *measured=NULL; size_t measured_count=0,measured_cap=0;
    for(size_t i=0;i<*count;) {
        size_t j=i+1;
        while(j<*count && same_unit(&(*out)[i],&(*out)[j])) j++;
        Unit prototype=(*out)[i]; size_t run_end=(*out)[j-1].end;
        shape=make_text(l,prototype.font,s+prototype.start,run_end-prototype.start,prototype.color);
        if(!shape) { free(measured); goto bad; }
        size_t at=prototype.start;
        while(at<run_end) {
            TTF_SubString sub,next;
            if(!TTF_GetTextSubString(shape,(int)(at-prototype.start),&sub) || sub.length<=0) { free(measured); goto bad; }
            size_t end_at=prototype.start+(size_t)sub.offset+(size_t)sub.length;
            if(end_at<=at || end_at>run_end || !TTF_GetTextSubString(shape,(int)(end_at-prototype.start),&next)) { free(measured); goto bad; }
            Unit value=prototype; value.start=at; value.end=end_at;
            value.width=(float)(next.rect.x-sub.rect.x);
            if(value.width<0 || !reserve((void**)&measured,&measured_cap,measured_count+1,sizeof(Unit))) { free(measured); goto bad; }
            measured[measured_count++]=value;
            at=end_at;
        }
        TTF_DestroyText(shape); shape=NULL;
        i=j;
    }
    free(*out); *out=measured; *count=measured_count;
    return true;
bad:
    if(shape) TTF_DestroyText(shape);
    free(*out); *out=NULL; *count=0; return false;
}
static bool text_line(CeRichLayout *l,const char *s,Unit *u,size_t first,size_t end,
                       float indent,float available,const char *alignment,bool justify,
                       float *y,size_t empty_offset,unsigned empty_font) {
    size_t fstart=l->fragment_count,cstart=l->cluster_count,row_index=l->row_count;
    float x=0,max_ascent=0,max_descent=0;
    for(size_t i=first;i<end;) {
        size_t j=i+1;
        while(j<end && same_unit(&u[i],&u[j]) && (!justify || s[u[j-1].end-1]!=' ')) j++;
        TTF_Text *t=make_text(l,u[i].font,s+u[i].start,u[j-1].end-u[i].start,u[i].color);
        if(!t) return false;
        int w,h;
        if(!TTF_GetTextSize(t,&w,&h)) { TTF_DestroyText(t); return false; }
        TTF_Font *font=font_for(l,u[i].font);
        float ascent=(float)TTF_GetFontAscent(font),descent=(float)h-ascent;
        if(ascent>max_ascent) max_ascent=ascent;
        if(descent>max_descent) max_descent=descent;
        if(!fragment(l,t,(SDL_FRect){x,ascent,(float)w,(float)h},u[i].highlight,u[i].background,false)) return false;
        Fragment *part=&l->fragments[l->fragment_count-1];
        part->underline=(u[i].font%8 & 4)!=0; part->foreground=u[i].color;
        part->underline_y=ascent+fmaxf(1,l->font_size*.07f);
        size_t at=u[i].start;
        while(at<u[j-1].end) {
            TTF_SubString sub;
            if(!TTF_GetTextSubString(t,(int)(at-u[i].start),&sub) || sub.length<=0) return false;
            size_t next=u[i].start+(size_t)sub.offset+(size_t)sub.length;
            if(next<=at || next>u[j-1].end) return false;
            if(!cluster(l,at,next,(SDL_FRect){x+(float)sub.rect.x,ascent,(float)sub.rect.w,(float)h},row_index)) return false;
            at=next;
        }
        x+=(float)w;
        i=j;
    }
    if(first==end) {
        TTF_Font *font=font_for(l,empty_font);
        if(!font) return false;
        max_ascent=(float)TTF_GetFontAscent(font);
        max_descent=(float)TTF_GetFontHeight(font)-max_ascent;
    }
    float height=ceilf(max_ascent+max_descent+fmaxf(3,l->font_size*.17f));
    float shift=0;
    if(!strcmp(alignment,"center")) shift=fmaxf(0,(available-x)/2);
    else if(!strcmp(alignment,"right")) shift=fmaxf(0,available-x);
    /* Justification expands inter-word spaces on wrapped, non-final lines.
     * CJK lines without spaces retain their shaped advances. */
    size_t spaces=0;
    if(justify && x<available)
        for(size_t i=cstart;i<l->cluster_count;i++)
            if(l->clusters[i].end==l->clusters[i].start+1 && s[l->clusters[i].start]==' ') spaces++;
    float stretch=spaces ? (available-x)/(float)spaces:0,extra=0;
    for(size_t i=fstart;i<l->fragment_count;i++) {
        Fragment *f=&l->fragments[i];
        f->rect.x+=indent+shift+extra;
        f->rect.y=*y+max_ascent-f->rect.y;
        if(stretch && f->text->text && *f->text->text && f->text->text[strlen(f->text->text)-1]==' ') extra+=stretch;
    }
    extra=0;
    for(size_t i=cstart;i<l->cluster_count;i++) {
        Cluster *c=&l->clusters[i];
        c->rect.x+=indent+shift+extra; c->rect.y=*y; c->rect.h=height;
        if(stretch && c->end==c->start+1 && s[c->start]==' ') { c->rect.w+=stretch; extra+=stretch; }
    }
    float line_width=spaces ? available:x;
    size_t a=first==end?empty_offset:u[first].start,b=first==end?empty_offset:u[end-1].end;
    if(first==end && !cluster(l,a,b,(SDL_FRect){indent+shift,*y,0,height},row_index)) return false;
    if(!row(l,a,b,(SDL_FRect){indent+shift,*y,line_width,height})) return false;
    *y+=height;
    return true;
}
static bool layout_segment(CeRichLayout *l,const CeDocument *doc,const CeBlockInfo *b,const char *s,
                            size_t start,size_t end,float indent,float *y) {
    Unit *units=NULL; size_t count=0;
    if(!build_units(l,doc,b,s,start,end,&units,&count)) return false;
    float available=fmaxf(l->font_size,l->width-indent);
    if(!count) {
        unsigned heading=b->kind==CE_BLOCK_HEADING ? (b->level>6?6:b->level):0;
        bool ok=text_line(l,s,NULL,0,0,indent,available,b->alignment,false,y,start,heading*8+(heading?1:0));
        free(units); return ok;
    }
    size_t first=0;
    while(first<count) {
        size_t end_unit=first; float width=0; size_t wordbreak=first;
        while(end_unit<count) {
            float next=width+units[end_unit].width;
            if(next>available && end_unit>first) break;
            width=next;
            end_unit++;
            if(s[units[end_unit-1].end-1]==' ' || s[units[end_unit-1].end-1]=='\t') wordbreak=end_unit;
        }
        if(end_unit<count && wordbreak>first) end_unit=wordbreak;
        bool justify=!strcmp(b->alignment,"justify") && end_unit<count;
        if(!text_line(l,s,units,first,end_unit,indent,available,b->alignment,justify,y,start,0)) { free(units); return false; }
        first=end_unit;
    }
    free(units); return true;
}
static bool append(char **s,const char *part) {
    size_t a=*s?strlen(*s):0,b=strlen(part);
    if(a>SIZE_MAX-b-1) return false;
    char *p=realloc(*s,a+b+1);
    if(!p) return false;
    memcpy(p+a,part,b+1); *s=p; return true;
}
static bool append_field(char **s,const cJSON *attrs,const char *field,const char *prefix) {
    const char *v=jstr(attrs,field);
    if(!*v) return true;
    return append(s,prefix) && append(s,v);
}
static char *atom_label(const cJSON *n) {
    const char *type=jstr(n,"type");
    cJSON *a=jfield(n,"attrs"); char *s=NULL;
    if(!strcmp(type,"zoteroCitationBlock")) {
        char number[64]="Citation"; cJSON *v=jfield(a,"endnoteNumber");
        if(cJSON_IsNumber(v)) snprintf(number,sizeof number,"Citation [%g]",v->valuedouble);
        if(!append(&s,number) || !append_field(&s,a,"author"," · ") || !append_field(&s,a,"year"," · ") ||
           !append_field(&s,a,"title","\n") || !append_field(&s,a,"page","\np. ") || !append_field(&s,a,"note","\n")) goto bad;
    } else if(!strcmp(type,"zoteroEndnoteSection")) {
        if(!append(&s,"References") || !append_field(&s,a,"style"," · ")) goto bad;
        cJSON *entries=cJSON_Parse(jstr(a,"entriesJson"));
        if(entries && cJSON_IsArray(entries)) {
            cJSON *entry;
            cJSON_ArrayForEach(entry,entries) {
                const char *html=jstr(entry,"entryHtml"),*title=jstr(entry,"title");
                char *text=NULL;
                if(*html) {
                    CeDocument *doc=ce_document_new(); char message[256];
                    if(doc && ce_document_import(doc,html,CE_FORMAT_HTML,message,sizeof message)) text=dupstr(ce_document_text(doc));
                    ce_document_free(doc);
                }
                const char *value=text?text:(*title?title:(*html?html:jstr(entry,"itemKey")));
                bool ok=append(&s,"\n") && append(&s,*value?value:"Reference");
                free(text);
                if(!ok) { cJSON_Delete(entries); goto bad; }
            }
        }
        cJSON_Delete(entries);
    } else {
        if(!append(&s,"Unsupported block: ") || !append(&s,type)) goto bad;
    }
    return s;
bad: free(s); return NULL;
}
static bool collect_labels(const cJSON *n,BlockLabel *labels,size_t cap,size_t *index,
                            const char *marker,bool *used,unsigned depth,bool controlled) {
    if(depth>128) return false;
    const char *type=jstr(n,"type"); cJSON *content=jfield(n,"content");
    bool text=!strcmp(type,"paragraph") || !strcmp(type,"heading");
    bool atom=!strcmp(type,"zoteroCitationBlock") || !strcmp(type,"zoteroEndnoteSection") ||
              (!content && strcmp(type,"doc"));
    if(text || atom) {
        if(*index>=cap) return false;
        BlockLabel *label=&labels[(*index)++];
        if(marker && used && !*used) { label->marker=dupstr(marker); *used=true; if(!label->marker) return false; }
        if(atom) {
            if(controlled && (!strcmp(type,"zoteroCitationBlock") || !strcmp(type,"zoteroEndnoteSection"))) label->citation=n;
            else { label->label=atom_label(n); if(!label->label) return false; }
        }
        return true;
    }
    bool ordered=!strcmp(type,"orderedList"),bullet=!strcmp(type,"bulletList");
    if(ordered || bullet) {
        cJSON *start=jfield(jfield(n,"attrs"),"start");
        double number=cJSON_IsNumber(start)?start->valuedouble:1;
        cJSON *item;
        cJSON_ArrayForEach(item,content) {
            char prefix[64]; bool item_used=false;
            if(ordered) snprintf(prefix,sizeof prefix,"%.0f.",number++);
            else snprintf(prefix,sizeof prefix,"\xe2\x80\xa2");
            if(!collect_labels(item,labels,cap,index,prefix,&item_used,depth+1,controlled)) return false;
        }
        return true;
    }
    cJSON *child;
    cJSON_ArrayForEach(child,content)
        if(!collect_labels(child,labels,cap,index,marker,used,depth+1,controlled)) return false;
    return true;
}
static bool layout_atom(CeRichLayout *l,const CeBlockInfo *b,const char *label,float indent,float *y) {
    if(!supported_direction(label)) return error(l,"Rich text bidirectional layout is not yet supported; keep the plain view for this document");
    TTF_Text *text=make_text(l,0,label,strlen(label),l->theme.text);
    if(!text) return false;
    float width=fmaxf(l->font_size,l->width-indent),padding=8;
    if(!TTF_SetTextWrapWidth(text,(int)fmaxf(1,width-padding*2))) { TTF_DestroyText(text); return false; }
    int w,h;
    if(!TTF_GetTextSize(text,&w,&h)) { TTF_DestroyText(text); return false; }
    SDL_FRect rect={indent,*y,width,(float)h+padding*2};
    if(!fragment(l,text,rect,false,l->theme.atom_background,true) ||
       !cluster(l,b->start,b->end,rect,l->row_count) || !row(l,b->start,b->end,rect)) return false;
    *y+=rect.h; return true;
}
/* Citation cell coordinates are presentation-only. They deliberately do not
 * create document clusters: one citation/reference section remains one atom. */
typedef struct { char *text; Unit *units; size_t count; } CitationText;
static bool citation_tick(CeRichLayout *l) {
    if(l->citation_work==CITATION_MAX_WORK) return error(l,"Citation layout work limit exceeded");
    l->citation_work++; return true;
}
static bool citation_supported(const CeCslOutput *output) {
    if(!output) return false;
    for(size_t i=0;i<ce_csl_output_run_count(output);i++) {
        const CeCslOutputRun *run=ce_csl_output_run(output,i);
        if((run->format&(CE_CSL_SMALL_CAPS|CE_CSL_LIGHT|CE_CSL_OBLIQUE)) || !supported_direction(run->text)) return false;
    }
    return true;
}
static bool citation_cache_supported(const CeCitationCache *cache) {
    if(ce_citation_cache_kind(cache)==CE_CITATION_CACHE_CITATION)
        return citation_supported(ce_citation_cache_citation_output(cache));
    for(size_t i=0;i<ce_citation_cache_entry_count(cache);i++) {
        const CeCitationCacheEntry *entry=ce_citation_cache_entry(cache,i);
        for(size_t j=0;j<entry->row_count;j++) {
            const CeCitationCacheRow *r=&entry->rows[j];
            if(!citation_supported(r->body) || (r->left && !citation_supported(r->left))) return false;
        }
    }
    return true;
}
static void citation_text_free(CitationText *text) {
    free(text->text); free(text->units); memset(text,0,sizeof *text);
}
static bool citation_newline(const CitationText *text,size_t index) {
    return index<text->count && (text->text[text->units[index].start]=='\n' || text->text[text->units[index].start]=='\r');
}
static bool citation_text_build(CeRichLayout *l,const CeCslOutput *output,CitationText *out) {
    size_t length=ce_csl_output_text_bytes(output),at=0,capacity=0;
    CitationText t={0}; Unit *measured=NULL; size_t measured_count=0,measured_capacity=0;
    TTF_Text *shape=NULL;
    t.text=malloc(length+1);
    if(!t.text) goto bad;
    for(size_t i=0;i<ce_csl_output_run_count(output);i++) {
        const CeCslOutputRun *run=ce_csl_output_run(output,i);
        memcpy(t.text+at,run->text,run->byte_length); at+=run->byte_length;
    }
    t.text[length]=0;
    size_t ri=0,run_start=0;
    const CeCslOutputRun *run=ce_csl_output_run(output,0);
    at=0;
    while(at<length) {
        size_t line_end=at;
        while(line_end<length && t.text[line_end]!='\n' && t.text[line_end]!='\r') line_end++;
        size_t line_start=at;
        if(line_end>at) {
            shape=make_text(l,0,t.text+at,line_end-at,l->theme.text);
            if(!shape) goto bad;
            while(at<line_end) {
                TTF_SubString sub;
                if(!citation_tick(l) || !TTF_GetTextSubString(shape,(int)(at-line_start),&sub) || sub.length<=0) goto bad;
                size_t end=line_start+(size_t)sub.offset+(size_t)sub.length;
                if(end<=at || end>line_end) goto bad;
                while(run && run_start+run->byte_length<=at) { run_start+=run->byte_length; run=ce_csl_output_run(output,++ri); }
                if(!run) goto bad;
                unsigned flags=((run->format&CE_CSL_BOLD)?1u:0u)|((run->format&CE_CSL_ITALIC)?2u:0u)|
                    (((run->format&CE_CSL_UNDERLINE)||run->href)?4u:0u);
                bool reduced=(run->format&(CE_CSL_SUPERSCRIPT|CE_CSL_SUBSCRIPT))!=0;
                bool emoji=l->fallback && emoji_cluster(t.text+at,end-at);
                unsigned font=reduced?112+flags+(emoji?8:0):flags+(emoji?56:0);
                float shift=(run->format&CE_CSL_SUPERSCRIPT)?-.35f*l->font_size:
                    (run->format&CE_CSL_SUBSCRIPT)?.20f*l->font_size:0;
                if(!reserve((void**)&t.units,&capacity,t.count+1,sizeof(Unit))) goto bad;
                t.units[t.count++]=(Unit){.start=at,.end=end,.font=font,
                    .color=run->href?l->theme.link:l->theme.text,.shift=shift};
                at=end;
            }
            TTF_DestroyText(shape); shape=NULL;
        }
        if(at<length) {
            size_t end=at+1;
            if(t.text[at]=='\r' && end<length && t.text[end]=='\n') end++;
            if(!citation_tick(l) || !reserve((void**)&t.units,&capacity,t.count+1,sizeof(Unit))) goto bad;
            t.units[t.count++]=(Unit){.start=at,.end=end,.color=l->theme.text}; at=end;
        }
    }
    /* Re-shape each actual style/emoji run. Neutral shaping above keeps an
     * original combining/ZWJ cluster together even across a formatting edge. */
    for(size_t i=0;i<t.count;) {
        if(citation_newline(&t,i)) {
            if(!reserve((void**)&measured,&measured_capacity,measured_count+1,sizeof(Unit))) goto bad;
            measured[measured_count++]=t.units[i++]; continue;
        }
        size_t j=i+1;
        while(j<t.count && !citation_newline(&t,j) && same_unit(&t.units[i],&t.units[j])) j++;
        Unit prototype=t.units[i]; size_t end=t.units[j-1].end;
        shape=make_text(l,prototype.font,t.text+prototype.start,end-prototype.start,prototype.color);
        if(!shape) goto bad;
        for(size_t p=prototype.start;p<end;) {
            TTF_SubString sub,next;
            if(!citation_tick(l) || !TTF_GetTextSubString(shape,(int)(p-prototype.start),&sub) || sub.length<=0) goto bad;
            size_t next_at=prototype.start+(size_t)sub.offset+(size_t)sub.length;
            if(next_at<=p || next_at>end || !TTF_GetTextSubString(shape,(int)(next_at-prototype.start),&next)) goto bad;
            Unit u=prototype; u.start=p; u.end=next_at; u.width=(float)(next.rect.x-sub.rect.x);
            if(u.width<0 || !reserve((void**)&measured,&measured_capacity,measured_count+1,sizeof(Unit))) goto bad;
            measured[measured_count++]=u; p=next_at;
        }
        TTF_DestroyText(shape); shape=NULL; i=j;
    }
    free(t.units); t.units=measured; t.count=measured_count; *out=t; return true;
bad:
    if(shape) TTF_DestroyText(shape);
    free(measured); citation_text_free(&t); return false;
}
static float citation_baseline(CeRichLayout *l) {
    TTF_Font *font=font_for(l,0);
    return font?ceilf((float)TTF_GetFontHeight(font)+fmaxf(3,l->font_size*.17f)):0;
}
static bool citation_line_width(CeRichLayout *l,const CitationText *t,size_t begin,size_t end,float *width) {
    *width=0;
    for(size_t i=begin;i<end;) {
        size_t j=i+1;while(j<end && same_unit(&t->units[i],&t->units[j])) j++;
        const Unit *u=&t->units[i];size_t bytes=t->units[j-1].end-u->start;
        if(bytes>CITATION_MAX_WORK-l->citation_work) return error(l,"Citation layout measurement limit exceeded");
        l->citation_work+=bytes;
        TTF_Text *text=make_text(l,u->font,t->text+u->start,bytes,u->color);
        int w,h;
        if(!text) return false;
        bool ok=TTF_GetTextSize(text,&w,&h);TTF_DestroyText(text);
        if(!ok) return false;
        *width+=(float)w;i=j;
    }
    return true;
}
static bool citation_line(CeRichLayout *l,const CitationText *t,size_t begin,size_t end,
                          float x,float *y,uint32_t spacing) {
    size_t first_fragment=l->fragment_count;
    TTF_Font *base=font_for(l,0);
    if(!base) return false;
    float ascent=(float)TTF_GetFontAscent(base),descent=(float)TTF_GetFontHeight(base)-ascent;
    for(size_t i=begin;i<end;) {
        size_t j=i+1;
        while(j<end && same_unit(&t->units[i],&t->units[j])) j++;
        const Unit *u=&t->units[i];
        if(l->fragment_count>=CITATION_MAX_FRAGMENTS) return error(l,"Citation layout fragment limit exceeded");
        TTF_Text *text=make_text(l,u->font,t->text+u->start,t->units[j-1].end-u->start,u->color);
        if(!text) return false;
        int w,h;
        if(!TTF_GetTextSize(text,&w,&h)) { TTF_DestroyText(text); return false; }
        float a=(float)TTF_GetFontAscent(font_for(l,u->font));
        ascent=fmaxf(ascent,a-u->shift); descent=fmaxf(descent,(float)h-a+u->shift);
        if(!fragment(l,text,(SDL_FRect){x,a-u->shift,(float)w,(float)h},false,(SDL_Color){0},false)) return false;
        Fragment *f=&l->fragments[l->fragment_count-1];
        f->underline=(u->font%8&4)!=0; f->foreground=u->color;
        f->underline_y=a+fmaxf(1,l->font_size*.07f);
        x+=(float)w; i=j;
    }
    float natural=ceilf(ascent+descent+fmaxf(3,l->font_size*.17f));
    float height=natural*(float)spacing;
    if(!isfinite(height) || *y>CITATION_MAX_HEIGHT-height) return error(l,"Citation layout height limit exceeded");
    float baseline=*y+(height-natural)*.5f+ascent;
    for(size_t i=first_fragment;i<l->fragment_count;i++) l->fragments[i].rect.y=baseline-l->fragments[i].rect.y;
    *y+=height; return true;
}
static bool citation_cell(CeRichLayout *l,const CeCslOutput *output,float x,float top,float width,
                          float first_indent,float continuation,uint32_t spacing,float *height) {
    CitationText text={0};
    if(!citation_text_build(l,output,&text)) return false;
    size_t first=0; float y=top; bool first_line=true,again=true;
    while(again) {
        float indent=first_line?first_indent:continuation;
        float available=fmaxf(1,width-indent),used=0; size_t end=first,word=first;
        while(end<text.count && !citation_newline(&text,end)) {
            if(used+text.units[end].width>available && end>first) break;
            used+=text.units[end].width; end++;
            unsigned char last=(unsigned char)text.text[text.units[end-1].end-1];
            if(last==' ' || last=='\t') word=end;
        }
        if(end<text.count && !citation_newline(&text,end) && word>first) end=word;
        /* Substring rectangles describe cluster ink bounds. Their differences
         * omit some run-edge padding that TTF_GetTextSize includes. Check the
         * exact pieces we will draw before accepting a line, especially across
         * many italic/normal/link boundaries. Never insert or trim whitespace. */
        float actual=0;
        if(!citation_line_width(l,&text,first,end,&actual)) { citation_text_free(&text); return false; }
        while(actual>available && end>first+1) {
            size_t cut=end-1,previous_word=first;
            for(size_t i=first+1;i<=cut;i++) {
                char last=text.text[text.units[i-1].end-1];
                if(last==' ' || last=='\t') previous_word=i;
            }
            end=previous_word>first?previous_word:cut;
            if(!citation_line_width(l,&text,first,end,&actual)) { citation_text_free(&text); return false; }
        }
        if(!citation_line(l,&text,first,end,x+indent,&y,spacing)) { citation_text_free(&text); return false; }
        bool newline=citation_newline(&text,end);
        first=end+(newline?1:0); first_line=false;
        again=first<text.count || (newline && first==text.count);
        if(newline && first==text.count) {
            if(!citation_line(l,&text,first,first,x+continuation,&y,spacing)) { citation_text_free(&text); return false; }
            again=false;
        }
    }
    *height=y-top; citation_text_free(&text); return true;
}
static bool citation_measure(CeRichLayout *l,const CeCslOutput *output,float *width) {
    CitationText text={0};
    if(!citation_text_build(l,output,&text)) return false;
    float maximum=0;
    for(size_t i=0;i<text.count;) {
        size_t end=i;while(end<text.count && !citation_newline(&text,end)) end++;
        float line=0;
        if(!citation_line_width(l,&text,i,end,&line)) { citation_text_free(&text); return false; }
        maximum=fmaxf(maximum,line);i=end+(end<text.count?1:0);
    }
    *width=maximum; citation_text_free(&text); return true;
}
static bool citation_placeholder(CeRichLayout *l,const CeBlockInfo *b,bool bibliography,
                                 CeRichCitationStatus status,float indent,float *y) {
    const char *message=status==CE_RICH_CITATION_STALE?
        (bibliography?"参考文献已过期，请重新同步":"引用已过期，请重新同步"):
        status==CE_RICH_CITATION_UNSUPPORTED?
        (bibliography?"此参考文献的格式暂不支持显示":"此引用的格式暂不支持显示"):
        (bibliography?"参考文献尚未生成":"引用尚未生成");
    return layout_atom(l,b,message,indent,y);
}
static bool archive_cell(CeRichLayout *l,const CeCslOutput *output,float x,float y,float width,float *height) {
    if(citation_supported(output)) return citation_cell(l,output,x,y,width,0,0,1,height);
    CeCslOutput *fallback=ce_csl_output_new(NULL,l->error,sizeof l->error);
    const char *text="旧存档文字方向暂不支持显示";
    bool ok=fallback && ce_csl_output_append(fallback,text,strlen(text),0,NULL,l->error,sizeof l->error) &&
        citation_cell(l,fallback,x,y,width,0,0,1,height);
    ce_csl_output_free(fallback); return ok;
}
static bool layout_archive(CeRichLayout *l,const CeBlockInfo *b,const cJSON *node,float indent,float *y) {
    CeBibliographyArchive *archive=NULL;
    if(!ce_bibliography_archive_decode(node,NULL,&archive,l->error,sizeof l->error)) return false;
    float top=*y,padding=8,width=fmaxf(l->font_size,l->width-indent),inside=fmaxf(1,width-padding*2);
    float x=indent+padding,cursor=top+padding,label=0,gap=l->font_size*.5f,h=0;
    CeCslOutput *caption=ce_csl_output_new(NULL,l->error,sizeof l->error);
    const char *title=ce_bibliography_archive_diagnostics(archive)?
        "参考文献（旧存档；部分格式按原文显示）":"参考文献（旧存档）";
    bool ok=caption && ce_csl_output_append(caption,title,strlen(title),CE_CSL_BOLD,NULL,l->error,sizeof l->error) &&
        citation_cell(l,caption,x,cursor,inside,0,0,1,&h);
    ce_csl_output_free(caption);
    if(!ok) goto done;
    cursor+=h+l->font_size*.4f;
    for(size_t i=0;i<ce_bibliography_archive_count(archive);i++) {
        const CeBibliographyArchiveEntry *entry=ce_bibliography_archive_entry(archive,i);
        if(entry->label) {
            float measured=0;
            if(!citation_measure(l,entry->label,&measured)) { ok=false; goto done; }
            label=fmaxf(label,measured);
        }
    }
    label=fminf(label,inside*.25f);
    for(size_t i=0;i<ce_bibliography_archive_count(archive);i++) {
        const CeBibliographyArchiveEntry *entry=ce_bibliography_archive_entry(archive,i);
        if(i) cursor+=l->font_size*.4f;
        float entry_top=cursor,label_height=0,gutter=entry->label?fminf(label+gap,inside*.45f):0;
        float body_width=fmaxf(1,inside-gutter),body_x=x+gutter;
        if(entry->label && !archive_cell(l,entry->label,x,cursor,fmaxf(1,label),&label_height)) { ok=false; goto done; }
        float inner_label=0;
        for(size_t j=0;j<entry->row_count;j++) if(entry->rows[j].left && citation_supported(entry->rows[j].left)) {
            float measured=0;
            if(!citation_measure(l,entry->rows[j].left,&measured)) { ok=false; goto done; }
            inner_label=fmaxf(inner_label,measured);
        }
        inner_label=fminf(inner_label,body_width*.35f);
        for(size_t j=0;j<entry->row_count;j++) {
            const CeBibliographyArchiveRow *r=&entry->rows[j]; float left_height=0;
            if(r->kind==CE_CSL_BIBLIOGRAPHY_TWO_COLUMNS) {
                float inner_gutter=fminf(inner_label+gap,body_width*.45f);
                ok=archive_cell(l,r->left,body_x,cursor,fmaxf(1,inner_label),&left_height) &&
                    archive_cell(l,r->body,body_x+inner_gutter,cursor,fmaxf(1,body_width-inner_gutter),&h);
            } else {
                float inset=r->kind==CE_CSL_BIBLIOGRAPHY_INDENT?fminf(2*l->font_size,body_width*.4f):0;
                ok=archive_cell(l,r->body,body_x+inset,cursor,fmaxf(1,body_width-inset),&h);
            }
            if(!ok) goto done;
            cursor+=fmaxf(h,left_height);
        }
        cursor=fmaxf(cursor,entry_top+label_height);
        if(!isfinite(cursor) || cursor>CITATION_MAX_HEIGHT) { ok=error(l,"Archive exceeds citation layout height limit"); goto done; }
    }
    {
        SDL_FRect bounds={indent,top,width,cursor-top+padding};
        if(l->fragment_count>=CITATION_MAX_FRAGMENTS) { ok=error(l,"Archive layout fragment limit exceeded"); goto done; }
        ok=fragment(l,NULL,bounds,false,l->theme.atom_background,true) &&
           cluster(l,b->start,b->end,bounds,l->row_count) && row(l,b->start,b->end,bounds);
        if(ok) *y=top+bounds.h;
    }
done:
    ce_bibliography_archive_free(archive); return ok;
}
static bool layout_citation(CeRichLayout *l,const CeBlockInfo *b,const cJSON *node,
                            const CeRichCitationSource *source,float indent,float *y) {
    bool bibliography=!strcmp(jstr(node,"type"),"zoteroEndnoteSection");
    const CeCitationCache *cache=NULL;
    CeRichCitationStatus status=source?source->resolve(source->context,node,&cache,l->error,sizeof l->error):CE_RICH_CITATION_ABSENT;
    if(status==CE_RICH_CITATION_ERROR) return *l->error?false:error(l,"Unable to resolve citation display");
    if(status<CE_RICH_CITATION_ABSENT || status>CE_RICH_CITATION_ARCHIVED) return error(l,"Invalid citation display status");
    l->error[0]=0;
    if(status==CE_RICH_CITATION_ARCHIVED) {
        if(cache || !bibliography || !ce_bibliography_archive_eligible(node)) return error(l,"Invalid archived bibliography display source");
        return layout_archive(l,b,node,indent,y);
    }
    if(status!=CE_RICH_CITATION_VALID) return citation_placeholder(l,b,bibliography,status,indent,y);
    if(!cache || ce_citation_cache_kind(cache)!=(bibliography?CE_CITATION_CACHE_BIBLIOGRAPHY:CE_CITATION_CACHE_CITATION))
        return error(l,"Citation display cache has the wrong kind");
    if(!citation_cache_supported(cache)) return citation_placeholder(l,b,bibliography,CE_RICH_CITATION_UNSUPPORTED,indent,y);
    float top=*y,padding=8,width=fmaxf(l->font_size,l->width-indent),inside=fmaxf(1,width-padding*2);
    float x=indent+padding,cursor=top+padding,em=l->font_size;
    if(!bibliography) {
        float h;
        if(!citation_cell(l,ce_citation_cache_citation_output(cache),x,cursor,inside,0,0,1,&h)) return false;
        cursor+=h;
        /* The user's excerpt is separate from the processor's citation. It is
         * plain text, never reparsed as field markup or included in CSL output. */
        const char *note=jstr(jfield(node,"attrs"),"note");
        if(*note) {
            CeCslOutput *excerpt=ce_csl_output_new(NULL,l->error,sizeof l->error);
            bool ok=excerpt && ce_csl_output_append(excerpt,"摘录：",strlen("摘录："),0,NULL,l->error,sizeof l->error) &&
                ce_csl_output_append(excerpt,note,strlen(note),0,NULL,l->error,sizeof l->error);
            if(ok && !citation_supported(excerpt)) {
                ce_csl_output_free(excerpt); excerpt=ce_csl_output_new(NULL,l->error,sizeof l->error);
                const char *message="摘录文字方向暂不支持显示";
                ok=excerpt && ce_csl_output_append(excerpt,message,strlen(message),0,NULL,l->error,sizeof l->error);
            }
            if(ok) {
                cursor+=em*.3f;
                ok=citation_cell(l,excerpt,x,cursor,inside,0,0,1,&h);
                if(ok) cursor+=h;
            }
            ce_csl_output_free(excerpt);
            if(!ok) return false;
        }
    } else {
        const CeCslBibliographyOptions *options=ce_citation_cache_bibliography_options(cache);
        if(!options) return error(l,"Missing bibliography layout options");
        float label=0,gap=.5f*em;
        for(size_t i=0;i<ce_citation_cache_entry_count(cache);i++) {
            const CeCitationCacheEntry *entry=ce_citation_cache_entry(cache,i);
            if(!entry->visible) continue;
            for(size_t j=0;j<entry->row_count;j++) if(entry->rows[j].left) {
                float measured;
                if(!citation_measure(l,entry->rows[j].left,&measured)) return false;
                label=fmaxf(label,measured);
            }
        }
        /* Narrow viewports wrap labels inside one shared column. Margin mode
         * reserves its gutter inside the atom's bounds, keeping labels visible. */
        label=fminf(label,fmaxf(1,(inside-gap)*.45f));
        float gutter=label+gap;
        bool emitted=false;
        for(size_t i=0;i<ce_citation_cache_entry_count(cache);i++) {
            const CeCitationCacheEntry *entry=ce_citation_cache_entry(cache,i);
            if(!entry->visible) continue;
            if(emitted) cursor+=citation_baseline(l)*(float)options->line_spacing*(float)options->entry_spacing;
            if(!isfinite(cursor) || cursor>CITATION_MAX_HEIGHT) return error(l,"Bibliography spacing exceeds layout limit");
            for(size_t j=0;j<entry->row_count;j++) {
                const CeCitationCacheRow *r=&entry->rows[j]; float h=0,left_h=0;
                if(r->kind==CE_CSL_BIBLIOGRAPHY_TWO_COLUMNS) {
                    if(!citation_cell(l,r->left,x,cursor,fmaxf(1,label),0,0,options->line_spacing,&left_h) ||
                       !citation_cell(l,r->body,x+gutter,cursor,fmaxf(1,inside-gutter),0,0,options->line_spacing,&h)) return false;
                } else {
                    float inset=r->kind==CE_CSL_BIBLIOGRAPHY_INDENT?2*em:0;
                    float continued=options->hanging_indent?fminf(2*em,inside*.5f):0;
                    /* A margin layout's full-width blocks align with its body. */
                    if(options->alignment==CE_CSL_BIBLIOGRAPHY_ALIGN_MARGIN) inset+=gutter;
                    if(!citation_cell(l,r->body,x+inset,cursor,fmaxf(1,inside-inset),0,continued,options->line_spacing,&h)) return false;
                }
                cursor+=fmaxf(h,left_h);
            }
            emitted=true;
        }
        if(!emitted) cursor+=citation_baseline(l);
    }
    SDL_FRect bounds={indent,top,width,cursor-top+padding};
    if(l->fragment_count>=CITATION_MAX_FRAGMENTS) return error(l,"Citation layout fragment limit exceeded");
    if(!fragment(l,NULL,bounds,false,l->theme.atom_background,true) ||
       !cluster(l,b->start,b->end,bounds,l->row_count) || !row(l,b->start,b->end,bounds)) return false;
    *y=top+bounds.h; return true;
}

static bool update_layout(CeRichLayout *l,const CeDocument *doc,float width,const CeRichTheme *theme,
                          bool controlled,const CeRichCitationSource *source) {
    if(!l || !doc || !theme) return false;
    if(source && !source->resolve) return error(l,"Citation source has no resolver");
    if(!isfinite(width) || width<1 || width>1000000) return error(l,"Rich layout width must be between 1 and 1000000 pixels");
    const char *text=ce_document_text(doc);
    if(ce_document_length(doc)>INT_MAX) return error(l,"Document is too long for SDL_ttf byte offsets");
    if(!supported_direction(text)) return error(l,"Rich text bidirectional layout is not yet supported; keep the plain view for this document");
    char *snapshot=ce_document_export_json(doc);
    if(!snapshot) return error(l,"Unable to copy document for rich layout");
    float size=TTF_GetFontSize(l->base);
    if(l->snapshot && !strcmp(snapshot,l->snapshot) && width==l->width && size==l->font_size && theme_eq(theme,&l->theme) &&
       controlled==l->controlled_citations && (source!=NULL)==l->citation_source &&
       (!source || source->revision==l->citation_revision)) {
        free(snapshot); l->error[0]=0; return true;
    }
    CeRichLayout next={0};
    next.renderer=l->renderer; next.base=l->base; next.fallback=l->fallback; next.width=width; next.theme=*theme; next.font_size=size;
    next.snapshot=snapshot; next.text_length=ce_document_length(doc); next.generation=l->generation+1;
    next.controlled_citations=controlled; next.citation_source=source!=NULL;
    next.citation_revision=source?source->revision:0;
    next.engine=TTF_CreateRendererTextEngine(next.renderer);
    size_t count=ce_document_block_count(doc),index=0;
    BlockLabel *labels=calloc(count?count:1,sizeof *labels);
    cJSON *tree=cJSON_Parse(snapshot);
    bool ok=labels && tree && next.engine && collect_labels(tree,labels,count,&index,NULL,NULL,0,controlled);
    float y=0;
    for(size_t i=0;ok && i<count;i++) {
        CeBlockInfo b;
        if(!ce_document_block_info(doc,i,&b)) { ok=false; break; }
        float indent=fminf((float)b.depth*size*1.35f,fmaxf(0,width-size*3));
        float top=y;
        size_t first_row=next.row_count;
        if(labels[i].citation) ok=layout_citation(&next,&b,labels[i].citation,source,indent,&y);
        else if(labels[i].label) ok=layout_atom(&next,&b,labels[i].label,indent,&y);
        else {
            size_t start=b.start;
            do {
                size_t end=start;
                while(end<b.end && text[end]!='\n') end++;
                if(!layout_segment(&next,doc,&b,text,start,end,indent,&y)) { ok=false; break; }
                if(end==b.end) break;
                Row *r=&next.rows[next.row_count-1];
                if(!cluster(&next,end,end+1,(SDL_FRect){r->rect.x+r->rect.w,r->rect.y,2,r->rect.h},next.row_count-1)) { ok=false; break; }
                start=end+1;
            } while(start<=b.end);
        }
        if(ok && labels[i].marker) {
            TTF_Text *marker=make_text(&next,0,labels[i].marker,strlen(labels[i].marker),theme->text);
            int w=0,h=0;
            if(!marker || !TTF_GetTextSize(marker,&w,&h)) { TTF_DestroyText(marker); ok=false; }
            else ok=fragment(&next,marker,(SDL_FRect){fmaxf(0,indent-size*.3f-(float)w),top,(float)w,(float)h},false,(SDL_Color){0},false);
        }
        if(ok && i+1<count && next.row_count>first_row) {
            Row *r=&next.rows[next.row_count-1];
            ok=cluster(&next,b.end,b.end+1,(SDL_FRect){r->rect.x+r->rect.w,r->rect.y,2,r->rect.h},next.row_count-1);
        }
        y+=size*.3f;
    }
    next.height=y>0?y:ceilf(size*1.4f);
    if(ok && !next.row_count) {
        SDL_FRect empty={0,0,0,next.height};
        ok=row(&next,0,0,empty) && cluster(&next,0,0,empty,0);
    }
    cJSON_Delete(tree);
    if(labels) { for(size_t i=0;i<count;i++) { free(labels[i].marker); free(labels[i].label); } free(labels); }
    if(!ok) {
        error(l,*next.error?next.error:SDL_GetError());
        clear(&next); return false;
    }
    clear(l); *l=next; return true;
}
bool ce_rich_layout_update(CeRichLayout *l,const CeDocument *doc,float width,const CeRichTheme *theme) {
    return update_layout(l,doc,width,theme,false,NULL);
}
bool ce_rich_layout_update_citations(CeRichLayout *l,const CeDocument *doc,float width,
                                     const CeRichTheme *theme,const CeRichCitationSource *source) {
    return update_layout(l,doc,width,theme,true,source);
}
bool ce_rich_layout_caret_affinity(const CeRichLayout *l,size_t offset,bool trailing,SDL_FRect *rect) {
    if(!l || !rect || !l->row_count || offset>l->text_length) return false;
    if(trailing) for(size_t i=0;i<l->row_count;i++) {
        const Row *r=&l->rows[i];
        if(r->end==offset && r->start<offset) { *rect=(SDL_FRect){r->rect.x+r->rect.w,r->rect.y,1.5f,r->rect.h}; return true; }
    }
    for(size_t i=0;i<l->cluster_count;i++) {
        const Cluster *c=&l->clusters[i];
        if(offset==c->start || (offset>c->start && offset<c->end)) {
            *rect=(SDL_FRect){c->rect.x,c->rect.y,1.5f,c->rect.h}; return true;
        }
    }
    const Row *last=&l->rows[l->row_count-1];
    *rect=(SDL_FRect){last->rect.x+last->rect.w,last->rect.y,1.5f,last->rect.h}; return true;
}
bool ce_rich_layout_caret(const CeRichLayout *l,size_t offset,SDL_FRect *rect) {
    return ce_rich_layout_caret_affinity(l,offset,false,rect);
}
size_t ce_rich_layout_step(const CeRichLayout *l,size_t offset,int direction) {
    if(!l) return 0;
    if(direction<0) {
        size_t previous=0;
        for(size_t i=0;i<l->cluster_count;i++) {
            const Cluster *c=&l->clusters[i];
            if(c->start<offset) previous=c->start; else break;
        }
        return previous;
    }
    for(size_t i=0;i<l->cluster_count;i++) if(l->clusters[i].end>offset) return l->clusters[i].end;
    return l->text_length;
}
size_t ce_rich_layout_hit_test(const CeRichLayout *l,float x,float y) {
    if(!l || !l->row_count || !isfinite(x) || !isfinite(y)) return 0;
    if(y<l->rows[0].rect.y) return l->rows[0].start;
    size_t index=l->row_count-1;
    for(size_t i=0;i<l->row_count;i++)
        if(y<l->rows[i].rect.y+l->rows[i].rect.h) { index=i; break; }
    const Row *r=&l->rows[index];
    if(x<=r->rect.x) return r->start;
    for(size_t i=0;i<l->cluster_count;i++) {
        const Cluster *c=&l->clusters[i];
        if(c->row!=index || c->start>=r->end) continue;
        if(x<c->rect.x+c->rect.w*.5f) return c->start;
        if(x<c->rect.x+c->rect.w) return c->end;
    }
    return r->end;
}
void ce_rich_layout_selection_rects(const CeRichLayout *l,size_t a,size_t b,CeRichRectCallback callback,void *userdata) {
    if(!l || !callback || a>=b) return;
    SDL_FRect pending={0}; bool have=false; size_t row_index=0;
    for(size_t i=0;i<l->cluster_count;i++) {
        const Cluster *c=&l->clusters[i];
        if(c->end<=a || c->start>=b) continue;
        SDL_FRect r=c->rect; if(r.w<2) r.w=2;
        if(have && row_index==c->row && fabsf(pending.x+pending.w-r.x)<2) pending.w=r.x+r.w-pending.x;
        else { if(have) callback(&pending,userdata); pending=r; row_index=c->row; have=true; }
    }
    if(have) callback(&pending,userdata);
}
typedef struct { SDL_Renderer *renderer; float x,y; bool ok; } DrawSelection;
static void draw_selection(const SDL_FRect *rect,void *userdata) {
    DrawSelection *d=userdata; SDL_FRect r=*rect; r.x+=d->x; r.y+=d->y;
    if(!SDL_RenderFillRect(d->renderer,&r)) d->ok=false;
}
bool ce_rich_layout_draw(CeRichLayout *l,float x,float y,const SDL_FRect *viewport,size_t a,size_t b) {
    if(!l || !l->renderer) return false;
    SDL_Renderer *renderer=l->renderer;
    SDL_Rect old_clip; bool clipped=SDL_RenderClipEnabled(renderer);
    SDL_GetRenderClipRect(renderer,&old_clip);
    SDL_Color old_color; SDL_GetRenderDrawColor(renderer,&old_color.r,&old_color.g,&old_color.b,&old_color.a);
    SDL_BlendMode old_blend; SDL_GetRenderDrawBlendMode(renderer,&old_blend);
    bool ok=true;
    if(viewport) {
        SDL_Rect clip={(int)floorf(viewport->x),(int)floorf(viewport->y),(int)ceilf(viewport->w),(int)ceilf(viewport->h)};
        if(clipped) { SDL_Rect intersection; if(SDL_GetRectIntersection(&clip,&old_clip,&intersection)) clip=intersection; else clip=(SDL_Rect){0,0,0,0}; }
        if(!SDL_SetRenderClipRect(renderer,&clip)) ok=false;
    }
    SDL_SetRenderDrawBlendMode(renderer,SDL_BLENDMODE_BLEND);
    /* Backgrounds precede selection, which precedes glyphs. */
    for(size_t i=0;ok && i<l->fragment_count;i++) {
        Fragment *f=&l->fragments[i];
        if(!f->highlighted && !f->atom) continue;
        SDL_FRect r=f->rect; r.x+=x; r.y+=y;
        SDL_Color color=f->background;
        SDL_SetRenderDrawColor(renderer,color.r,color.g,color.b,color.a);
        ok=SDL_RenderFillRect(renderer,&r);
        if(f->atom) { color=l->theme.atom_border; SDL_SetRenderDrawColor(renderer,color.r,color.g,color.b,color.a); ok=ok && SDL_RenderRect(renderer,&r); }
    }
    SDL_Color color=l->theme.selection;
    SDL_SetRenderDrawColor(renderer,color.r,color.g,color.b,color.a);
    DrawSelection selection={renderer,x,y,true};
    ce_rich_layout_selection_rects(l,a,b,draw_selection,&selection);
    ok=ok && selection.ok;
    for(size_t i=0;ok && i<l->fragment_count;i++) {
        Fragment *f=&l->fragments[i];
        if(!f->text) continue;
        if(viewport && (y+f->rect.y+f->rect.h<viewport->y || y+f->rect.y>viewport->y+viewport->h)) continue;
        float padding=f->atom?8:0;
        if(f->texture) {
            SDL_FRect destination={x+f->rect.x+padding,y+f->rect.y+padding,f->rect.w,f->rect.h};
            ok=SDL_RenderTexture(renderer,f->texture,NULL,&destination);
        } else ok=TTF_DrawRendererText(f->text,x+f->rect.x+padding,y+f->rect.y+padding);
        if(ok && f->underline) {
            SDL_Color ink=f->foreground;
            SDL_SetRenderDrawColor(renderer,ink.r,ink.g,ink.b,ink.a);
            SDL_FRect line={x+f->rect.x,y+f->rect.y+f->underline_y,f->rect.w,fmaxf(1,l->font_size*.055f)};
            ok=SDL_RenderFillRect(renderer,&line);
        }
    }
    SDL_SetRenderClipRect(renderer,clipped?&old_clip:NULL);
    SDL_SetRenderDrawBlendMode(renderer,old_blend);
    SDL_SetRenderDrawColor(renderer,old_color.r,old_color.g,old_color.b,old_color.a);
    return ok;
}
