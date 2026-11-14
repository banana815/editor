#include "csl_number_field.h"
#include "csl_unicode.h"
#include <stdio.h>
#include <string.h>

typedef struct Part {
    size_t start,end,digits,digits_end,separator,separator_size;
} Part;
static bool fail(char *error,size_t capacity,const char *message) {
    if(error && capacity) snprintf(error,capacity,"%s",message);
    return false;
}
static bool digit(char c) { return c>='0' && c<='9'; }
static uint32_t next(const char *text,size_t size,size_t at,size_t *after) {
    uint32_t cp=0; *after=at;
    (void)ce_csl_unicode_next(text,size,after,&cp); return cp;
}
static void spaces(const char *text,size_t size,size_t *at) {
    while(*at<size) {
        size_t after; uint32_t cp=next(text,size,*at,&after);
        if(!ce_csl_unicode_is_space(cp)) break;
        *at=after;
    }
}
static bool letters(const char *text,size_t size,size_t *at) {
    bool have_letter=false;
    while(*at<size) {
        size_t after; uint32_t cp=next(text,size,*at,&after);
        if(ce_csl_unicode_is_letter(cp)) have_letter=true;
        else if(!have_letter || !ce_csl_unicode_is_mark(cp)) break;
        *at=after;
    }
    return have_letter;
}
static bool part(const char *text,size_t size,size_t *at,Part *out) {
    memset(out,0,sizeof *out); spaces(text,size,at); out->start=*at;
    (void)letters(text,size,at); out->digits=*at;
    while(*at<size && digit(text[*at])) ++*at;
    out->digits_end=*at; if(out->digits==out->digits_end) return false;
    (void)letters(text,size,at); out->end=*at; spaces(text,size,at);
    if(*at==size) return true;
    size_t after; uint32_t separator=next(text,size,*at,&after);
    if(separator!=',' && separator!='-' && separator!='&' && separator!=0x2013) return false;
    out->separator=*at; out->separator_size=after-*at; *at=after;
    spaces(text,size,at); return *at<size;
}
bool ce_csl_number_field_scan(const char *text,size_t size,CeCslNumberFieldInfo *info,char *error,size_t capacity) {
    if(error && capacity) *error=0;
    if(!info || (!text && size) || size>CE_CSL_OUTPUT_MAX_BYTES ||
       ce_csl_unicode_validate(text,size,NULL)!=CE_CSL_UNICODE_OK)
        return fail(error,capacity,"Invalid or oversized numeric field");
    for(size_t i=0;i<size;i++) if((unsigned char)text[i]<0x20 && text[i]!='\t' && text[i]!='\r' && text[i]!='\n')
        return fail(error,capacity,"Invalid control character in numeric field");
    CeCslNumberFieldInfo result={0}; size_t at=0; spaces(text,size,&at);
    if(at==size) { *info=result; return true; }
    result.numeric=true;
    while(at<size) {
        Part token;
        if(!part(text,size,&at,&token)) { result=(CeCslNumberFieldInfo){0}; break; }
        ++result.number_count;
        if(result.number_count==1) {
            size_t first=token.digits;
            while(first<token.digits_end && text[first]=='0') ++first;
            result.single_greater_than_one=token.digits_end-first>1 || (first<token.digits_end && text[first]>'1');
        } else result.single_greater_than_one=false;
    }
    *info=result; return true;
}
bool ce_csl_number_field_render(const char *text,size_t size,CeCslNumberForm form,
    const CeCslLocale *locale,CeCslLocaleGender gender,const CeCslOutputAllocator *allocator,
    CeCslOutputString *output,bool *missing_term,char *error,size_t capacity) {
    if(error && capacity) *error=0;
    if(!output || form<CE_CSL_NUMBER_NUMERIC || form>CE_CSL_NUMBER_ROMAN ||
       gender<CE_CSL_LOCALE_NEUTER || gender>CE_CSL_LOCALE_FEMININE)
        return fail(error,capacity,"Invalid numeric-field rendering options");
    CeCslNumberFieldInfo info;
    if(!ce_csl_number_field_scan(text,size,&info,error,capacity)) return false;
    CeCslOutput *temporary=ce_csl_output_new(allocator,error,capacity); if(!temporary) return false;
    bool ok=true,missing=false;
    if(!info.numeric) ok=ce_csl_output_append(temporary,text,size,0,NULL,error,capacity);
    else {
        size_t at=0;
        while(ok && at<size) {
            Part token; if(!part(text,size,&at,&token)) { ok=false; break; }
            bool affixed=token.digits!=token.start || token.digits_end!=token.end;
            if(affixed || form==CE_CSL_NUMBER_NUMERIC)
                ok=ce_csl_output_append(temporary,text+token.start,token.end-token.start,0,NULL,error,capacity);
            else {
                uint64_t number=0;
                for(size_t i=token.digits;i<token.digits_end;i++) {
                    unsigned d=(unsigned)(text[i]-'0');
                    if(number>(UINT64_MAX-d)/10) { ok=fail(error,capacity,"Numeric field exceeds scalar formatting limit"); break; }
                    number=number*10+d;
                }
                if(ok) {
                    CeCslOutputString converted={0}; bool absent=false;
                    ok=ce_csl_number_format(number,form,locale,gender,allocator,&converted,&absent,error,capacity)==CE_CSL_NUMBER_OK;
                    if(ok) { missing|=absent; ok=ce_csl_output_append(temporary,converted.data,converted.length,0,NULL,error,capacity); }
                    ce_csl_output_string_free(&converted);
                }
            }
            if(ok && token.separator_size) {
                const char *separator=text+token.separator; size_t length=token.separator_size;
                if(*separator==',') { separator=", "; length=2; }
                else if(*separator=='&') { separator=" & "; length=3; }
                ok=ce_csl_output_append(temporary,separator,length,0,NULL,error,capacity);
            }
        }
    }
    if(ok) ok=ce_csl_output_plain(temporary,output,error,capacity);
    ce_csl_output_free(temporary);
    if(ok && missing_term) *missing_term=missing;
    return ok;
}
