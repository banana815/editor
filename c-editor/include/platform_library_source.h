#ifndef PLATFORM_LIBRARY_SOURCE_H
#define PLATFORM_LIBRARY_SOURCE_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define PLATFORM_LIBRARY_SOURCE_MAX_BYTES ((size_t)128 * 1024 * 1024)
#define PLATFORM_LIBRARY_SOURCE_MAX_URL ((size_t)8192)
#define PLATFORM_LIBRARY_SOURCE_DEFAULT_TIMEOUT_MS UINT32_C(30000)
#define PLATFORM_LIBRARY_SOURCE_MAX_TIMEOUT_MS UINT32_C(120000)

typedef struct PlatformLibrarySource PlatformLibrarySource;
typedef enum {
    PLATFORM_LIBRARY_SOURCE_PENDING, PLATFORM_LIBRARY_SOURCE_DONE,
    PLATFORM_LIBRARY_SOURCE_CANCELED, PLATFORM_LIBRARY_SOURCE_FAILED
} PlatformLibrarySourceStatus;
typedef struct {
    size_t max_bytes;      /* Zero selects 128 MiB; may only lower that limit. */
    uint32_t timeout_ms;   /* Zero selects 30 s; maximum 120 s. */
} PlatformLibrarySourceOptions;
typedef struct {
    unsigned char *data;   /* Owned HTTP bytes; data[size] is a sentinel zero. */
    size_t size;          /* Byte length, never inferred from strlen. */
    char *native_path;    /* Desktop SQLite reopen: let the readonly WAL loader read it. */
    unsigned http_status;
} PlatformLibrarySourceResult;

/* Starts only when explicitly called by the user's Get/Refresh action. This
 * transport never parses metadata, changes a library, or persists a source.
 * URL is an ASCII HTTP(S) URL (percent-encode non-ASCII path/query bytes), without
 * credentials, fragments or control characters. HTTP is limited to canonical
 * loopback/private/link-local IPs and localhost/.localhost/.local hostnames;
 * HTTPS accepts ordinary hosts. Redirects, cookies and HTTP authentication are
 * disabled; certificate verification is never disabled. Browser CORS/local
 * network restrictions still apply, including Zotero's refusal of browser BBT
 * pull exports. NULL means invalid arguments or failure to allocate a job.
 * At most eight live jobs (including canceled transports still draining) may
 * exist. Start/status/take/cancel/free must be called on the main thread. */
PlatformLibrarySource *platform_library_source_fetch(const char *url,
    const PlatformLibrarySourceOptions *options, char *error, size_t capacity);
/* Desktop only, for a native path previously selected by the user. This does
 * not read SQLite bytes or bypass WAL: it returns a copied absolute regular-file
 * path. Mobile security-scoped/content-provider grants are not fabricated. */
PlatformLibrarySource *platform_library_source_reopen_sqlite(const char *native_path,
    char *error, size_t capacity);
PlatformLibrarySourceStatus platform_library_source_status(PlatformLibrarySource *job,
    const char **message);
/* Transfer the result once after DONE. out must not already own memory. */
bool platform_library_source_take_result(PlatformLibrarySource *job,
    PlatformLibrarySourceResult *out);
void platform_library_source_result_free(PlatformLibrarySourceResult *result);
/* Cancellation immediately becomes terminal and discards partial bytes. Free
 * cancels pending work; backends retain their own reference until any late OS
 * completion has drained. Neither operation invokes application callbacks. */
void platform_library_source_cancel(PlatformLibrarySource *job);
void platform_library_source_free(PlatformLibrarySource *job);
#endif
