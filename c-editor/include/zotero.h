#ifndef CE_ZOTERO_H
#define CE_ZOTERO_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct CeZoteroItem {
    int64_t database_id, library_id;
    char *id, *item_key, *title, *author, *year, *cite_key;
    char *item_uri, *pdf_uri;
    char *csl_json; /* Original CSL object, or NULL for a SQLite item. */
} CeZoteroItem;

typedef struct CeZoteroCollection {
    int64_t id, library_id, parent_id;
    bool has_parent;
    char *key, *name;
    size_t parent_index; /* SIZE_MAX for a root or repaired invalid parent. */
    size_t *children, child_count;
    size_t *item_indices, item_count; /* Direct, non-deleted bibliographic items. */
} CeZoteroCollection;

typedef struct CeZoteroLibrary {
    CeZoteroItem *items;
    size_t item_count;
    CeZoteroCollection *collections;
    size_t collection_count;
    size_t *roots, root_count;
    size_t repaired_parents; /* Cycles, orphans and cross-library parents. */
} CeZoteroLibrary;

/* Zero-initialize outputs. Success atomically replaces the previous library;
 * failure preserves it. Arrays and their strings belong to the library. */
bool ce_zotero_import_csl(const char *json, CeZoteroLibrary *library,
                          char *error, size_t error_capacity);
/* Accepts UTF-8 native paths or an imported database in a virtual filesystem.
 * Opens SQLite READONLY, reads one consistent transaction and never modifies
 * Zotero data. This loader does not discover/access any default user path. */
bool ce_zotero_load_sqlite(const char *path, CeZoteroLibrary *library,
                           char *error, size_t error_capacity);
void ce_zotero_library_free(CeZoteroLibrary *library);

/* Deduplicated item indices in library order. include_descendants follows the
 * repaired, acyclic collection tree. Free returned indices with free(). */
bool ce_zotero_collection_items(const CeZoteroLibrary *library, size_t collection,
                                bool include_descendants, size_t **indices,
                                size_t *count, char *error, size_t error_capacity);

typedef enum CeZoteroApproximateStyle {
    CE_ZOTERO_APPROXIMATE_APA,
    CE_ZOTERO_APPROXIMATE_IEEE,
    CE_ZOTERO_APPROXIMATE_GBT_7714
} CeZoteroApproximateStyle;
/* Deterministic plain-text fallback matching the previous app's fallback.
 * This is NOT a CSL processor or a claim of standards-compliant formatting.
 * Returns owned UTF-8 text (free with free()), or NULL on invalid input/OOM. */
char *ce_zotero_format_approximate(const CeZoteroItem *item, size_t number,
                                  CeZoteroApproximateStyle style);
#endif
