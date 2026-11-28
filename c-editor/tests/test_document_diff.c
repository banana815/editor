#include "document_diff.h"
#include <assert.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
int main(void){char error[256];CeDocument *a=ce_document_new(),*b=ce_document_new();assert(a&&b);assert(ce_document_import(a,"<h1>标题</h1><p>旧段落</p><p>结尾</p>",CE_FORMAT_HTML,error,sizeof error));assert(ce_document_import(b,"<h1>标题</h1><p>新段落</p><p>结尾</p><p>新增段落</p>",CE_FORMAT_HTML,error,sizeof error));
    char *before=ce_document_export_json(a),*after=ce_document_export_json(b);uint64_t revision=ce_document_revision(b);char *report=ce_document_diff(a,b,error,sizeof error);assert(report&&strstr(report,"新增 1")&&strstr(report,"修改 1")&&strstr(report,"旧段落")&&strstr(report,"新段落")&&strstr(report,"新增段落"));free(report);assert(ce_document_revision(b)==revision);char *check=ce_document_export_json(a);assert(!strcmp(check,before));free(check);check=ce_document_export_json(b);assert(!strcmp(check,after));free(check);free(before);free(after);
    assert(ce_document_import(a,"<p>相同文字</p>",CE_FORMAT_HTML,error,sizeof error));assert(ce_document_import(b,"<p><strong>相同文字</strong></p>",CE_FORMAT_HTML,error,sizeof error));report=ce_document_diff(a,b,error,sizeof error);assert(report&&strstr(report,"修改 1")&&strstr(report,"结构/属性")&&strstr(report,"bold"));free(report);
    report=ce_document_diff(a,a,error,sizeof error);assert(report&&strstr(report,"正文与段落属性相同"));free(report);
    /* Repeated long identical paragraphs must compare via exact interned IDs,
     * avoiding a full 2 KiB string comparison at every LCS matrix cell. */
    size_t rows=1200,width=2048;char *long_text=malloc(rows*(width+1)+1);assert(long_text);
    for(size_t i=0;i<rows;i++){memset(long_text+i*(width+1),'x',width);long_text[i*(width+1)+width]='\n';}long_text[rows*(width+1)]=0;
    assert(ce_document_import(a,long_text,CE_FORMAT_TEXT,error,sizeof error));assert(ce_document_import(b,long_text,CE_FORMAT_TEXT,error,sizeof error));report=ce_document_diff(a,b,error,sizeof error);assert(report&&strstr(report,"修改 0"));free(report);free(long_text);
    ce_document_free(a);ce_document_free(b);puts("Read-only paragraph, formatting and large repeated-line differences passed");return 0;}
