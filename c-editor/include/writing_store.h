#ifndef CE_WRITING_STORE_H
#define CE_WRITING_STORE_H
#include "note_store.h"
#include <stdbool.h>
#include <stdint.h>

typedef struct {
    char folder[256], tags[512];
    unsigned status; /* 0 draft, 1 revision, 2 published */
    bool pinned;
    size_t goal;
} CeArticleInfo;
typedef struct {
    NoteStore catalog_store, versions, trash;
    void *catalog;
    unsigned sequence;
    bool ready;
    char error[256];
} CeWritingStore;
typedef struct { char *id, *name; int64_t timestamp; } CeArchiveEntry;
typedef struct { CeArchiveEntry *entries; size_t count; } CeArchiveList;

/* Owned sidecars only, rooted in the explicitly supplied notes directory.
 * Catalog updates clone, validate and atomically save before swapping state.
 * Catalog JSON is bounded at 16 MiB; recovery envelopes allow worst-case JSON
 * escaping of a 16 MiB article plus catalog metadata (113 MiB encoded maximum).
 * Unknown catalog/article fields survive known-field updates. */
bool ce_writing_store_open(CeWritingStore *store,const char *notes_root);
void ce_writing_store_close(CeWritingStore *store);
bool ce_writing_article_get(CeWritingStore *store,const char *name,CeArticleInfo *out);
bool ce_writing_article_set(CeWritingStore *store,const char *name,const CeArticleInfo *info);
bool ce_writing_folder_add(CeWritingStore *store,const char *folder);
size_t ce_writing_folder_count(const CeWritingStore *store);
const char *ce_writing_folder_at(const CeWritingStore *store,size_t index);
bool ce_writing_archive_list(CeWritingStore *store,bool trash,const char *name,CeArchiveList *out);
void ce_writing_archive_list_free(CeArchiveList *list);
/* Caller gets exact original bytes and name; preview/restore must validate the
 * document before mutation. History is deduplicated, never pruned silently. */
bool ce_writing_archive_read(CeWritingStore *store,bool trash,const char *id,char **name,char **data);
bool ce_writing_snapshot(CeWritingStore *store,const char *name,const char *data,int64_t now);
/* Archive is written first. Failed deletion leaves original and recovery copy.
 * Restore never overwrites an existing note; caller chooses a new valid name.
 * Metadata is prepared before creating the note. Catalog write failure removes
 * the just-created note; if that removal also fails, error explicitly reports
 * a partial restore. The recovery record is retained in all cases. */
bool ce_writing_trash_note(CeWritingStore *store,NoteStore *notes,const char *name,int64_t now);
bool ce_writing_trash_restore(CeWritingStore *store,NoteStore *notes,const char *id,const char *new_name);
#endif
