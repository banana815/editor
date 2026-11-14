#include "csl_condition.h"
#include <stdio.h>
#include <string.h>

#define MAX_TESTS ((size_t)4096)
typedef enum Predicate { VARIABLE,NUMERIC,UNCERTAIN,TYPE,LOCATOR,POSITION } Predicate;
static bool fail(char *error,size_t capacity,const char *message) {
    if(error && capacity) snprintf(error,capacity,"%s",message);
    return false;
}
static bool space(char c) { return c==' ' || c=='\t' || c=='\r' || c=='\n'; }
static bool valid_context(const CeCslConditionContext *context) {
    if(!context || context->position&~31u) return false;
    return !(context->position&CE_CSL_POSITION_FIRST) || !(context->position&~CE_CSL_POSITION_FIRST);
}
static unsigned positions(unsigned value) {
    if(value&CE_CSL_POSITION_IBID_WITH_LOCATOR) value|=CE_CSL_POSITION_IBID;
    if(value&(CE_CSL_POSITION_IBID|CE_CSL_POSITION_NEAR_NOTE)) value|=CE_CSL_POSITION_SUBSEQUENT;
    return value;
}
static bool position(const char *name,unsigned *bit) {
    static const char *const names[]={"first","subsequent","ibid","ibid-with-locator","near-note"};
    for(unsigned i=0;i<5;i++) if(!strcmp(name,names[i])) { *bit=1u<<i; return true; }
    return false;
}
static bool predicate(Predicate predicate_kind,const char *name,const CeCslConditionContext *context,
    bool *result,char *error,size_t capacity) {
    if(predicate_kind==TYPE) { *result=context->item_type && !strcmp(name,context->item_type); return true; }
    if(predicate_kind==LOCATOR) {
        if(!context->locator_present) { *result=false; return true; }
        const char *type=context->locator_type && *context->locator_type?context->locator_type:"page";
        if(!strcmp(type,"sub verbo")) type="sub-verbo";
        *result=!strcmp(type,name); return true;
    }
    if(predicate_kind==POSITION) {
        unsigned bit=0; if(!position(name,&bit)) return fail(error,capacity,"Unknown CSL position predicate");
        *result=!context->bibliography && (positions(context->position)&bit)!=0; return true;
    }
    if(!context->variable) return fail(error,capacity,"CSL variable resolver is required");
    CeCslVariableFacts facts={0};
    if(!context->variable(context->context,name,&facts,error,capacity)) {
        if(error && capacity && !*error) return fail(error,capacity,"CSL variable resolver failed");
        return false;
    }
    *result=predicate_kind==VARIABLE?facts.present:predicate_kind==NUMERIC?facts.numeric:facts.uncertain_date;
    return true;
}
static bool kind(const char *name,Predicate *out) {
    static const char *const names[]={"variable","is-numeric","is-uncertain-date","type","locator","position"};
    for(unsigned i=0;i<6;i++) if(!strcmp(name,names[i])) { *out=(Predicate)i; return true; }
    return false;
}
static bool validate(const CeCslXmlNode *condition,char *error,size_t capacity) {
    if(!ce_csl_xml_is(condition,"if") && !ce_csl_xml_is(condition,"else-if")) return fail(error,capacity,"Expected a CSL condition");
    size_t tests=0;
    for(size_t i=0;i<condition->attribute_count;i++) {
        const CeCslXmlAttr *attribute=&condition->attributes[i];
        if(!strcmp(attribute->name,"match")) {
            if(strcmp(attribute->value,"all") && strcmp(attribute->value,"any") && strcmp(attribute->value,"none")) return fail(error,capacity,"Unknown CSL match rule");
            continue;
        }
        if(!strcmp(attribute->name,"disambiguate")) {
            if(strcmp(attribute->value,"true")) return fail(error,capacity,"CSL disambiguate condition must be true");
            ++tests; continue;
        }
        Predicate type; if(!kind(attribute->name,&type)) return fail(error,capacity,"Unknown CSL condition attribute");
        const char *p=attribute->value; size_t count=0;
        while(*p) {
            while(space(*p)) ++p;
            if(!*p) break;
            const char *start=p; while(*p && !space(*p)) ++p;
            size_t length=(size_t)(p-start); if(length>255) return fail(error,capacity,"CSL condition token is too long");
            if(type==POSITION) {
                char name[256]; memcpy(name,start,length); name[length]=0; unsigned bit;
                if(!position(name,&bit)) return fail(error,capacity,"Unknown CSL position predicate");
            }
            ++count; if(++tests>MAX_TESTS) return fail(error,capacity,"Too many CSL condition tests");
        }
        if(!count) return fail(error,capacity,"Empty CSL condition list");
    }
    if(!tests || tests>MAX_TESTS) return fail(error,capacity,"Invalid number of CSL condition tests");
    return true;
}
bool ce_csl_condition_test(const CeCslXmlNode *condition,const CeCslConditionContext *context,
    bool *matches,char *error,size_t capacity) {
    if(error && capacity) *error=0;
    if(!matches || !valid_context(context)) return fail(error,capacity,"Invalid CSL condition context");
    if(!validate(condition,error,capacity)) return false;
    bool any=false,all=true;
    for(size_t i=0;i<condition->attribute_count;i++) {
        const CeCslXmlAttr *attribute=&condition->attributes[i];
        if(!strcmp(attribute->name,"match")) continue;
        if(!strcmp(attribute->name,"disambiguate")) { any|=context->disambiguate; all&=context->disambiguate; continue; }
        Predicate type=VARIABLE; (void)kind(attribute->name,&type);
        const char *p=attribute->value;
        while(*p) {
            while(space(*p)) ++p;
            if(!*p) break;
            const char *start=p; while(*p && !space(*p)) ++p;
            size_t length=(size_t)(p-start); char name[256]; memcpy(name,start,length); name[length]=0;
            bool result=false; if(!predicate(type,name,context,&result,error,capacity)) return false;
            any|=result; all&=result;
        }
    }
    const char *match=ce_csl_xml_attribute(condition,"match");
    *matches=match && !strcmp(match,"none")?!any:match && !strcmp(match,"any")?any:all;
    return true;
}
bool ce_csl_choose_branch(const CeCslXmlNode *choose,const CeCslConditionContext *context,
    const CeCslXmlNode **branch,char *error,size_t capacity) {
    if(error && capacity) *error=0;
    if(!branch || !valid_context(context) || !ce_csl_xml_is(choose,"choose") ||
       choose->attribute_count || !ce_csl_xml_is(choose->children,"if"))
        return fail(error,capacity,"Invalid CSL choose element or context");
    for(const CeCslXmlNode *node=choose->children;node;node=node->next) {
        if(node!=choose->children && !ce_csl_xml_is(node,"else-if") && !ce_csl_xml_is(node,"else")) return fail(error,capacity,"Invalid CSL choose branch order");
        if(ce_csl_xml_is(node,"else")) {
            if(node->next || node->attribute_count || !node->children) return fail(error,capacity,"Invalid CSL else branch");
        } else if(!validate(node,error,capacity)) return false;
    }
    for(const CeCslXmlNode *node=choose->children;node;node=node->next) {
        bool match=ce_csl_xml_is(node,"else");
        if(!match && !ce_csl_condition_test(node,context,&match,error,capacity)) return false;
        if(match) { *branch=node; return true; }
    }
    *branch=NULL; return true;
}
