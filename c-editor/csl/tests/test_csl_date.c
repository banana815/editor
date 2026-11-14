#include "csl_date.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define NS CE_CSL_NAMESPACE
#define COUNT(a) (sizeof(a)/sizeof((a)[0]))
/* Date expectations include the official processor fixtures at
 * citation-style-language/test-suite commit
 * 6eefc5b07c6969ab8999e48542acbcc131cba864, processor-tests/humans/date_*.txt.
 * These tests exercise the date unit only, not citation/bibliography evaluation.
 * Optional argv[1]/[2] are pinned real en-US/zh-CN locale files; argv[3] is an
 * isolated official-fixture JSON bundle (style/input/expected date strings). */
static const char terms[] =
    "<term name='month-01'>January</term><term name='month-02'>February</term>"
    "<term name='month-03'>March</term><term name='month-04'>April</term>"
    "<term name='month-05'>May</term><term name='month-06'>June</term>"
    "<term name='month-07'>July</term><term name='month-08'>August</term>"
    "<term name='month-09'>September</term><term name='month-10'>October</term>"
    "<term name='month-11'>November</term><term name='month-12'>December</term>"
    "<term name='month-01' form='short'>Jan.</term><term name='month-12' form='short'>Dec.</term>"
    "<term name='season-01'>Spring</term><term name='season-02'>Summer</term>"
    "<term name='season-03'>Autumn</term><term name='season-04'>Winter</term>"
    "<term name='ordinal'>th</term><term name='ordinal-01'>st</term>"
    "<term name='ordinal-02'>nd</term><term name='ordinal-03'>rd</term>"
    "<term name='ordinal-11'>th</term><term name='ordinal-12'>th</term><term name='ordinal-13'>th</term>"
    "<term name='ad'> AD</term><term name='bc'> BC</term>";
static const char en_dates[] =
    "<date form='text'><date-part name='month' suffix=' '/><date-part name='day' suffix=', '/><date-part name='year'/></date>"
    "<date form='numeric'><date-part name='month' form='numeric-leading-zeros' suffix='/'/>"
    "<date-part name='day' form='numeric-leading-zeros' suffix='/'/><date-part name='year'/></date>";
