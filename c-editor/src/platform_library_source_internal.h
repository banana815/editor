#ifndef PLATFORM_LIBRARY_SOURCE_INTERNAL_H
#define PLATFORM_LIBRARY_SOURCE_INTERNAL_H
#include "platform_library_source.h"

/* Transport owns one reference from successful start until backend_done. No
 * backend may call application code or interpret the downloaded metadata. The
 * context is set once before callbacks/threads start, and destroyed only after
 * both user ownership and backend ownership have ended. All byte/status helpers
 * serialize access and enforce the common deadline/cancellation/size guards. */
typedef struct PlatformLibrarySourceTransport {
    bool (*start)(PlatformLibrarySource *job, void *userdata);
    void (*cancel)(PlatformLibrarySource *job);
    void (*destroy)(PlatformLibrarySource *job);
} PlatformLibrarySourceTransport;
extern const PlatformLibrarySourceTransport platform_library_source_transport;

const char *platform_library_source_url(PlatformLibrarySource *job);
size_t platform_library_source_byte_limit(PlatformLibrarySource *job);
uint32_t platform_library_source_timeout(PlatformLibrarySource *job);
void platform_library_source_set_context(PlatformLibrarySource *job, void *context);
void *platform_library_source_context(PlatformLibrarySource *job);
bool platform_library_source_active(PlatformLibrarySource *job);
/* A final (non-informational) HTTP status must precede bytes. False means stop.
 * Redirect/non-2xx responses fail without returning their bodies as library data. */
bool platform_library_source_response(PlatformLibrarySource *job, unsigned status);
bool platform_library_source_append(PlatformLibrarySource *job, const void *bytes, size_t size);
void platform_library_source_success(PlatformLibrarySource *job);
void platform_library_source_error(PlatformLibrarySource *job, const char *message);
/* Call exactly once after every successful start, even after cancel/error. No
 * further job/context access is permitted after this may release the last ref. */
void platform_library_source_backend_done(PlatformLibrarySource *job);

/* Per-job test seams; never change a process-global allocator or clock. The
 * allocation hook only permits/rejects real module malloc allocations. Caller
 * context and transport vtable must outlive backend_done. Production uses NULL. */
typedef struct {
    uint64_t (*clock_ms)(void *context);
    bool (*allow_allocation)(void *context, size_t size);
    void *context;
} PlatformLibrarySourceTestHooks;
PlatformLibrarySource *platform_library_source_with_transport(const char *url,
    const PlatformLibrarySourceOptions *options, const PlatformLibrarySourceTransport *transport,
    void *transport_userdata, const PlatformLibrarySourceTestHooks *hooks,
    char *error, size_t capacity);
#endif
