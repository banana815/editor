#ifndef CE_LIBRARY_PANEL_H
#define CE_LIBRARY_PANEL_H
#include "zotero.h"

/* Owns the imported library, query and visible index array. No borrowed item
 * pointers survive replacement. Initialize before use; destroy on shutdown. */
typedef struct CeLibraryPanel {
    CeZoteroLibrary library;
    char *query;
    size_t collection, selected_item;
    bool include_descendants, collections_view;
    size_t *rows, row_count;
    float scroll;
} CeLibraryPanel;
void ce_library_panel_init(CeLibraryPanel *panel);
void ce_library_panel_destroy(CeLibraryPanel *panel);
/* Atomic ownership transfer: success empties library, failure preserves both. */
bool ce_library_panel_replace(CeLibraryPanel *panel, CeZoteroLibrary *library);
bool ce_library_panel_query(CeLibraryPanel *panel, const char *query);
bool ce_library_panel_collection(CeLibraryPanel *panel, size_t collection);
bool ce_library_panel_descendants(CeLibraryPanel *panel, bool include);
bool ce_library_panel_collections(CeLibraryPanel *panel, bool show);
/* All references returned by these accessors last only until replacement. */
const CeZoteroItem *ce_library_panel_selected(const CeLibraryPanel *panel);
size_t ce_library_panel_collection_depth(const CeLibraryPanel *panel, size_t index);
/* Existing absolute http(s), Zotero select/open-pdf links only. Does not make
 * up a URI from an item key or a library that has no usable link. */
bool ce_library_uri_allowed(const char *uri);
#endif
