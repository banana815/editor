#include "platform_persistence.h"
#include <stddef.h>

#ifdef __EMSCRIPTEN__

#include <emscripten.h>
#include <stdlib.h>

typedef struct PersistRequest {
    PlatformPersistCallback callback;
    void *userdata;
    struct PersistRequest *next;
} PersistRequest;

static PersistRequest *queued_head;
static PersistRequest *queued_tail;
static PersistRequest *active_head;
static bool flushing;
static bool delivering_callbacks;
static char flush_error[768];

EM_JS(void, platform_storage_js_pending, (int pending), {
    Module["editorPersistencePending"] = !!pending;
});

EM_JS(void, platform_storage_js_begin, (char *message, int capacity), {
    var finished = false;
    function finish(error) {
        if (finished) return;
        finished = true;
        var explanation = "";
        if (error) {
            var detail = error;
            if (error.target && error.target.error) detail = error.target.error;
            var description = detail.message || detail.name || String(detail);
            explanation = "Browser storage could not be saved: " + description +
                ". Keep this page open and retry, or export your note.";
        }
        // Even synchronous errors are delivered outside the request call. This
        // prevents recursive callback/retry chains from exhausting the C stack.
        Promise.resolve().then(function() {
            var bytes = new TextEncoder().encode(explanation);
            var length = Math.min(bytes.length, capacity - 1);
            // Preserve complete UTF-8 characters when truncating a long error.
            while (length > 0 && length < bytes.length &&
                   (bytes[length] & 0xc0) === 0x80) length--;
            HEAPU8.set(bytes.subarray(0, length), message);
            HEAPU8[message + length] = 0;
            _platform_persistence_complete(error ? 0 : 1);
        });
    }
    try {
        if (typeof FS === "undefined" || typeof FS.syncfs !== "function") {
            throw new Error("Persistent filesystem is not available");
        }
        FS.syncfs(false, finish);
    } catch (error) {
        finish(error);
    }
});

bool platform_persist_pending(void)
{
    return flushing || delivering_callbacks || queued_head != NULL;
}

static void start_next_flush(void)
{
    if (flushing || delivering_callbacks || queued_head == NULL) return;
    active_head = queued_head;
    queued_head = NULL;
    queued_tail = NULL;
    flushing = true;
    flush_error[0] = '\0';
    platform_storage_js_pending(1);
    platform_storage_js_begin(flush_error, (int)sizeof(flush_error));
}

/* Retained and exported by Emscripten; no EXPORTED_FUNCTIONS change is needed.
 * Only the JS completion wrapper above invokes this function. */
EMSCRIPTEN_KEEPALIVE void platform_persistence_complete(int success)
{
    PersistRequest *request;
    const char *message;
    if (!flushing) return;

    request = active_head;
    active_head = NULL;
    flushing = false;
    delivering_callbacks = true;
    message = success ? "" : (flush_error[0] ? flush_error :
        "Browser storage could not be saved. Keep this page open and retry, "
        "or export your note.");

    while (request != NULL) {
        PersistRequest *next = request->next;
        PlatformPersistCallback callback = request->callback;
        void *userdata = request->userdata;
        free(request);
        if (callback != NULL) callback(success != 0, message, userdata);
        request = next;
    }
    delivering_callbacks = false;

    /* Reentrant requests made by callbacks join the next batch too. An error
     * affects only its batch; subsequent writes still get their own attempt. */
    start_next_flush();
    platform_storage_js_pending(platform_persist_pending() ? 1 : 0);
}

void platform_persist(PlatformPersistCallback callback, void *userdata)
{
    PersistRequest *request = malloc(sizeof(*request));
    if (request == NULL) {
        if (callback != NULL) {
            callback(false, "Not enough memory to queue browser storage save. "
                     "Keep this page open and retry, or export your note.", userdata);
        }
        return;
    }
    request->callback = callback;
    request->userdata = userdata;
    request->next = NULL;
    if (queued_tail != NULL) queued_tail->next = request;
    else queued_head = request;
    queued_tail = request;
    platform_storage_js_pending(1);
    start_next_flush();
}

#else

void platform_persist(PlatformPersistCallback callback, void *userdata)
{
    if (callback != NULL) callback(true, "", userdata);
}

bool platform_persist_pending(void)
{
    return false;
}

#endif
