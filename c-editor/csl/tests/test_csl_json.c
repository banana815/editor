#include "csl_json.h"
#include <assert.h>
#include <float.h>
#include <locale.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char error[256];
static size_t cases;
static void accepted(cJSON **out,const char *text,size_t size) {
    if(!ce_csl_json_parse_strict(text,size,out,error,sizeof(error))) { fprintf(stderr,"strict JSON: %s\n",error); assert(0); }
    assert(*out); ++cases;
}
static void rejected(cJSON **out,const char *text,size_t size) {
    cJSON *old=*out;
    char *before=cJSON_PrintUnformatted(old); assert(before);
    assert(!ce_csl_json_parse_strict(text,size,out,error,sizeof(error)) && error[0] && *out==old);
    char *after=cJSON_PrintUnformatted(*out); assert(after && !strcmp(before,after));
    cJSON_free(before); cJSON_free(after); ++cases;
}
static void test_values(void) {
    cJSON *out=NULL;
    const char *good[]={"null","true","false","0","-0","12.5e+2","1e-999","[]","{}","\"\"",
        " \t{\"空\":\"中文🙂\",\"escaped\":\"\\b\\f\\n\\r\\t\\/\\\\\\\"\",\"pair\":\"\\ud83d\\ude42\"}\r\n",
        "{\"a\":1,\"A\":2}","{\"é\":1,\"é\":2}","[{},null,{\"nested\":[true,0]}]"};
    for(size_t i=0;i<sizeof(good)/sizeof(*good);++i) accepted(&out,good[i],strlen(good[i]));
    accepted(&out,"12.5e+2",7); assert(cJSON_IsNumber(out) && out->valuedouble==1250);
    const char *bad[]={""," ","{","[1,]","{\"a\":1,}","[1 2]","true false","//comment\n0","/*x*/0",
        "NaN","Infinity","-Infinity","+1","01","-01",".1","1.","1e","1e+","0x10","1e309","-1e9999",
        "{\"x\":1,\"x\":2}","{\"a\":1,\"\\u0061\":2}","[{\"x\":1,\"x\":2}]",
        "{\"\":1,\"\":2}","\"\\u0000\"","\"\\ud800\"","\"\\udc00\"","\"\\ud800\\u0061\"",
        "\"\\x41\"","\"line\nbreak\"","\"unterminated","\"bad\\","\xef\xbb\xbf{}"};
    for(size_t i=0;i<sizeof(bad)/sizeof(*bad);++i) rejected(&out,bad[i],strlen(bad[i]));
    const unsigned char invalid[][7]={{'"',0xc0,0x80,'"',0},{'"',0xed,0xa0,0x80,'"',0},
        {'"',0xf4,0x90,0x80,0x80,'"',0},{'"',0x80,'"',0},{'"',0xc2,'"',0},{'"',0xf0,0x9f,0,'"',0}};
    const size_t lengths[]={4,5,6,3,3,5};
    for(size_t i=0;i<sizeof(lengths)/sizeof(*lengths);++i) rejected(&out,(const char *)invalid[i],lengths[i]);
    const char nul[]={'{','}',0,'x'}; rejected(&out,nul,sizeof(nul));
    /* The counted buffer is deliberately not NUL-terminated, with forbidden
     * bytes outside its declared region. No strlen or implicit lookahead. */
    const char bounded[]={'[','1',']','x',0}; accepted(&out,bounded,3); assert(cJSON_GetArraySize(out)==1);
    char *exact=malloc(3); assert(exact); memcpy(exact,"123",3); accepted(&out,exact,3); free(exact);
    assert(cJSON_IsNumber(out) && out->valuedouble==123);
    cJSON *old=out; assert(!ce_csl_json_parse_strict(NULL,0,&out,NULL,0) && out==old);
    assert(!ce_csl_json_parse_strict("{}",2,NULL,error,sizeof(error)) && error[0]);
    cJSON_Delete(out);
}
static void test_limits(void) {
    cJSON *out=NULL; accepted(&out,"null",4);
    char nesting[259];
    for(size_t depth=128;depth<=129;++depth) {
        memset(nesting,'[',depth); nesting[depth]='0'; memset(nesting+depth+1,']',depth);
        if(depth==128) accepted(&out,nesting,2*depth+1); else rejected(&out,nesting,2*depth+1);
    }
    size_t max=CE_CSL_INPUT_MAX_BYTES;
    char *large=malloc(max+1); assert(large); memset(large,'x',max+1); large[0]='"'; large[max-1]='"';
    accepted(&out,large,max); assert(cJSON_IsString(out) && strlen(out->valuestring)==max-2);
    /* Do not serialize a 16 MiB old tree for each subsequent failure. */
    accepted(&out,"null",4); rejected(&out,large,max+1); free(large);
    size_t elements=CE_CSL_INPUT_MAX_NODES-1,bytes=elements*2+1;
    char *nodes=malloc(bytes+2); assert(nodes); nodes[0]='[';
    for(size_t i=0;i<elements;++i) { nodes[1+2*i]='0'; nodes[2+2*i]=','; }
    nodes[bytes-1]=']'; accepted(&out,nodes,bytes); assert((size_t)cJSON_GetArraySize(out)==elements);
    accepted(&out,"null",4); nodes[bytes-1]=','; nodes[bytes]='0'; nodes[bytes+1]=']'; rejected(&out,nodes,bytes+2);
    free(nodes); cJSON_Delete(out);
}
static void test_number_fidelity(void) {
    const char *exact[]={"{}","[null,true,\"1.5\"]","0","-0.0","0e-9999","1.00","12.5e2",
        "9007199254740991","-9007199254740991","900719925474099100e-2"};
    const char *inexact[]={"1.5","1.0000000000000001","0.9999999999999999999","1e-9999",
        "-1e-9999","9007199254740992","9007199254740993","-9007199254740992",
        "9007199254740991.1","[1,{\"unknown\":1.0000000000000001}]"};
    cJSON *tree=NULL;
    for(size_t group=0;group<2;group++) {
        const char *const *values=group?inexact:exact;
        size_t count=group?sizeof inexact/sizeof *inexact:sizeof exact/sizeof *exact;
        for(size_t i=0;i<count;i++) {
            bool result=group==0?false:true;
            assert(ce_csl_json_parse_strict_with_numbers(values[i],strlen(values[i]),&tree,&result,error,sizeof error));
            assert(result==(group==0)); ++cases;
        }
    }
    for(size_t i=0;i<2;i++) {
        bool result=i!=0,prior=result; cJSON *old=tree;
        assert(!ce_csl_json_parse_strict_with_numbers("[1,]",4,&tree,&result,error,sizeof error));
        assert(result==prior && tree==old); ++cases;
    }
    cJSON_Delete(tree);
}
static void *deny_allocation(size_t bytes) { (void)bytes; return NULL; }
static void test_exact_print(void) {
    const double numbers[]={4503599627370496.0,4503599627370497.0,4503599627370501.0,
        9007199254740991.0,-9007199254740991.0,-4503599627370501.0,0.0,-0.0,
        0.1,1.2345678901234567,DBL_MIN,DBL_MAX,0x1p-1074,9007199254740992.0};
    for(size_t i=0;i<sizeof numbers/sizeof *numbers;i++) {
        cJSON *node=cJSON_CreateNumber(numbers[i]); assert(node);
        char *printed=ce_csl_json_print_exact(node); assert(printed);
        cJSON *copy=cJSON_Parse(printed); assert(cJSON_IsNumber(copy));
        assert(copy->valuedouble==numbers[i]);
        if(numbers[i]==0) assert(!!signbit(copy->valuedouble)==!!signbit(numbers[i]));
        if(i==2) assert(!strcmp(printed,"4503599627370501"));
        cJSON_Delete(copy); cJSON_free(printed); cJSON_Delete(node); ++cases;
    }
    const char *json="{\"中文🙂\":[null,true,false,\"\\\"\\\\\\b\\f\\n\\r\\t\\u0001\"],\"a\":4503599627370501}";
    cJSON *tree=cJSON_Parse(json); assert(tree);
    char *printed=ce_csl_json_print_exact(tree); assert(printed && !strcmp(printed,json));
    size_t size=strlen(printed);
    char *exact=ce_csl_json_print_exact_with_limit(tree,size); assert(exact && !strcmp(exact,printed));
    assert(!ce_csl_json_print_exact_with_limit(tree,size-1));
    assert(!ce_csl_json_print_exact_with_limit(tree,0));
    assert(!ce_csl_json_print_exact_with_limit(tree,SIZE_MAX));
    cJSON_Hooks hooks={deny_allocation,free}; cJSON_InitHooks(&hooks);
    assert(!ce_csl_json_print_exact(tree)); cJSON_InitHooks(NULL);
    char *after=ce_csl_json_print_exact(tree); assert(after && !strcmp(after,printed));
    cJSON_free(after); cJSON_free(exact); cJSON_free(printed); cJSON_Delete(tree); cases+=7;
    cJSON *bad=cJSON_CreateNumber(INFINITY); assert(bad && !ce_csl_json_print_exact(bad));
    bad->valuedouble=NAN; assert(!ce_csl_json_print_exact(bad)); cJSON_Delete(bad);
    bad=cJSON_CreateRaw("{\"unsafe\":1}"); assert(bad && !ce_csl_json_print_exact(bad)); cJSON_Delete(bad);
    bad=cJSON_CreateString("\xc0\x80"); assert(bad && !ce_csl_json_print_exact(bad)); cJSON_Delete(bad); cases+=4;
    /* snprintf's locale must not leak a decimal comma into exported JSON. */
    const char *current=setlocale(LC_NUMERIC,NULL); char saved[128];
    assert(current && strlen(current)<sizeof saved); strcpy(saved,current);
    const char *locales[]={"de_DE.UTF-8","de_DE.utf8","fr_FR.UTF-8","German_Germany.1252"};
    for(size_t i=0;i<sizeof locales/sizeof *locales;i++) if(setlocale(LC_NUMERIC,locales[i])) {
        cJSON *number=cJSON_CreateNumber(1.25); char *text=ce_csl_json_print_exact(number);
        assert(text && !strcmp(text,"1.25")); cJSON_free(text); cJSON_Delete(number); ++cases; break;
    }
    assert(setlocale(LC_NUMERIC,saved));
}
int main(void) {
    test_values(); test_limits(); test_number_fidelity(); test_exact_print();
    printf("Shared strict JSON passed (%zu cases)\n",cases); return 0;
}
