#include "library_panel.h"
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static char *copy(const char *value) { char *out=malloc(strlen(value)+1); assert(out); strcpy(out,value); return out; }
static CeZoteroLibrary fixture(void) {
    CeZoteroLibrary library={0}; char error[128];
    assert(ce_zotero_import_csl("[{\"id\":\"A\",\"title\":\"中文研究\",\"author\":[{\"family\":\"WANG\"}]},{\"id\":\"B\",\"title\":\"Second paper\"},{\"id\":\"C\",\"title\":\"Other\"}]",&library,error,sizeof error));
    library.collection_count=2; library.collections=calloc(2,sizeof *library.collections); assert(library.collections);
    library.root_count=1; library.roots=malloc(sizeof(size_t)); assert(library.roots); library.roots[0]=0;
    library.collections[0].name=copy("父分类"); library.collections[0].key=copy("ROOT");
    library.collections[0].parent_index=SIZE_MAX;
    library.collections[0].child_count=1; library.collections[0].children=malloc(sizeof(size_t)); assert(library.collections[0].children);
    library.collections[0].children[0]=1;
    library.collections[1].name=copy("子分类"); library.collections[1].key=copy("CHILD"); library.collections[1].parent_index=0;
    for(size_t i=0;i<2;i++) {
        library.collections[i].item_count=1; library.collections[i].item_indices=malloc(sizeof(size_t)); assert(library.collections[i].item_indices);
        library.collections[i].item_indices[0]=i;
    }
    return library;
}
int main(void) {
    CeLibraryPanel panel; ce_library_panel_init(&panel); CeZoteroLibrary library=fixture();
    assert(ce_library_panel_replace(&panel,&library)); assert(!library.items && !library.collections);
    assert(panel.row_count==3 && !ce_library_panel_selected(&panel));
    panel.selected_item=0; assert(ce_library_panel_query(&panel,"wang"));
    assert(panel.row_count==1 && panel.rows[0]==0 && ce_library_panel_selected(&panel));
    assert(ce_library_panel_query(&panel,"中文")); assert(panel.row_count==1 && panel.rows[0]==0);
    assert(ce_library_panel_query(&panel,"Second")); assert(panel.row_count==1 && panel.rows[0]==1 && !ce_library_panel_selected(&panel));
    assert(ce_library_panel_query(&panel,"")); assert(ce_library_panel_collection(&panel,0)); assert(panel.row_count==2);
    assert(ce_library_panel_descendants(&panel,false)); assert(panel.row_count==1 && panel.rows[0]==0);
    assert(ce_library_panel_collection(&panel,1)); assert(panel.row_count==1 && panel.rows[0]==1);
    assert(!ce_library_panel_collection(&panel,100)); assert(panel.row_count==1 && panel.collection==1);
    assert(ce_library_panel_collections(&panel,true)); assert(panel.row_count==2);
    assert(ce_library_panel_collection_depth(&panel,0)==0 && ce_library_panel_collection_depth(&panel,1)==1);
    assert(ce_library_panel_query(&panel,"子")); assert(panel.row_count==1 && panel.rows[0]==1);
    library=fixture(); panel.selected_item=2; assert(ce_library_panel_replace(&panel,&library));
    assert(panel.row_count==3 && panel.collection==SIZE_MAX && panel.selected_item==SIZE_MAX && !panel.query && !panel.collections_view);
    ce_library_panel_destroy(&panel);
    const char *good[]={"https://example.com/item", "HTTP://example.com/paper?q=%E4%B8%AD", "zotero://select/groups/7/items/ABCD1234", "zotero://open-pdf/library/items/ABCD1234?page=2"};
    const char *bad[]={NULL,"","javascript:alert(1)","file:///tmp/example","data:text/html,x","https:///missing-host","http://","zotero://debug/x","zotero://select/","zotero://open-pdf/","https://evil\\example.com","https://example.com/ a","https://example.com/%0a","https://example.com/%G1","https://example.com/%"};
    for(size_t i=0;i<sizeof good/sizeof *good;i++) assert(ce_library_uri_allowed(good[i]));
    for(size_t i=0;i<sizeof bad/sizeof *bad;i++) assert(!ce_library_uri_allowed(bad[i]));
    puts("library ownership, search, collection filtering and existing-link validation passed"); return 0;
}
