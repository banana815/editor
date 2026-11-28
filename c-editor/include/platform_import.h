#ifndef PLATFORM_IMPORT_H
#define PLATFORM_IMPORT_H
#include <SDL3/SDL.h>
#include <stddef.h>

#define PLATFORM_IMPORT_MAX_BYTES ((size_t)128 * 1024 * 1024)
typedef struct PlatformImport PlatformImport;
typedef enum {
    PLATFORM_IMPORT_JSON, PLATFORM_IMPORT_SQLITE, PLATFORM_IMPORT_ZOTERO,
    PLATFORM_IMPORT_DOCUMENT, PLATFORM_IMPORT_IMAGE, PLATFORM_IMPORT_BACKUP
} PlatformImportKind;
typedef enum {
    PLATFORM_IMPORT_PENDING, PLATFORM_IMPORT_DONE,
    PLATFORM_IMPORT_CANCELED, PLATFORM_IMPORT_FAILED
} PlatformImportStatus;
typedef struct {
    PlatformImportKind kind; /* Actual JSON, SQLITE or DOCUMENT; never ZOTERO. */
    char *filename;          /* UTF-8 display name, not a storage destination. */
    char *native_path;       /* Desktop SQLite: preserve the original WAL view. */
    unsigned char *data;     /* Otherwise owned bytes; data[size] is a sentinel 0. */
    size_t size;
} PlatformImportFile;

/* Start/poll/take/free on the main thread. File access starts only after the
 * user selects a file. A pending job must remain alive for its dialog callback. */
PlatformImport *platform_import_open(SDL_Window *window, PlatformImportKind kind);
PlatformImportStatus platform_import_status(PlatformImport *job, const char **message);
/* Transfer the result once after DONE; out must not already own any memory. */
bool platform_import_take_result(PlatformImport *job, PlatformImportFile *out);
void platform_import_file_free(PlatformImportFile *file);
void platform_import_free(PlatformImport *job);

/* Native picker bridge. Exactly one completion per job, possibly on a worker
 * thread. UTF-8 strings and bytes are copied before these functions return.
 * After completion do not access job again. Bytes may contain NUL and are
 * limited to PLATFORM_IMPORT_MAX_BYTES. The bridge can then release its file
 * coordinator / security-scoped resource. */
void platform_import_complete_bytes(PlatformImport *job, const char *filename,
                                    const void *bytes, size_t size);
void platform_import_complete_cancel(PlatformImport *job);
void platform_import_complete_error(PlatformImport *job, const char *message);
#ifdef SDL_PLATFORM_IOS
void platform_import_ios_begin(SDL_Window *window, PlatformImport *job,
                               PlatformImportKind kind);
#endif
#endif
