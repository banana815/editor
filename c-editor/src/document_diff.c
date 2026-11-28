#include "document_diff.h"
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
typedef struct {char *json,*text;CeBlockInfo info;size_t identity;} Block;
typedef struct {char *data;size_t size;} Buffer;
static bool append(Buffer *b,const char *s){size_t n=strlen(s);if(n>64u*1024u*1024u-b->size)return false;char *p=realloc(b->data,b->size+n+1);if(!p)return false;memcpy(p+b->size,s,n+1);b->data=p;b->size+=n;return true;}
static void free_blocks(Block *b,size_t n){if(!b)return;for(size_t i=0;i<n;i++){free(b[i].json);free(b[i].text);}free(b);}
static Block *blocks(const CeDocument *doc,size_t *count){*count=ce_document_block_count(doc);if(*count>250000)return NULL;Block *b=calloc(*count?*count:1,sizeof *b);if(!b)return NULL;const char *text=ce_document_text(doc);size_t allocated=0;for(size_t i=0;i<*count;i++){if(!ce_document_block_info(doc,i,&b[i].info)){free_blocks(b,*count);return NULL;}size_t start=b[i].info.start,end=b[i].info.end;b[i].json=ce_document_block_json(doc,i);b[i].text=malloc(end-start+1);if(!b[i].json||!b[i].text){free_blocks(b,*count);return NULL;}size_t cost=strlen(b[i].json)+end-start;if(cost>64u*1024u*1024u-allocated){free_blocks(b,*count);return NULL;}allocated+=cost;memcpy(b[i].text,text+start,end-start);b[i].text[end-start]=0;}return b;}
static int compare_blocks(const void *a,const void *b){return strcmp((*(Block *const *)a)->json,(*(Block *const *)b)->json);}
static bool identities(Block *a,size_t na,Block *b,size_t nb){size_t count=na+nb;Block **sorted=malloc((count?count:1)*sizeof *sorted);if(!sorted)return false;for(size_t i=0;i<na;i++)sorted[i]=&a[i];for(size_t i=0;i<nb;i++)sorted[na+i]=&b[i];qsort(sorted,count,sizeof *sorted,compare_blocks);size_t id=0;for(size_t i=0;i<count;i++){if(!i||strcmp(sorted[i-1]->json,sorted[i]->json))id++;sorted[i]->identity=id;}free(sorted);return true;}
static bool same(const Block *a,const Block *b){return a->identity==b->identity;}
static bool emit(Buffer *out,const char *prefix,const Block *b,bool details){char heading[64];snprintf(heading,sizeof heading,"%s%s",prefix,b->info.kind==CE_BLOCK_HEADING?"[标题] ":"");const char *text=b->info.kind==CE_BLOCK_IMAGE?"[图片]":b->info.kind==CE_BLOCK_RULE?"[分隔线]":b->info.kind==CE_BLOCK_CITATION?"[引文]":b->info.kind==CE_BLOCK_ENDNOTES?"[参考文献]":*b->text?b->text:"[空段落]";return append(out,heading)&&append(out,text)&&append(out,"\n")&&(!details||(append(out,"  结构/属性：")&&append(out,b->json)&&append(out,"\n")));}
static bool changes(Buffer *out,Block *a,size_t first,size_t end,Block *b,size_t start,size_t last,size_t *added,size_t *removed,size_t *modified){
    size_t pairs=end-first<last-start?end-first:last-start;*modified+=pairs;*removed+=end-first-pairs;*added+=last-start-pairs;
    for(size_t k=0;k<pairs;k++){bool details=!strcmp(a[first+k].text,b[start+k].text);if(!emit(out,"− 修改前：",&a[first+k],details)||!emit(out,"+ 修改后：",&b[start+k],details)||!append(out,"\n"))return false;}
    for(size_t i=first+pairs;i<end;i++)if(!emit(out,"− 删除：",&a[i],a[i].info.kind>=CE_BLOCK_CITATION)||!append(out,"\n"))return false;
    for(size_t i=start+pairs;i<last;i++)if(!emit(out,"+ 新增：",&b[i],b[i].info.kind>=CE_BLOCK_CITATION)||!append(out,"\n"))return false;
    return true;
}
char *ce_document_diff(const CeDocument *before,const CeDocument *after,char *error,size_t cap){
    if(!before||!after)return NULL;size_t na=0,nb=0;Block *a=blocks(before,&na),*b=blocks(after,&nb);Buffer out={0},report={0};uint32_t *lcs=NULL;size_t added=0,removed=0,modified=0;bool ok=a&&b;
    if(ok)ok=identities(a,na,b,nb);
    if(ok&&na<=4096&&nb<=4096&&(na+1)*(nb+1)<=4194304){size_t stride=nb+1;lcs=calloc((na+1)*stride,sizeof *lcs);if(!lcs)ok=false;else for(size_t i=na;i-->0;)for(size_t j=nb;j-->0;)lcs[i*stride+j]=same(&a[i],&b[j])?1+lcs[(i+1)*stride+j+1]:(lcs[(i+1)*stride+j]>lcs[i*stride+j+1]?lcs[(i+1)*stride+j]:lcs[i*stride+j+1]);}
    size_t i=0,j=0;
    while(ok&&(i<na||j<nb)){
        if(i<na&&j<nb&&same(&a[i],&b[j])){i++;j++;continue;}size_t ai=i,bj=j;
        if(lcs){while((i<na||j<nb)&&!(i<na&&j<nb&&same(&a[i],&b[j]))){if(i<na&&(j==nb||lcs[(i+1)*(nb+1)+j]>=lcs[i*(nb+1)+j+1]))i++;else j++;}}
        else {size_t best=SIZE_MAX,di=na-i,dj=nb-j;for(size_t x=0;x<64&&i+x<na;x++)for(size_t y=0;y<64&&j+y<nb;y++)if(x+y<best&&same(&a[i+x],&b[j+y])){best=x+y;di=x;dj=y;}if(best==SIZE_MAX){i=na;j=nb;}else{i+=di;j+=dj;}}
        ok=changes(&out,a,ai,i,b,bj,j,&added,&removed,&modified);
    }
    if(ok){char summary[256];snprintf(summary,sizeof summary,"所选历史版本 → 当前文章\n新增 %zu 段 · 删除 %zu 段 · 修改 %zu 段\n\n",added,removed,modified);ok=append(&report,summary)&&append(&report,out.data?out.data:"正文与段落属性相同。\n");
        char *old_json=ce_document_export_json(before),*new_json=ce_document_export_json(after);if(!old_json||!new_json)ok=false;else if(!added&&!removed&&!modified&&strcmp(old_json,new_json))ok=ok&&append(&report,"文档级元数据或容器属性存在差异；恢复将采用所选版本的完整数据。\n");free(old_json);free(new_json);}
    free_blocks(a,na);free_blocks(b,nb);free(lcs);free(out.data);if(!ok){free(report.data);if(error&&cap)snprintf(error,cap,"Cannot compare these versions within memory limits");return NULL;}return report.data;
}
