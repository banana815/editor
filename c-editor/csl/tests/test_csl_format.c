#include "csl_format.h"
#include <assert.h>
#include <stdlib.h>
#include <string.h>
static char error[256];
static CeCslXml *parse(const char *xml) { CeCslXml *x=ce_csl_xml_parse(xml,strlen(xml),error,sizeof error); assert(x); return x; }
static void expect(CeCslOutput *o,const char *expected) {
    CeCslOutputString s={0}; assert(ce_csl_output_plain(o,&s,error,sizeof error));
    assert(!strcmp(s.data,expected)); ce_csl_output_string_free(&s);
}
static size_t attempts,fail_at,live;
static void *allocate(void *context,size_t bytes) { (void)context; if(++attempts==fail_at) return NULL; void *p=malloc(bytes); if(p) ++live; return p; }
static void release(void *context,void *pointer) { (void)context; if(pointer) { assert(live); --live; free(pointer); } }
int main(void) {
    CeCslXml *x=parse("<text xmlns=\"http://purl.org/net/xbiblio/csl\" font-style=\"oblique\" font-weight=\"light\" font-variant=\"normal\" text-decoration=\"none\" vertical-align=\"baseline\" prefix=\"[ \" suffix=\" ]\"/>");
    unsigned resolved=999;
    assert(ce_csl_format_resolve(x->root,CE_CSL_BOLD|CE_CSL_ITALIC|CE_CSL_SMALL_CAPS|CE_CSL_UNDERLINE|CE_CSL_SUPERSCRIPT,&resolved,error,sizeof error));
    assert(resolved==(CE_CSL_OBLIQUE|CE_CSL_LIGHT));
    assert(ce_csl_format_resolve(NULL,resolved,&resolved,error,sizeof error));
    CeCslOutput *content=ce_csl_output_new(NULL,error,sizeof error),*out=ce_csl_output_new(NULL,error,sizeof error); assert(content && out);
    assert(ce_csl_format_affixes(x->root,content,0,NULL,out,error,sizeof error)); expect(out,"");
    assert(ce_csl_output_append(content,"中文",strlen("中文"),resolved,NULL,error,sizeof error));
    assert(ce_csl_format_affixes(x->root,content,CE_CSL_BOLD,NULL,out,error,sizeof error));
    expect(out,"[ 中文 ]"); assert(ce_csl_output_run_count(out)==3);
    assert(ce_csl_output_run(out,0)->format==CE_CSL_BOLD && ce_csl_output_run(out,1)->format==resolved);
    CeCslOutputString html={0}; assert(ce_csl_output_html(out,&html,error,sizeof error));
    assert(strstr(html.data,"<span style=\"font-weight:300\"><span style=\"font-style:oblique\">中文</span></span>"));
    ce_csl_output_string_free(&html);
    assert(!ce_csl_output_append(out,"bad",3,CE_CSL_BOLD|CE_CSL_LIGHT,NULL,error,sizeof error));
    assert(!ce_csl_output_append(out,"bad",3,CE_CSL_ITALIC|CE_CSL_OBLIQUE,NULL,error,sizeof error));
    expect(out,"[ 中文 ]");
    CeCslXml *bad=parse("<text font-style=\"invalid\"/>");
    assert(!ce_csl_format_resolve(bad->root,0,&resolved,error,sizeof error)); assert(resolved==(CE_CSL_OBLIQUE|CE_CSL_LIGHT));
    assert(!ce_csl_format_resolve(NULL,CE_CSL_BOLD|CE_CSL_LIGHT,&resolved,error,sizeof error));
    CeCslOutputAllocator allocator={NULL,allocate,release};
    for(size_t failure=1;failure<40;failure++) {
        attempts=fail_at=0; CeCslOutput *target=ce_csl_output_new(&allocator,error,sizeof error); assert(target);
        assert(ce_csl_output_append(target,"old",3,0,NULL,error,sizeof error));
        attempts=0; fail_at=failure;
        bool ok=ce_csl_format_affixes(x->root,content,0,&allocator,target,error,sizeof error);
        fail_at=0; expect(target,ok?"old[ 中文 ]":"old");
        ce_csl_output_free(target); assert(!live); if(ok) break;
    }
    assert(ce_csl_format_affixes(x->root,content,0,NULL,content,error,sizeof error));
    expect(content,"中文[ 中文 ]");
    ce_csl_output_free(out); ce_csl_output_free(content); ce_csl_xml_free(x); ce_csl_xml_free(bad);
    return 0;
}
