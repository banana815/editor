#ifndef NOTELAB_PLATFORM_PERSISTENCE_H
#define NOTELAB_PLATFORM_PERSISTENCE_H

#include <stdbool.h>

/* Call from the application/main thread. The message is never NULL, is empty
 * on success, and is valid only for the duration of the callback. */
typedef void (*PlatformPersistCallback)(bool success, const char *message,
                                       void *userdata);

/* Persist changes already written by note_store. Native files are durable when
 * note_store returns, so native callbacks run immediately. Browser callbacks
 * run after the relevant IDBFS flush finishes. Requests made during an active
 * flush wait for a subsequent flush, rather than borrowing its result.
 * A NULL callback is allowed. Each non-NULL callback is invoked exactly once. */
void platform_persist(PlatformPersistCallback callback, void *userdata);

/* True while a browser flush or a queued request is outstanding, including
 * callback delivery. Browser glue also mirrors this as the boolean
 * Module.editorPersistencePending for beforeunload/visibility handlers. */
bool platform_persist_pending(void);

#endif
