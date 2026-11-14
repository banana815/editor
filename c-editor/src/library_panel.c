#include "library_panel.h"
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

static char *copy_string(const char *text) {
    size_t n=strlen(text)+1; char *copy=malloc(n);
    if(copy) memcpy(copy,text,n); return copy;
}
static unsigned char fold(unsigned char ch) { return ch>='A' && ch<='Z'?(unsigned char)(ch+32):ch; }
static bool matches(const char *value,const char *query) {
    if(!*query) return true;
    if(!value) return false;
    for(const unsigned char *start=(const unsigned char *)value;*start;start++) {
        const unsigned char *at=start,*needle=(const unsigned char *)query;
        while(*at && *needle && fold(*at)==fold(*needle)) { at++; needle++; }
        if(!*needle) return true;
    }
    return false;
}
static bool item_matches(const CeZoteroItem *item,const char *query) {
    return matches(item->title,query) || matches(item->author,query) || matches(item->year,query) ||
        matches(item->cite_key,query) || matches(item->item_key,query);
}
static bool rebuild(CeLibraryPanel *panel,const char *query,size_t collection,bool descendants,bool collections) {
    size_t *candidates=NULL,count=0;
    if(collections) count=panel->library.collection_count;
    else if(collection!=SIZE_MAX) {
        char error[128];
        if(collection>=panel->library.collection_count ||
           !ce_zotero_collection_items(&panel->library,collection,descendants,&candidates,&count,error,sizeof error)) return false;
    } else count=panel->library.item_count;
    if(count>SIZE_MAX/sizeof(size_t)) { free(candidates); return false; }
    size_t *rows=count?malloc(count*sizeof(size_t)):NULL,used=0;
    if(count && !rows) { free(candidates); return false; }
    for(size_t i=0;i<count;i++) {
        size_t index=candidates?candidates[i]:i;
        if(collections?matches(panel->library.collections[index].name,query):item_matches(&panel->library.items[index],query))
            rows[used++]=index;
    }
    free(candidates); free(panel->rows); panel->rows=rows; panel->row_count=used;
    panel->scroll=0;
    if(!collections) {
        bool selected=false;
        for(size_t i=0;i<used;i++) if(rows[i]==panel->selected_item) selected=true;
        if(!selected) panel->selected_item=SIZE_MAX;
    }
    return true;
}
void ce_library_panel_init(CeLibraryPanel *panel) {
    memset(panel,0,sizeof *panel); panel->collection=panel->selected_item=SIZE_MAX;
    panel->include_descendants=true;
}
void ce_library_panel_destroy(CeLibraryPanel *panel) {
    if(!panel) return;
    ce_zotero_library_free(&panel->library); free(panel->query); free(panel->rows);
    ce_library_panel_init(panel);
}
bool ce_library_panel_replace(CeLibraryPanel *panel,CeZoteroLibrary *library) {
    if(!panel || !library || library==&panel->library) return false;
    CeLibraryPanel next; ce_library_panel_init(&next); next.library=*library;
    if(!rebuild(&next,"",SIZE_MAX,true,false)) return false;
    ce_library_panel_destroy(panel); *panel=next; memset(library,0,sizeof *library); return true;
}
bool ce_library_panel_query(CeLibraryPanel *panel,const char *query) {
    if(!panel || !query) return false;
    char *owned=copy_string(query); if(!owned) return false;
    if(!rebuild(panel,query,panel->collection,panel->include_descendants,panel->collections_view)) { free(owned); return false; }
    free(panel->query); panel->query=owned; return true;
}
bool ce_library_panel_collection(CeLibraryPanel *panel,size_t collection) {
    if(!rebuild(panel,panel->query?panel->query:"",collection,panel->include_descendants,false)) return false;
    panel->collection=collection; panel->collections_view=false; return true;
}
bool ce_library_panel_descendants(CeLibraryPanel *panel,bool include) {
    if(!rebuild(panel,panel->query?panel->query:"",panel->collection,include,panel->collections_view)) return false;
    panel->include_descendants=include; return true;
}
bool ce_library_panel_collections(CeLibraryPanel *panel,bool show) {
    if(!rebuild(panel,panel->query?panel->query:"",panel->collection,panel->include_descendants,show)) return false;
    panel->collections_view=show; return true;
}
const CeZoteroItem *ce_library_panel_selected(const CeLibraryPanel *panel) {
    return panel && panel->selected_item<panel->library.item_count?&panel->library.items[panel->selected_item]:NULL;
}
size_t ce_library_panel_collection_depth(const CeLibraryPanel *panel,size_t index) {
    size_t depth=0;
    if(!panel || index>=panel->library.collection_count) return 0;
    size_t next=panel->library.collections[index].parent_index;
    while(next<panel->library.collection_count && depth<panel->library.collection_count) {
        depth++; next=panel->library.collections[next].parent_index;
    }
    return depth;
}
static bool prefix(const char *value,const char *expected) {
    while(*expected) if(!*value || fold((unsigned char)*value++)!=(unsigned char)*expected++) return false;
    return true;
}
static int hex(unsigned char ch) {
    if(ch>='0' && ch<='9') return ch-'0';
    ch=fold(ch); return ch>='a' && ch<='f'?ch-'a'+10:-1;
}
bool ce_library_uri_allowed(const char *uri) {
    if(!uri || !*uri) return false;
    const char *rest=NULL;
    if(prefix(uri,"https://")) rest=uri+8;
    else if(prefix(uri,"http://")) rest=uri+7;
    else if(prefix(uri,"zotero://select/")) rest=uri+16;
    else if(prefix(uri,"zotero://open-pdf/")) rest=uri+18;
    if(!rest || !*rest || *rest=='/' || *rest=='?' || *rest=='#') return false;
    for(const unsigned char *p=(const unsigned char *)uri;*p;p++) {
        if(*p<=32 || *p==127 || *p=='\\') return false;
        if(*p=='%') {
            if(!p[1] || !p[2]) return false;
            int a=hex(p[1]),b=hex(p[2]);
            if(a<0 || b<0 || a*16+b<32 || a*16+b==127) return false;
            p+=2;
        }
    }
    return true;
}
