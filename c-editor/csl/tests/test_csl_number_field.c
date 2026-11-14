#include "csl_number_field.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static char error[256];
static void scan(const char *text,bool numeric,size_t count,bool greater) {
    CeCslNumberFieldInfo info={0};
    assert(ce_csl_number_field_scan(text,strlen(text),&info,error,sizeof error));
    if(info.numeric!=numeric) fprintf(stderr,"unexpected classification: %s\n",text);
    assert(info.numeric==numeric && info.number_count==count && info.single_greater_than_one==greater);
}
static void render(const char *text,CeCslNumberForm form,const char *expected) {
    CeCslOutputString output={0}; bool missing=false;
    assert(ce_csl_number_field_render(text,strlen(text),form,NULL,CE_CSL_LOCALE_NEUTER,NULL,&output,&missing,error,sizeof error));
    if(strcmp(output.data,expected)) fprintf(stderr,"%s => %s, expected %s\n",text,output.data,expected);
    assert(!strcmp(output.data,expected)); ce_csl_output_string_free(&output);
}
static size_t attempts,fail_at,live;
static void *allocate(void *context,size_t bytes) {
    (void)context; if(++attempts==fail_at) return NULL;
    void *memory=malloc(bytes); if(memory) ++live; return memory;
}
static void release(void *context,void *memory) { (void)context; if(memory) { assert(live); --live; free(memory); } }
int main(void) {
    scan("",false,0,false); scan("\t  \n",false,0,false);
    scan("0",true,1,false); scan("0001",true,1,false); scan("0002",true,1,true);
    scan("90071992547409910000000000001",true,1,true);
    scan("D2",true,1,true); scan("2b",true,1,true); scan("L2d",true,1,true);
    scan("2nd",true,1,true); scan("second",false,0,false); scan("2nd edition",false,0,false);
    scan("2, 3",true,2,false); scan("2-4",true,2,false); scan("2 & 4",true,2,false);
    scan("2 – 4",true,2,false); scan("2d5",false,0,false); scan("2/4",false,0,false);
    scan("2 and 4",false,0,false); scan("2.5",false,0,false); scan("-2",false,0,false);
    scan("第2章",true,1,true); scan("D\xcc\x81" "2",true,1,true);
    scan("2,",false,0,false); scan("2 & ",false,0,false); scan("2 3",false,0,false);
    scan("2,,3",false,0,false); scan("2;;3",false,0,false);
    render("2 - 4",CE_CSL_NUMBER_NUMERIC,"2-4");
    render("2,3 &4 – 5",CE_CSL_NUMBER_NUMERIC,"2, 3 & 4–5");
    render("2, 3",CE_CSL_NUMBER_ROMAN,"ii, iii");
    render("L2d, 3E, 第4章, 5",CE_CSL_NUMBER_ROMAN,"L2d, 3E, 第4章, v");
    render("  Special edition  ",CE_CSL_NUMBER_ROMAN,"  Special edition  ");
    render(" 002 ",CE_CSL_NUMBER_NUMERIC,"002");
    render("90071992547409910000000000001",CE_CSL_NUMBER_NUMERIC,"90071992547409910000000000001");
    CeCslOutputString old={0}; bool missing=true;
    assert(ce_csl_number_field_render("keep",4,CE_CSL_NUMBER_NUMERIC,NULL,CE_CSL_LOCALE_NEUTER,NULL,&old,&missing,error,sizeof error));
    assert(!missing);
    const char *bad="90071992547409910000000000001";
    assert(!ce_csl_number_field_render(bad,strlen(bad),CE_CSL_NUMBER_ORDINAL,NULL,CE_CSL_LOCALE_NEUTER,NULL,&old,&missing,error,sizeof error));
    assert(!strcmp(old.data,"keep") && !missing);
    assert(!ce_csl_number_field_render("0",1,CE_CSL_NUMBER_ROMAN,NULL,CE_CSL_LOCALE_NEUTER,NULL,&old,&missing,error,sizeof error));
    assert(!strcmp(old.data,"keep") && !missing);
    CeCslNumberFieldInfo info={true,7,true},before=info;
    assert(!ce_csl_number_field_scan("a\0b",3,&info,error,sizeof error));
    assert(info.numeric==before.numeric && info.number_count==7 && info.single_greater_than_one);
    const char invalid[]={(char)0xc0,(char)0x80,0};
    assert(!ce_csl_number_field_scan(invalid,2,&info,error,sizeof error));
    assert(!ce_csl_number_field_scan("\1",1,&info,error,sizeof error));
    ce_csl_output_string_free(&old);
    CeCslOutputAllocator allocator={NULL,allocate,release};
    bool completed=false;
    for(size_t i=1;i<100;i++) {
        attempts=fail_at=0;
        assert(ce_csl_number_field_render("old",3,CE_CSL_NUMBER_NUMERIC,NULL,CE_CSL_LOCALE_NEUTER,&allocator,&old,&missing,error,sizeof error));
        attempts=0; fail_at=i;
        bool ok=ce_csl_number_field_render("2 & 3",5,CE_CSL_NUMBER_ROMAN,NULL,CE_CSL_LOCALE_NEUTER,&allocator,&old,&missing,error,sizeof error);
        fail_at=0; assert(!strcmp(old.data,ok?"ii & iii":"old") && !missing);
        ce_csl_output_string_free(&old); assert(!live);
        if(ok) { completed=true; break; }
    }
    assert(completed);
    puts("CSL numeric-field extraction, affixes, separator normalization, preservation and OOM passed");
    return 0;
}
