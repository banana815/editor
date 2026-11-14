#include "csl_format.h"
#include <stdio.h>
#include <string.h>

static bool fail(char *error,size_t capacity,const char *message) {
    if(error && capacity) snprintf(error,capacity,"%s",message);
    return false;
}
static bool valid(unsigned flags) {
    const unsigned mask=CE_CSL_BOLD|CE_CSL_ITALIC|CE_CSL_UNDERLINE|CE_CSL_SMALL_CAPS|
        CE_CSL_SUPERSCRIPT|CE_CSL_SUBSCRIPT|CE_CSL_OBLIQUE|CE_CSL_LIGHT;
    return !(flags&~mask) && !((flags&CE_CSL_BOLD)&&(flags&CE_CSL_LIGHT)) &&
        !((flags&CE_CSL_ITALIC)&&(flags&CE_CSL_OBLIQUE)) &&
        !((flags&CE_CSL_SUPERSCRIPT)&&(flags&CE_CSL_SUBSCRIPT));
}
bool ce_csl_format_resolve(const CeCslXmlNode *node,unsigned inherited,unsigned *resolved,char *error,size_t capacity) {
    if(error && capacity) *error=0;
    if(!resolved || !valid(inherited)) return fail(error,capacity,"Invalid inherited CSL formatting");
    struct Property { const char *name,*normal,*a,*b; unsigned first,second; };
    static const struct Property properties[]={
        {"font-style","normal","italic","oblique",CE_CSL_ITALIC,CE_CSL_OBLIQUE},
        {"font-weight","normal","bold","light",CE_CSL_BOLD,CE_CSL_LIGHT},
        {"font-variant","normal","small-caps",NULL,CE_CSL_SMALL_CAPS,0},
        {"text-decoration","none","underline",NULL,CE_CSL_UNDERLINE,0},
        {"vertical-align","baseline","sup","sub",CE_CSL_SUPERSCRIPT,CE_CSL_SUBSCRIPT}
    };
    unsigned flags=inherited;
    for(size_t i=0;i<sizeof properties/sizeof *properties;i++) {
        const struct Property *p=&properties[i]; const char *value=ce_csl_xml_attribute(node,p->name);
        if(!value) continue;
        unsigned bits=0;
        if(!strcmp(value,p->a)) bits=p->first;
        else if(p->b && !strcmp(value,p->b)) bits=p->second;
        else if(strcmp(value,p->normal)) return fail(error,capacity,"Unknown CSL formatting value");
        flags=(flags&~(p->first|p->second))|bits;
    }
    *resolved=flags; return true;
}
bool ce_csl_format_affixes(const CeCslXmlNode *node,const CeCslOutput *content,
    unsigned affix_format,const CeCslOutputAllocator *allocator,CeCslOutput *destination,char *error,size_t capacity) {
    if(error && capacity) *error=0;
    if(!content || !destination || !valid(affix_format)) return fail(error,capacity,"Invalid CSL affix argument");
    if(!ce_csl_output_text_bytes(content)) return true;
    CeCslOutput *temporary=ce_csl_output_new(allocator,error,capacity); if(!temporary) return false;
    const char *prefix=ce_csl_xml_attribute(node,"prefix"),*suffix=ce_csl_xml_attribute(node,"suffix");
    bool ok=(!prefix || ce_csl_output_append(temporary,prefix,strlen(prefix),affix_format,NULL,error,capacity)) &&
        ce_csl_output_concat(temporary,content,error,capacity) &&
        (!suffix || ce_csl_output_append(temporary,suffix,strlen(suffix),affix_format,NULL,error,capacity)) &&
        ce_csl_output_concat(destination,temporary,error,capacity);
    ce_csl_output_free(temporary); return ok;
}
