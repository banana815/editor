#include "csl_condition.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
static char error[256];
static bool variable(void *context,const char *name,CeCslVariableFacts *facts,char *message,size_t capacity) {
    (void)context; (void)message; (void)capacity;
    if(!strcmp(name,"failure")) return false;
    facts->present=!strcmp(name,"title") || !strcmp(name,"edition") || !strcmp(name,"issued");
    facts->numeric=!strcmp(name,"edition"); facts->uncertain_date=!strcmp(name,"issued"); return true;
}
static CeCslXml *parse(const char *xml) { CeCslXml *x=ce_csl_xml_parse(xml,strlen(xml),error,sizeof error); assert(x); return x; }
static void check(const char *attributes,CeCslConditionContext *context,bool expected) {
    char xml[1024]; snprintf(xml,sizeof xml,"<if xmlns=\"%s\" %s/>",CE_CSL_NAMESPACE,attributes);
    CeCslXml *x=parse(xml); bool result=!expected;
    assert(ce_csl_condition_test(x->root,context,&result,error,sizeof error)); assert(result==expected); ce_csl_xml_free(x);
}
int main(void) {
    CeCslConditionContext context={0}; context.variable=variable; context.item_type="book"; context.position=CE_CSL_POSITION_FIRST;
    check("variable=\"title missing\"",&context,false);
    check("variable=\"title missing\" match=\"any\"",&context,true);
    check("variable=\"title missing\" match=\"none\"",&context,false);
    check("variable=\"missing absent\" match=\"none\"",&context,true);
    check("variable=\"title\" type=\"book article\"",&context,false);
    check("variable=\"missing\" type=\"article book\" match=\"any\"",&context,true);
    check("is-numeric=\"edition title\" match=\"any\"",&context,true);
    check("is-uncertain-date=\"issued\"",&context,true);
    check("locator=\"page\"",&context,false); context.locator_present=true;
    check("locator=\"page\"",&context,true);
    context.locator_type="sub verbo"; check("locator=\"sub-verbo\"",&context,true);
    check("position=\"first\"",&context,true); check("position=\"subsequent\"",&context,false);
    context.position=CE_CSL_POSITION_IBID_WITH_LOCATOR;
    check("position=\"subsequent ibid ibid-with-locator\"",&context,true);
    context.position=CE_CSL_POSITION_NEAR_NOTE;
    check("position=\"subsequent near-note\"",&context,true); check("position=\"ibid\"",&context,false);
    context.bibliography=true; check("position=\"subsequent near-note\" match=\"any\"",&context,false);
    check("position=\"first subsequent\" match=\"none\"",&context,true);
    check("disambiguate=\"true\"",&context,false); context.disambiguate=true;
    check("disambiguate=\"true\"",&context,true);
    check("disambiguate=\"true\" variable=\"missing\"",&context,false);
    const char *invalid[]={"", "match=\"all\"", "match=\"bogus\" variable=\"title\"", "variable=\" \"", "unknown=\"title\"", "position=\"last\"", "disambiguate=\"false\"", "variable=\"failure\""};
    for(size_t i=0;i<sizeof invalid/sizeof *invalid;i++) {
        char xml[1024]; snprintf(xml,sizeof xml,"<if xmlns=\"%s\" %s/>",CE_CSL_NAMESPACE,invalid[i]);
        CeCslXml *x=parse(xml); bool result=true;
        assert(!ce_csl_condition_test(x->root,&context,&result,error,sizeof error)); assert(result && *error); ce_csl_xml_free(x);
    }
    CeCslXml *x=parse("<choose xmlns=\"http://purl.org/net/xbiblio/csl\"><if variable=\"missing\"/><else-if type=\"book\"><text value=\"book\"/></else-if><else><text value=\"fallback\"/></else></choose>");
    const CeCslXmlNode *branch=NULL;
    assert(ce_csl_choose_branch(x->root,&context,&branch,error,sizeof error)); assert(branch==x->root->children->next);
    context.item_type="article"; assert(ce_csl_choose_branch(x->root,&context,&branch,error,sizeof error)); assert(branch==x->root->last_child);
    context.position=CE_CSL_POSITION_FIRST|CE_CSL_POSITION_SUBSEQUENT;
    const CeCslXmlNode *before=branch;
    assert(!ce_csl_choose_branch(x->root,&context,&branch,error,sizeof error) && branch==before);
    ce_csl_xml_free(x); context.position=CE_CSL_POSITION_FIRST;
    x=parse("<choose xmlns=\"http://purl.org/net/xbiblio/csl\"><if variable=\"title\"/><else-if bogus=\"x\"/></choose>");
    assert(!ce_csl_choose_branch(x->root,&context,&branch,error,sizeof error) && branch==before); ce_csl_xml_free(x);
    x=parse("<choose xmlns=\"http://purl.org/net/xbiblio/csl\"><if variable=\"missing\"/></choose>");
    assert(ce_csl_choose_branch(x->root,&context,&branch,error,sizeof error) && !branch); ce_csl_xml_free(x);
    puts("CSL all/any/none, ordered branches, cite positions and resolver failure propagation passed");
    return 0;
}
