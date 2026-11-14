#include "csl_page_field.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static char error[512];
static CeCslPageFieldOptions options = {CE_CSL_PAGE_FIELD_PAGE, true, CE_CSL_PAGE_MINIMAL};
static void render(const char *source, const char *expected, bool numeric, size_t ranges) {
    CeCslOutputString out = {0}; CeCslPageFieldInfo info = {0};
    bool ok = ce_csl_page_field_render(source, strlen(source), &options, NULL, NULL, &out, &info, error, sizeof(error));
    if (!ok || strcmp(out.data, expected)) fprintf(stderr, "%s => %s, expected %s\n", source, ok ? out.data : error, expected);
    assert(ok && !strcmp(out.data, expected));
    assert(info.numeric == numeric && info.range_count == ranges && info.missing_term == (ranges != 0));
    ce_csl_output_string_free(&out);
}
static CeCslXml *xml;
static CeCslLocale *locale(const char *term) {
    char raw[2048]; snprintf(raw, sizeof(raw),
        "<style xmlns='http://purl.org/net/xbiblio/csl' version='1.0'><locale><terms><term name='page-range-delimiter'>%s</term></terms></locale></style>",term);
    xml=ce_csl_xml_parse(raw,strlen(raw),error,sizeof(error)); assert(xml);
    CeCslLocale *result=ce_csl_locale_new(xml->root,NULL,"en-US",NULL,error,sizeof(error)); assert(result); return result;
}
static void reject(const char *text, size_t size, const char *message) {
    CeCslOutputString old={0}; CeCslPageFieldInfo info={true,true,99};
    CeCslPageFieldOptions literal={CE_CSL_PAGE_FIELD_PAGE,false,CE_CSL_PAGE_EXPANDED};
    assert(ce_csl_page_field_render("old",3,&literal,NULL,NULL,&old,NULL,error,sizeof(error)));
    char *before=old.data;
    assert(!ce_csl_page_field_render(text,size,&options,NULL,NULL,&old,&info,error,sizeof(error)));
    if (!strstr(error,message)) fprintf(stderr,"wrong error: %s (wanted %s)\n",error,message);
    assert(strstr(error,message) && old.data==before && !strcmp(old.data,"old"));
    assert(info.numeric && info.missing_term && info.range_count==99); ce_csl_output_string_free(&old);
}
static size_t attempts,fail_at,live;
static void *allocate(void *context,size_t bytes) { (void)context; if(++attempts==fail_at)return NULL; void *p=malloc(bytes);if(p)++live;return p; }
static void release(void *context,void *p) { (void)context;if(p){assert(live);--live;free(p);} }
static void oom(const char *source, const char *expected) {
    CeCslOutputAllocator allocator={NULL,allocate,release}; bool completed=false;
    for(size_t i=1;i<200;++i) {
        CeCslOutputString out={0}; CeCslPageFieldInfo info={true,false,99}; attempts=fail_at=0;
        assert(ce_csl_page_field_render("old",3,&options,NULL,&allocator,&out,NULL,error,sizeof(error)));
        char *before=out.data; attempts=0;fail_at=i;
        bool ok=ce_csl_page_field_render(source,strlen(source),&options,NULL,&allocator,&out,&info,error,sizeof(error));
        fail_at=0;
        if(!ok) assert(out.data==before && !strcmp(out.data,"old") && info.numeric && !info.missing_term && info.range_count==99);
        else assert(!strcmp(out.data,expected));
        ce_csl_output_string_free(&out);assert(!live);
        if(ok){printf("page-field OOM: %zu failure points\n",i-1);completed=true;break;}
    }
    assert(completed);
}
int main(void) {
    render("101-109","101–9",true,1);
    render(" 101 - 109,201–209 & 301-309\t"," 101 – 9,201–9 & 301–9\t",true,3);
    render("S101-S109","S101–S9",true,1);
    render("101a-109a","101a–9a",true,1);
    render("甲101章-甲109章","甲101章–甲9章",true,1);
    render("A\xcc\x81" "101-A\xcc\x81" "109","A\xcc\x81" "101–A\xcc\x81" "9",true,1);
    render("S101-T109","S101–T109",true,1);
    render("S101-109","S101–109",true,1);
    render("001-009","001–009",true,1);
    render("184467440737095516160001-184467440737095516160009","184467440737095516160001–9",true,1);
    render("A-B","A-B",false,0); render("pre-publication 101-109","pre-publication 101-109",false,0);
    render("1/2-3/4","1/2-3/4",false,0); render("101, 201 &301","101, 201 &301",true,0);
    render("<unknown-tag>101-109</unknown-tag>","<unknown-tag>101-109</unknown-tag>",false,0);
    reject("1-2-3",5,"chained"); reject("9-2",3,"Descending");
    reject("<i>101-109</i>",strlen("<i>101-109</i>"),"markup");
    reject("<span class='nocase'>101-109</span>",strlen("<span class='nocase'>101-109</span>"),"markup");
    reject("<b>1-2",6,"markup"); reject("1\0-2",4,"Invalid"); reject("\xff",1,"Invalid");
    options.format=CE_CSL_PAGE_EXPANDED;render("199-02","199–202",true,1);render("999-02","999–1002",true,1);
    options.format=CE_CSL_PAGE_MINIMAL_TWO;render("321-328","321–28",true,1);
    options.format=CE_CSL_PAGE_CHICAGO;render("1496-1504","1496–1504",true,1);
    options.format=CE_CSL_PAGE_CHICAGO_15;render("1496-1504","1496–1504",true,1);
    options.format=CE_CSL_PAGE_CHICAGO_16;render("1496-1504","1496–504",true,1);
    options.has_format=false;render(" 101 - 109,201-209 &301-309 "," 101 - 109,201-209 &301-309 ",true,0);
    render("1-2-3","1-2-3",true,0);
    options.policy=CE_CSL_PAGE_FIELD_PAGE_LOCATOR;render("101-109","101–109",true,0);
    render("pre-publication","pre–publication",false,0);
    options.has_format=true;options.format=CE_CSL_PAGE_MINIMAL;render("101-109","101–9",true,1);
    options.policy=CE_CSL_PAGE_FIELD_OTHER_LOCATOR;render("101-109","101–109",true,0);
    options.policy=CE_CSL_PAGE_FIELD_PAGE;
    for(size_t i=0;i<2;++i){
        CeCslLocale *loc=locale(i?"":"至");CeCslOutputString out={0};CeCslPageFieldInfo info={0};
        assert(ce_csl_page_field_render("101-109",7,&options,loc,NULL,&out,&info,error,sizeof(error)));
        assert(!strcmp(out.data,i?"1019":"101至9") && !info.missing_term && info.range_count==1);
        ce_csl_output_string_free(&out);ce_csl_locale_free(loc);ce_csl_xml_free(xml);
    }
    char *large=malloc(CE_CSL_OUTPUT_MAX_BYTES+2);assert(large);memset(large,'x',CE_CSL_OUTPUT_MAX_BYTES+1);large[CE_CSL_OUTPUT_MAX_BYTES+1]=0;
    CeCslOutputString out={0};CeCslPageFieldInfo info={0};
    assert(ce_csl_page_field_render(large,CE_CSL_OUTPUT_MAX_BYTES,&options,NULL,NULL,&out,&info,error,sizeof(error)));
    assert(out.length==CE_CSL_OUTPUT_MAX_BYTES);ce_csl_output_string_free(&out);
    reject(large,CE_CSL_OUTPUT_MAX_BYTES+1,"oversized");
    options.policy=CE_CSL_PAGE_FIELD_OTHER_LOCATOR;
    memset(large,'-',CE_CSL_OUTPUT_MAX_BYTES);reject(large,CE_CSL_OUTPUT_MAX_BYTES,"limit");
    options.policy=CE_CSL_PAGE_FIELD_PAGE;
    memset(large,'1',CE_CSL_NUMBER_MAX_DECIMAL_DIGITS);
    large[CE_CSL_NUMBER_MAX_DECIMAL_DIGITS]='-';
    memset(large+CE_CSL_NUMBER_MAX_DECIMAL_DIGITS+1,'1',CE_CSL_NUMBER_MAX_DECIMAL_DIGITS);
    large[2*CE_CSL_NUMBER_MAX_DECIMAL_DIGITS]='9';
    assert(ce_csl_page_field_render(large,2*CE_CSL_NUMBER_MAX_DECIMAL_DIGITS+1,&options,NULL,NULL,&out,&info,error,sizeof(error)));
    assert(out.length==CE_CSL_NUMBER_MAX_DECIMAL_DIGITS+4 && !memcmp(out.data,large,CE_CSL_NUMBER_MAX_DECIMAL_DIGITS));
    assert(!memcmp(out.data+CE_CSL_NUMBER_MAX_DECIMAL_DIGITS,"–9",4));
    ce_csl_output_string_free(&out);
    memset(large,'9',CE_CSL_NUMBER_MAX_DECIMAL_DIGITS+1);large[CE_CSL_NUMBER_MAX_DECIMAL_DIGITS+1]='-';large[CE_CSL_NUMBER_MAX_DECIMAL_DIGITS+2]='1';
    reject(large,CE_CSL_NUMBER_MAX_DECIMAL_DIGITS+3,"limit");
    large[0]='A';memset(large+1,'9',CE_CSL_NUMBER_MAX_DECIMAL_DIGITS+1);
    memcpy(large+CE_CSL_NUMBER_MAX_DECIMAL_DIGITS+2,"-B1",3);
    reject(large,CE_CSL_NUMBER_MAX_DECIMAL_DIGITS+5,"limit");free(large);
    options.has_format=false;
    assert(ce_csl_page_field_render("101-109",7,&options,NULL,NULL,&out,NULL,error,sizeof(error)));
    options.has_format=true;
    assert(ce_csl_page_field_render(out.data,out.length,&options,NULL,NULL,&out,&info,error,sizeof(error)));
    assert(!strcmp(out.data,"101–9") && info.range_count==1); ce_csl_output_string_free(&out);
    oom("S101-S109, 199-02 & 301-309","S101–S9, 199–202 & 301–9");
    oom("<unknown-tag>101-109</unknown-tag>","<unknown-tag>101-109</unknown-tag>");
    puts("CSL mixed page fields: exact decimals, Unicode affixes, whitespace, locale, locator policy, limits and atomic OOM passed");return 0;
}
