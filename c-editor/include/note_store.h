#ifndef NOTE_STORE_H
#define NOTE_STORE_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum NoteResult {
    NOTE_OK = 0,
    NOTE_INVALID_NAME,
    NOTE_EXISTS,
    NOTE_NOT_FOUND,
    NOTE_IO_ERROR,
    NOTE_NO_MEMORY,
    NOTE_UNSUPPORTED
} NoteResult;

typedef struct NoteStore {
    void *internal;
    char error[256];
} NoteStore;

typedef struct NoteList {
    char **names;
    size_t count;
} NoteList;

/* Zero-initialize before init; destroy before reinitializing. The caller chooses
 * an application-owned or explicitly selected UTF-8 root. No home directory is
 * accessed implicitly. Calls on one store must be serialized by the caller. */
NoteResult note_store_init(NoteStore *store, const char *root);
void note_store_destroy(NoteStore *store);

/* Flat, portable UTF-8 names only; .md/.txt/.json extensions are case-insensitive.
 * Control characters, path separators and Windows device names are rejected. */
int note_store_valid_name(const char *name);
NoteResult note_store_normalize_name(const char *input, char **name);

NoteResult note_store_list(NoteStore *store, NoteList *list);
void note_store_list_free(NoteList *list);

/* Returned bytes are NUL-terminated, but size excludes the extra terminator.
 * Content is not transformed or interpreted. Embedded NULs are preserved.
 * Release returned strings/data with note_store_free(). */
NoteResult note_store_read(NoteStore *store, const char *name,
                           char **data, size_t *size);
NoteResult note_store_create(NoteStore *store, const char *name,
                             const void *data, size_t size);
/* Save replaces an existing regular file atomically; it will not create a
 * missing note. A failed write never truncates the previous file. */
NoteResult note_store_save(NoteStore *store, const char *name,
                           const void *data, size_t size);
/* Explicitly selected publication output only. Same portable flat-name and
 * regular-file checks, adding .html/.zip to the note extensions. Create exclusively
 * or atomically replace an existing file; a failed write never truncates an
 * existing destination. This does not add HTML to note listing/read/create/save
 * or normalization contracts. No implicit output location is selected. */
int note_store_valid_export_name(const char *name);
/* Private managed assets: SHA-256 lowercase names + .png/.jpg/.bmp only;
 * no listing as notes, no replacement. 16 MiB bounded reads/creates. */
int note_store_valid_asset_name(const char *name);
NoteResult note_store_read_asset(NoteStore *,const char *,char **,size_t *);
NoteResult note_store_create_asset(NoteStore *,const char *,const void *,size_t);
NoteResult note_store_export(NoteStore *store, const char *name,
                             const void *data, size_t size);
NoteResult note_store_delete(NoteStore *store, const char *name);
void note_store_free(void *allocation);

/* Native writes are flushed before returning. Emscripten's virtual filesystem
 * additionally needs platform-level IDBFS sync to persist across page reloads. */

#ifdef __cplusplus
}
#endif
#endif
