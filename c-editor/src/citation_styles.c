#include "citation_styles.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Generated in the build directory after verifying the pinned source bytes. */
#include "citation_styles_data.inc"

struct CeCitationStyle {
    CeCslOutputAllocator allocator;
    const CeCitationStyleInfo *info;
    CeCslStyle *style;
    CeCslXml *en_us, *zh_cn;
    CeCslLocale *locale;
};

static void *allocate_default(void *context, size_t size) {
    (void)context; return malloc(size);
}
static void deallocate_default(void *context, void *memory) {
    (void)context; free(memory);
}
static bool failure(char *error, size_t capacity, const char *message) {
    if (error && capacity) snprintf(error, capacity, "%s", message);
    return false;
}
static bool ascii_equal(const char *a, const char *b) {
    for (;;) {
        unsigned char x=(unsigned char)*a++, y=(unsigned char)*b++;
        if(x>='A' && x<='Z') x=(unsigned char)(x+('a'-'A'));
        if(y>='A' && y<='Z') y=(unsigned char)(y+('a'-'A'));
        if(x!=y) return false;
        if(!x) return true;
    }
}
static const char *supported_language(const char *language) {
    if(ascii_equal(language,"en-US")) return "en-US";
    if(ascii_equal(language,"zh-CN")) return "zh-CN";
    return NULL;
}
const CeCitationStyleInfo *ce_citation_style_info(CeCitationProfile profile) {
    switch(profile) {
        case CE_CITATION_PROFILE_APA: return &citation_style_info[0];
        case CE_CITATION_PROFILE_IEEE: return &citation_style_info[1];
        case CE_CITATION_PROFILE_GBT: return &citation_style_info[2];
    }
    return NULL;
}
const CeCitationStyleInfo *ce_citation_style_loaded_info(const CeCitationStyle *style) {
    return style?style->info:NULL;
}
const CeCslStyle *ce_citation_style_style(const CeCitationStyle *style) {
    return style?style->style:NULL;
}
const CeCslLocale *ce_citation_style_locale(const CeCitationStyle *style) {
    return style?style->locale:NULL;
}
void ce_citation_style_free(CeCitationStyle *style) {
    if(!style) return;
    ce_csl_locale_free(style->locale);
    ce_csl_xml_free(style->en_us);ce_csl_xml_free(style->zh_cn);
    ce_csl_style_free(style->style);
    style->allocator.deallocate(style->allocator.context,style);
}
bool ce_citation_style_load(CeCitationProfile profile, const char *chosen_language,
    const CeCslOutputAllocator *allocator, CeCitationStyle **out,
    char *error, size_t capacity) {
    if(error && capacity) error[0]=0;
    const CeCitationStyleInfo *info=ce_citation_style_info(profile);
    if(!out || !info) return failure(error,capacity,"Invalid citation profile or output");
    if(allocator && (!allocator->allocate || !allocator->deallocate))
        return failure(error,capacity,"Citation style allocator needs both callbacks");
    const char *language=chosen_language?supported_language(chosen_language):NULL;
    if(chosen_language && !language)
        return failure(error,capacity,"Citation locale is not embedded (only en-US and zh-CN are available)");
    CeCslOutputAllocator alloc={NULL,allocate_default,deallocate_default};
    if(allocator) alloc=*allocator;
    CeCitationStyle *candidate=alloc.allocate(alloc.context,sizeof *candidate);
    if(!candidate) return failure(error,capacity,"Out of memory loading citation style");
    memset(candidate,0,sizeof *candidate);candidate->allocator=alloc;candidate->info=info;
    const unsigned char *bytes=NULL;size_t size=0;
    switch(profile) {
        case CE_CITATION_PROFILE_APA: bytes=citation_data_apa;size=sizeof citation_data_apa-1;break;
        case CE_CITATION_PROFILE_IEEE: bytes=citation_data_ieee;size=sizeof citation_data_ieee-1;break;
        case CE_CITATION_PROFILE_GBT: bytes=citation_data_gbt;size=sizeof citation_data_gbt-1;break;
    }
    CeCslStyleAllocator style_allocator={alloc.context,alloc.allocate,alloc.deallocate};
    if(!ce_csl_style_load_with_allocator(&candidate->style,(const char *)bytes,size,
        &style_allocator,error,capacity)) goto failed;
    /* The manifest's public metadata must describe the actual parsed resource. */
    if(strcmp(info->style_id,ce_csl_style_id(candidate->style)) ||
        strcmp(info->title,ce_csl_style_title(candidate->style))) {
        failure(error,capacity,"Embedded CSL metadata does not match the source manifest");goto failed;
    }
    if(!language) {
        const char *default_language=ce_csl_style_default_locale(candidate->style);
        language=supported_language(default_language?default_language:"en-US");
        if(!language) { failure(error,capacity,"Style default locale is not embedded");goto failed; }
    }
    candidate->en_us=ce_csl_xml_parse((const char *)citation_data_en_us,sizeof citation_data_en_us-1,error,capacity);
    if(!candidate->en_us) goto failed;
    candidate->zh_cn=ce_csl_xml_parse((const char *)citation_data_zh_cn,sizeof citation_data_zh_cn-1,error,capacity);
    if(!candidate->zh_cn) goto failed;
    const CeCslXmlNode *roots[]={candidate->en_us->root,candidate->zh_cn->root};
    CeCslLocaleCatalog catalog={roots,2,NULL,0};
    CeCslLocaleAllocator locale_allocator={alloc.context,alloc.allocate,alloc.deallocate};
    candidate->locale=ce_csl_locale_new(ce_csl_style_root(candidate->style),&catalog,language,
        &locale_allocator,error,capacity);
    if(!candidate->locale) goto failed;
    ce_citation_style_free(*out);*out=candidate;return true;
failed:
    ce_citation_style_free(candidate);return false;
}
