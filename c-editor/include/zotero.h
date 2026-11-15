#ifndef CE_ZOTERO_H
#define CE_ZOTERO_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct CeZoteroItem {
    int64_t database_id, library_id;
    char *id, *item_key, *title, *author, *year, *cite_key;
    char *item_uri, *pdf_uri;
    char *csl_json; /* Original CSL object, or mapped SQLite metadata with raw
                    * fields retained in custom.zotero.fields. */
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
    /* Optional owned client settings, a strict JSON object of at most 16 KiB.
     * Kept verbatim in the local cache alongside this exact library snapshot;
     * never included in CSL exports and never interpreted by this module. */
    char *client_state_json;
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
/* Imports a complete SQLite snapshot selected in a browser/document provider.
 * Copies at most 128 MiB into a read-only in-memory database. No temporary file
 * or source modification. WAL-mode files are rejected: their separate journal
 * may contain newer data. Use the native path API for a live Zotero database,
 * or export CSL JSON / a standalone SQLite backup for portable import.
 * Failure preserves the previous library. The input remains caller-owned. */
bool ce_zotero_load_sqlite_bytes(const void *data, size_t size,
                                 CeZoteroLibrary *library,
                                 char *error, size_t error_capacity);
void ce_zotero_library_free(CeZoteroLibrary *library);

/* Versioned application cache, separate from CSL exchange files. Preserves
 * item metadata, exact 64-bit IDs and collection relationships. Caller owns
 * the exported UTF-8 string (free()). Import is atomic and rebuilds the safe
 * collection tree. This codec never reads or writes filesystem paths. */
char *ce_zotero_cache_export(const CeZoteroLibrary *library,
                             char *error, size_t error_capacity);
bool ce_zotero_cache_import(const char *json, CeZoteroLibrary *library,
                             char *error, size_t error_capacity);

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