static CeCslXml *en_xml;
static CeCslLocale *en;
static CeCslXml *xml(const char *s) {
    char error[256];
    CeCslXml *x = ce_csl_xml_parse(s,strlen(s),error,sizeof(error));
    if (!x) fprintf(stderr,"XML failed: %s\n",error);
    assert(x); return x;
}
static CeCslXml *node(const char *body) {
    size_t n = strlen(body)+100;
    char *s = malloc(n);
    CeCslXml *x;
    assert(s); snprintf(s,n,"<style xmlns='%s' version='1.0'>%s</style>",NS,body);
    x = xml(s); free(s); return x;
}
static CeCslLocale *resolver(CeCslXml *style, CeCslXml *base, const char *language) {
    const CeCslXmlNode *roots[] = {base ? base->root : en_xml->root};
    CeCslLocaleCatalog c = {roots,1,NULL,0};
    char error[256];
    CeCslLocale *l = ce_csl_locale_new(style ? style->root : NULL,&c,language,NULL,error,sizeof(error));
    if (!l) fprintf(stderr,"Locale failed: %s\n",error);
    assert(l); return l;
}
static cJSON *json(const char *s) { cJSON *j = cJSON_Parse(s); assert(j); return j; }
static void plain_is(CeCslOutput *out, const char *expected) {
    CeCslOutputString p = {0}; char error[256];
    assert(ce_csl_output_plain(out,&p,error,sizeof(error)));
    if (strcmp(p.data,expected)) fprintf(stderr,"Actual <%s>; expected <%s>\n",p.data,expected);
    assert(!strcmp(p.data,expected)); ce_csl_output_string_free(&p);
}
static CeCslOutput *render(const char *data, const char *body, const CeCslLocale *locale,
    const CeCslDateOptions *options, CeCslDateResult *result) {
    CeCslXml *x = node(body); cJSON *j = data ? json(data) : NULL;
    char error[256]; CeCslOutput *out = ce_csl_output_new(NULL,error,sizeof(error));
    assert(out);
    if (!ce_csl_date_render(j,x->root->children,locale ? locale : en,options,out,result,error,sizeof(error))) {
        fprintf(stderr,"Render failed: %s\nBody:%s\nInput:%s\n",error,body,data ? data : "NULL"); abort();
    }
    assert(!*error); cJSON_Delete(j); ce_csl_xml_free(x); return out;
}
static void expect(const char *data, const char *body, const char *expected) {
    CeCslDateResult result = {0};
    CeCslOutput *out = render(data,body,NULL,NULL,&result);
    plain_is(out,expected); assert(result.rendered == !!*expected); ce_csl_output_free(out);
}
static void setup(void) {
    char s[8192];
    snprintf(s,sizeof(s),"<locale xmlns='%s' version='1.0' xml:lang='en-US'><style-options/>%s<terms>%s</terms></locale>",NS,en_dates,terms);
    en_xml = xml(s); en = resolver(NULL,en_xml,"en-US");
}
static void test_basic(void) {
    const char *long_date = "<date variable='issued' prefix='(' suffix=')'><date-part name='day' suffix=' '/><date-part name='month' suffix=' '/><date-part name='year'/></date>";
    const char *text = "<date variable='issued' form='text'/>";
    const char *numeric = "<date variable='issued' form='numeric'/>";
    const char *year = "<date variable='issued'><date-part name='year'/></date>";
    const char *short_year = "<date variable='issued'><date-part name='year' form='short'/></date>";
    /* TextFormFulldate{Day,Month,Year}Range, MonthdateMonthRange, YeardateYearRangeOpen. */
    expect("{\"date-parts\":[[2003,8,10],[2003,8,23]]}",long_date,"(10–23 August 2003)");
    expect("{\"date-parts\":[[2003,8,3],[2003,10,23]]}",long_date,"(3 August–23 October 2003)");
    expect("{\"date-parts\":[[1987,8,3],[2003,10,23]]}",long_date,"(3 August 1987–23 October 2003)");
    expect("{\"date-parts\":[[2003,8],[2003,10]]}",long_date,"(August–October 2003)");
    expect("{\"date-parts\":[[1987],[2003]]}",long_date,"(1987–2003)");
    expect("{\"date-parts\":[[1987],[0]]}",long_date,"(1987–)");
    expect("{\"date-parts\":[[0],[1987]]}",long_date,"(–1987)");
    expect("{\"date-parts\":[[1987,3,2],[0]]}",long_date,"(2 March 1987–)");
    expect("{\"date-parts\":[[2000,1,1],[2000,1,3]]}",text,"January 1–3, 2000");
    expect("{\"date-parts\":[[2000,1,1],[2000,3,1]]}",text,"January 1–March 1, 2000");
    expect("{\"date-parts\":[[2000,1,1],[2001,1,1]]}",text,"January 1, 2000–January 1, 2001");
    expect("{\"date-parts\":[[2000,1,1],[2000,1,1]]}",text,"January 1, 2000");
    expect("{\"date-parts\":[[2000,1,1],[2000,1,3]]}",numeric,"01/01–03/2000");
    expect("{\"date-parts\":[[2005,12]]}",numeric,"12/2005");
    expect("{\"date-parts\":[[2005,12]]}",text,"December 2005");
    expect("{\"date-parts\":[[\"2000\",\"\",\"\"]]}",numeric,"2000");
    expect("{\"date-parts\":[[2000,0,0]]}",numeric,"2000");
    expect("{\"date-parts\":[]}",long_date,""); expect("{}",long_date,""); expect(NULL,long_date,"");
    expect("{\"date-parts\":[[0]]}",year,"");
    expect("{\"date-parts\":[[499]]}",year,"499 AD");
    expect("{\"date-parts\":[[999]]}",year,"999 AD"); /* Normative <four digits, not an oracle's <500 bug. */
    expect("{\"date-parts\":[[1000]]}",year,"1000");
    expect("{\"date-parts\":[[-250]]}",year,"250 BC");
    expect("{\"date-parts\":[[2005]]}",short_year,"05");
    expect("{\"date-parts\":[[10]]}",short_year,"10 AD");
    expect("{\"date-parts\":[[-2500]]}",short_year,"00 BC");
    expect("{\"date-parts\":[[999999999]]}",year,"999999999");
    expect("{\"date-parts\":[[-999999999]]}",year,"999999999 BC");
    expect("{\"literal\":\"<literal> & exact\"}",long_date,"(<literal> & exact)");
    expect("{\"date-parts\":[],\"literal\":\"maybe around 2000bc sometime?\"}",long_date,"(maybe around 2000bc sometime?)");
    expect("{\"raw\":\"2000-01-02\"}",text,"January 2, 2000");
    expect("{\"raw\":\"2000-01/2000-03\"}",text,"January–March 2000");
    expect("{\"raw\":\"-0250\"}",year,"250 BC");
    expect("{\"raw\":\"2000/..\"}",year,"2000–");
    expect("{\"raw\":\"../2000\"}",year,"–2000");
    expect("{\"date-parts\":[[2000]],\"raw\":\"unparsed archival metadata\"}",year,"2000");
    expect("{\"date-parts\":[[2000]],\"season\":3}",long_date,"(Autumn 2000)");
    expect("{\"date-parts\":[[\"1999\",\"21\"],[\"2001\",\"22\"]]}",text,"Spring 1999–Summer 2001");
    expect("{\"date-parts\":[[1999,22],[1999,21]]}",text,"Summer–Spring 1999");
    expect("{\"date-parts\":[[2000]],\"season\":\"1\"}",numeric,"Spring/2000");
    expect("{\"date-parts\":[[2000,21],[2000,22]]}",numeric,"Spring–Summer/2000");
}
static void test_parts_ranges(void) {
    const char *localized[] = {
        "<date variable='issued' form='text' date-parts='year-month'/>",
        "<date variable='issued' form='numeric' date-parts='year-month'/>",
        "<date variable='issued' form='numeric' date-parts='year'/>"
    };
    expect("{\"date-parts\":[[2005,12,15]]}",localized[0],"December 2005");
    expect("{\"date-parts\":[[2005,12,15]]}",localized[1],"12/2005");
    expect("{\"date-parts\":[[2005,12,15]]}",localized[2],"2005");
    expect("{\"date-parts\":[[2005,11,15],[2006,12,20]]}",localized[1],"11/2005–12/2006");
    expect("{\"date-parts\":[[2005,11,15],[2005,12,20]]}",localized[2],"2005");
    expect("{\"date-parts\":[[2005,12,15]]}","<date variable='issued' form='text'><date-part name='year' form='short'/><date-part name='month' form='short'/></date>","Dec. 15, 05");
    expect("{\"date-parts\":[[2005,12,15]]}","<date variable='issued' form='text'><date-part name='month' form='short' strip-periods='true'/></date>","Dec 15, 2005");
    expect("{\"date-parts\":[[2000,1,1],[2000,3,1]]}","<date variable='issued' delimiter=' '><date-part name='day'/><date-part name='year'/><date-part name='month'/></date>","1 2000 January–1 March");
    {
        const char *date = "<date variable='issued' delimiter=' '><date-part name='day' range-delimiter='_[DRD]_'/><date-part name='month' range-delimiter='_[MRD]_'/><date-part name='year' range-delimiter='_[YRD]_'/></date>";
        expect("{\"date-parts\":[[1999,1,2],[1999,1,4]]}",date,"2_[DRD]_4 January 1999");
        expect("{\"date-parts\":[[1999,1,2],[1999,3,4]]}",date,"2 January_[MRD]_4 March 1999");
        expect("{\"date-parts\":[[1999,1,2],[2000,1,4]]}",date,"2 January 1999_[YRD]_4 January 2000");
    }
    expect("{\"date-parts\":[[2000,1,1],[2000,1,3]]}","<date variable='issued'><date-part name='year' suffix='年'/><date-part name='month' form='numeric' suffix='月'/><date-part name='day' suffix='日'/></date>","2000年1月1–3日");
    expect("{\"date-parts\":[[2000,1,1],[2000,3,1]]}","<date variable='issued'><date-part name='year'/><date-part name='month' prefix='-' form='numeric-leading-zeros' range-delimiter='/'/><date-part name='day' prefix='-' form='numeric-leading-zeros'/></date>","2000-01-01/03-01");
    expect("{\"date-parts\":[[2000],[2001]]}","<date variable='issued'><date-part name='year' range-delimiter=''/></date>","20002001");
}
static void test_locale_and_case(void) {
    CeCslXml *x = node("<locale><terms><term name='month-01' gender='masculine'>janvier</term><term name='ordinal'>e</term><term name='ordinal-01' gender-form='masculine' match='whole-number'>er</term></terms><style-options limit-day-ordinals-to-day-1='true'/></locale>");
    CeCslLocale *l = resolver(x,NULL,"en-US");
    const char *date = "<date variable='issued' delimiter=' '><date-part name='day' form='ordinal'/><date-part name='month'/><date-part name='year'/></date>";
    CeCslOutput *out = render("{\"date-parts\":[[2000,1,1]]}",date,l,NULL,NULL);
    plain_is(out,"1er janvier 2000"); ce_csl_output_free(out);
    out = render("{\"date-parts\":[[2000,1,2]]}",date,l,NULL,NULL);
    plain_is(out,"2 janvier 2000"); ce_csl_output_free(out);
    ce_csl_locale_free(l); ce_csl_xml_free(x);
    expect("{\"date-parts\":[[2000,1,11]]}",date,"11th January 2000");
    expect("{\"date-parts\":[[2000,1,21]]}",date,"21st January 2000");
    expect("{\"date-parts\":[[2000,1,1]]}","<date variable='issued' form='text' text-case='uppercase'/>","JANUARY 1, 2000");
    expect("{\"date-parts\":[[2000,12]]}","<date variable='issued' form='text'><date-part name='month' form='short' text-case='uppercase'/></date>","DEC. 2000");
    {
        CeCslDateResult result = {0};
        const char *values[] = {"true","1","\"1\"","\"true\""};
        size_t i; char input[128];
        for (i = 0; i < COUNT(values); ++i) {
            snprintf(input,sizeof(input),"{\"date-parts\":[[2000]],\"circa\":%s}",values[i]);
            out = render(input,"<date variable='issued'><date-part name='year'/></date>",NULL,NULL,&result);
            plain_is(out,"2000"); assert(result.circa); ce_csl_output_free(out);
        }
    }
    x = node("<locale><terms><term name='month-01'/><term name='ordinal'/><term name='ad'/></terms></locale>");
    l = resolver(x,NULL,"en-US");
    {
        CeCslDateResult result = {0};
        out = render("{\"date-parts\":[[50,1,1]]}",date,l,NULL,&result);
        plain_is(out,"1 50"); assert(!result.missing_term); ce_csl_output_free(out);
    }
    ce_csl_locale_free(l); ce_csl_xml_free(x);
    l = ce_csl_locale_new(NULL,NULL,"en-US",NULL,NULL,0); assert(l);
    {
        CeCslDateResult result = {0};
        out = render("{\"date-parts\":[[50,1,1]]}",date,l,NULL,&result);
        plain_is(out,"1 50"); assert(result.missing_term); ce_csl_output_free(out);
    }
    ce_csl_locale_free(l);
}
static void formats_at(CeCslOutput *out, const char *needle, unsigned format) {
    size_t i; bool found = false;
    for (i = 0; i < ce_csl_output_run_count(out); ++i) {
        const CeCslOutputRun *run = ce_csl_output_run(out,i);
        if (strstr(run->text,needle)) { assert(run->format == format); found = true; }
    }
    assert(found);
}
static void test_format(void) {
    CeCslDateOptions options = {CE_CSL_BOLD,NULL,NULL,NULL};
    CeCslOutput *out = render("{\"date-parts\":[[2000,1,2]]}",
        "<date variable='issued' font-style='italic' prefix='(' suffix=')'><date-part name='month' font-style='normal' suffix=' '/><date-part name='year'/></date>",NULL,&options,NULL);
    plain_is(out,"(January 2000)");
    formats_at(out,"(",CE_CSL_BOLD); formats_at(out,"January",CE_CSL_BOLD);
    formats_at(out,"2000",CE_CSL_BOLD|CE_CSL_ITALIC); formats_at(out," ",CE_CSL_BOLD|CE_CSL_ITALIC);
    ce_csl_output_free(out);
    out = render("{\"date-parts\":[[2000,1,2]]}",
        "<date variable='issued' form='text' text-case='uppercase' prefix='before '><date-part name='month' font-style='italic'/></date>",NULL,NULL,NULL);
    plain_is(out,"before JANUARY 2, 2000"); formats_at(out,"JANUARY",CE_CSL_ITALIC); formats_at(out,"2000",0); formats_at(out,"before",0);
    ce_csl_output_free(out);
    out = render("{\"literal\":\"straßE\"}","<date variable='issued' text-case='uppercase' font-style='oblique' font-weight='light'><date-part name='year'/></date>",NULL,NULL,NULL);
    plain_is(out,"STRASSE"); formats_at(out,"STRASSE",CE_CSL_OBLIQUE|CE_CSL_LIGHT); ce_csl_output_free(out);
    /* A whole-string case context spans part text and differently styled affix:
     * final sigma must see the following Greek letter despite the font split. */
    {
        CeCslXml *x = node("<locale><terms><term name='month-01'>ΟΣ</term></terms></locale>");
        CeCslLocale *l = resolver(x,NULL,"en-US");
        out = render("{\"date-parts\":[[2000,1]]}","<date variable='issued' text-case='lowercase'><date-part name='month' font-style='italic' suffix='Α'/></date>",l,NULL,NULL);
        plain_is(out,"οσα"); formats_at(out,"οσ",CE_CSL_ITALIC); formats_at(out,"α",0);
        ce_csl_output_free(out); ce_csl_locale_free(l); ce_csl_xml_free(x);
    }
}
static void rejected(const char *data, const char *style) {
    CeCslXml *x = node(style); cJSON *j = json(data);
    char error[256]; CeCslDateResult result = {true,true,true};
    CeCslOutput *out = ce_csl_output_new(NULL,error,sizeof(error));
    assert(out && ce_csl_output_append(out,"unchanged",9,CE_CSL_BOLD,NULL,error,sizeof(error)));
    assert(!ce_csl_date_render(j,x->root->children,en,NULL,out,&result,error,sizeof(error)));
    assert(*error && result.rendered && result.missing_term && result.circa);
    plain_is(out,"unchanged"); assert(ce_csl_output_run_count(out) == 1);
    ce_csl_output_free(out); cJSON_Delete(j); ce_csl_xml_free(x);
}
static void test_invalid(void) {
    const char *style = "<date variable='issued' form='text'/>";
    const char *bad[] = {
        "null", "[]", "42", "{\"x\":1}", "{\"circa\":true,\"circa\":false}",
        "{\"date-parts\":1}", "{\"date-parts\":[[]]}", "{\"date-parts\":[[1,2,3,4]]}",
        "{\"date-parts\":[[1],[2],[3]]}", "{\"date-parts\":[[1.5]]}",
        "{\"date-parts\":[[1000000000]]}", "{\"date-parts\":[[\"1000000000\"]]}",
        "{\"date-parts\":[[\"1e3\"]]}", "{\"date-parts\":[[2000,-1]]}",
        "{\"date-parts\":[[2000,13]]}", "{\"date-parts\":[[2000,17]]}",
        "{\"date-parts\":[[2000,25]]}", "{\"date-parts\":[[2000,1,32]]}",
        "{\"date-parts\":[[2000,0,1]]}", "{\"date-parts\":[[0,1]]}",
        "{\"date-parts\":[[2000,21,1]]}", "{\"date-parts\":[[2000,1],[2000]]}",
        "{\"date-parts\":[[2000]],\"literal\":\"other\"}", "{\"literal\":1}",
        "{\"raw\":1}", "{\"raw\":\"Bogus Date\"}", "{\"raw\":\"2000?\"}",
        "{\"raw\":\"2000-01-02T12:00Z\"}", "{\"raw\":\"2000-1-2\"}",
        "{\"raw\":\"0000\"}", "{\"raw\":\"2000-00\"}", "{\"raw\":\"2000-01-00\"}",
        "{\"raw\":\"../..\"}", "{\"raw\":\"2000/2001/2002\"}",
        "{\"circa\":2}", "{\"circa\":\"yes\"}", "{\"circa\":\"01\"}", "{\"season\":1}",
        "{\"date-parts\":[[2000]],\"season\":5}", "{\"date-parts\":[[2000,1]],\"season\":1}"
    };
    const char *styles[] = {
        "<date variable='issued'/>", "<date variable='issued' form='other'/>",
        "<date variable='issued' form='text' display='block'/>",
        "<date variable='issued' form='text' delimiter=' '/>",
        "<date variable='issued' date-parts='year'><date-part name='year'/></date>",
        "<date variable='issued' form='text' date-parts='month'/>",
        "<date variable='issued' form='text'><date-part name='month' prefix='x'/></date>",
        "<date variable='issued'><date-part name='year'/><date-part name='year'/></date>",
        "<date variable='issued'><date-part name='month' form='ordinal'/></date>",
        "<date variable='issued'><date-part name='day' form='short'/></date>",
        "<date variable='issued'><date-part name='year' form='numeric'/></date>",
        "<date variable='issued'><date-part name='hour'/></date>",
        "<date variable='issued'><date-part name='year' strip-periods='true'/></date>",
        "<date variable='issued'><date-part name='month' strip-periods='yes'/></date>",
        "<date variable='issued'><date-part name='year' text-case='other'/></date>",
        "<date variable='issued' font-style='other'><date-part name='year'/></date>",
        "<date variable='issued'><date-part name='year'>text</date-part></date>"
    };
    size_t i;
    for (i = 0; i < COUNT(bad); ++i) rejected(bad[i],style);
    for (i = 0; i < COUNT(styles); ++i) rejected("{\"date-parts\":[[2000,1,1]]}",styles[i]);
}
typedef struct Heap { size_t calls, fail_at, live; } Heap;
static void *allocate(void *context, size_t bytes) {
    Heap *h = context; void *p;
    if (++h->calls == h->fail_at) return NULL;
    p = malloc(bytes); if (p) ++h->live; return p;
}
static void release(void *context, void *p) {
    Heap *h = context;
    if (p) { assert(h->live); --h->live; free(p); }
}
static void test_atomic(void) {
    CeCslXml *x = node("<date variable='issued' form='text' text-case='uppercase' prefix='(' suffix=')'><date-part name='month' font-style='italic'/><date-part name='day' form='ordinal'/></date>");
    cJSON *j = json("{\"date-parts\":[[2000,1,1],[2000,3,2]],\"circa\":true}");
    size_t fail_at, total = 0;
    char error[256];
    for (fail_at = 0; !fail_at || fail_at <= total; ++fail_at) {
        Heap h = {0}; CeCslOutputAllocator a = {&h,allocate,release};
        CeCslDateOptions opts = {0,NULL,NULL,&a}; CeCslDateResult result = {true,true,false};
        CeCslOutput *out = ce_csl_output_new(&a,error,sizeof(error));
        assert(out && ce_csl_output_append(out,"seed",4,0,NULL,error,sizeof(error)));
        h.calls = 0; h.fail_at = fail_at;
        if (!fail_at) {
            assert(ce_csl_date_render(j,x->root->children,en,&opts,out,&result,error,sizeof(error)));
            total = h.calls; assert(total > 20); assert(result.circa && !result.missing_term);
            plain_is(out,"seed(JANUARY 1ST–MARCH 2ND, 2000)");
        } else {
            assert(!ce_csl_date_render(j,x->root->children,en,&opts,out,&result,error,sizeof(error)));
            assert(*error && result.rendered && result.missing_term && !result.circa);
            h.fail_at = 0; plain_is(out,"seed");
        }
        ce_csl_output_free(out); assert(!h.live);
    }
    ce_csl_xml_free(x); cJSON_Delete(j);
    {
        CeCslOutput *out = ce_csl_output_new(NULL,error,sizeof(error));
        char *large = malloc(CE_CSL_OUTPUT_MAX_BYTES+1);
        assert(large && out); memset(large,'x',CE_CSL_OUTPUT_MAX_BYTES); large[CE_CSL_OUTPUT_MAX_BYTES] = 0;
        assert(ce_csl_output_append(out,large,CE_CSL_OUTPUT_MAX_BYTES,0,NULL,error,sizeof(error)));
        x = node("<date variable='issued'><date-part name='year'/></date>"); j = json("{\"date-parts\":[[2000]]}");
        assert(!ce_csl_date_render(j,x->root->children,en,NULL,out,NULL,error,sizeof(error)));
        assert(ce_csl_output_text_bytes(out) == CE_CSL_OUTPUT_MAX_BYTES && ce_csl_output_run_count(out) == 1);
        cJSON_Delete(j); ce_csl_xml_free(x); ce_csl_output_free(out); free(large);
    }
}
static char *read_file(const char *path) {
    FILE *f = fopen(path,"rb"); long length; char *text;
    assert(f && !fseek(f,0,SEEK_END)); length = ftell(f); assert(length >= 0 && length < 4*1024*1024);
    rewind(f); text = malloc((size_t)length+1); assert(text);
    assert(fread(text,1,(size_t)length,f) == (size_t)length && !fclose(f)); text[length] = 0; return text;
}
static void real_locales(const char *english, const char *chinese) {
    char *a = read_file(english), *b = read_file(chinese);
    CeCslXml *e = xml(a), *z = xml(b); CeCslLocale *el = resolver(NULL,e,"en-US"), *zl = resolver(NULL,z,"zh-CN");
    CeCslOutput *out = render("{\"date-parts\":[[2005,12,15]]}","<date variable='issued' form='text'/>",el,NULL,NULL);
    plain_is(out,"December 15, 2005"); ce_csl_output_free(out);
    out = render("{\"date-parts\":[[1998,4,10]]}","<date variable='issued' form='text'/>",zl,NULL,NULL);
    plain_is(out,"1998年4月10日"); ce_csl_output_free(out);
    out = render("{\"date-parts\":[[1998,4,10],[1998,4,12]]}","<date variable='issued' form='numeric'/>",zl,NULL,NULL);
    plain_is(out,"1998-04-10/12"); ce_csl_output_free(out);
    ce_csl_locale_free(el); ce_csl_locale_free(zl); ce_csl_xml_free(e); ce_csl_xml_free(z); free(a); free(b);
}
static void collect_dates(const CeCslXmlNode *n, const CeCslXmlNode **dates, size_t *count) {
    for (; n; n = n->next) {
        if (ce_csl_xml_is(n,"locale")) continue;
        if (ce_csl_xml_is(n,"date")) { assert(*count < 16); dates[(*count)++] = n; }
        collect_dates(n->children,dates,count);
    }
}
static void official(const char *path) {
    char *text = read_file(path); cJSON *tests = json(text), *test;
    size_t count = 0;
    for (test = tests->child; test; test = test->next) {
        const cJSON *styles = cJSON_GetObjectItemCaseSensitive(test,"style"), *inputs = cJSON_GetObjectItemCaseSensitive(test,"input");
        const cJSON *expected = cJSON_GetObjectItemCaseSensitive(test,"expected"), *name = cJSON_GetObjectItemCaseSensitive(test,"name");
        const CeCslXmlNode *dates[16]; size_t date_count = 0, i;
        CeCslXml *x = xml(styles->valuestring); CeCslLocale *l = resolver(x,NULL,"en-US");
        const cJSON *item, *expectation = expected->child;
        collect_dates(x->root,dates,&date_count); assert(date_count);
        for (item = inputs->child; item; item = item->next) for (i = 0; i < date_count; ++i) {
            char error[256]; CeCslOutput *out = ce_csl_output_new(NULL,error,sizeof(error));
            const char *variable = ce_csl_xml_attribute(dates[i],"variable");
            assert(out && expectation && cJSON_IsString(expectation));
            if (!ce_csl_date_render(cJSON_GetObjectItemCaseSensitive(item,variable),dates[i],l,NULL,out,NULL,error,sizeof(error))) {
                fprintf(stderr,"Official %s failed: %s\n",name->valuestring,error); abort();
            }
            plain_is(out,expectation->valuestring); expectation = expectation->next; ce_csl_output_free(out); ++count;
        }
        assert(!expectation); ce_csl_locale_free(l); ce_csl_xml_free(x);
    }
    cJSON_Delete(tests); free(text); printf("Official date cases passed: %zu\n",count);
}
int main(int argc, char **argv) {
    setup(); test_basic(); test_parts_ranges(); test_locale_and_case(); test_format(); test_invalid(); test_atomic();
    if (argc >= 3) real_locales(argv[1],argv[2]);
    if (argc >= 4) official(argv[3]);
    ce_csl_locale_free(en); ce_csl_xml_free(en_xml);
    puts("CSL date tests passed"); return 0;
}
