#include "platform_persistence.h"

#include <assert.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#ifdef __EMSCRIPTEN__

/* Host-side driver for the real C queue implementation, compiled using the
 * tests/support/emscripten_stub header. Real browser/IDBFS tests remain needed.
 * Compile this file together with src/platform_persistence.c, not editor_core,
 * so that both translation units see the same __EMSCRIPTEN__ definition. */
void platform_persistence_complete(int success);

typedef struct RequestCase {
    int id;
    int calls;
    bool success;
    char message[1024];
    struct RequestCase *reenter;
} RequestCase;

static int flushes_started;
static bool driver_inflight;
static bool js_pending;
static char *driver_error_buffer;
static int driver_error_capacity;
static int callback_order[16];
static size_t callback_count;

void platform_storage_js_pending(int pending)
{
    js_pending = pending != 0;
}

void platform_storage_js_begin(char *message, int capacity)
{
    /* A second sync must never start before the first has completed. */
    assert(!driver_inflight);
    assert(platform_persist_pending());
    assert(js_pending);
    assert(message != NULL && capacity > 0);
    assert(message[0] == '\0');
    driver_inflight = true;
    driver_error_buffer = message;
    driver_error_capacity = capacity;
    flushes_started++;
}

static void request_completed(bool success, const char *message, void *userdata)
{
    RequestCase *request = userdata;
    assert(request != NULL);
    assert(message != NULL);
    assert(platform_persist_pending());
    assert(js_pending);
    assert(request->calls == 0);
    request->calls++;
    request->success = success;
    snprintf(request->message, sizeof(request->message), "%s", message);
    assert(callback_count < sizeof(callback_order) / sizeof(callback_order[0]));
    callback_order[callback_count++] = request->id;
    if (success) assert(message[0] == '\0');
    else assert(message[0] != '\0');

    if (request->reenter != NULL) {
        int before = flushes_started;
        platform_persist(request_completed, request->reenter);
        /* Requests made during delivery must not interrupt the current batch. */
        assert(flushes_started == before);
    }
}

static void finish_flush(bool success, const char *message)
{
    assert(driver_inflight);
    if (message != NULL) {
        snprintf(driver_error_buffer, (size_t)driver_error_capacity, "%s", message);
    }
    driver_inflight = false;
    platform_persistence_complete(success ? 1 : 0);
}

int main(void)
{
    RequestCase requests[8] = {0};
    for (int i = 0; i < 8; i++) requests[i].id = i;
    requests[0].reenter = &requests[3];

    assert(!platform_persist_pending());
    platform_persist(request_completed, &requests[0]);
    platform_persist(request_completed, &requests[1]);
    platform_persist(request_completed, &requests[2]);
    assert(flushes_started == 1 && driver_inflight && js_pending);
    assert(callback_count == 0);

    /* Only request 0 belongs to the first snapshot. The other two requests,
     * plus request 3 enqueued by callback 0, belong to the next flush. */
    finish_flush(true, NULL);
    assert(requests[0].calls == 1 && requests[0].success);
    assert(requests[1].calls == 0 && requests[2].calls == 0);
    assert(requests[3].calls == 0);
    assert(flushes_started == 2 && driver_inflight && js_pending);

    platform_persist(request_completed, &requests[4]);
    platform_persist(NULL, NULL);
    finish_flush(false, "Browser storage quota exceeded");
    for (int i = 1; i <= 3; i++) {
        assert(requests[i].calls == 1 && !requests[i].success);
        assert(strcmp(requests[i].message, "Browser storage quota exceeded") == 0);
    }
    assert(requests[4].calls == 0);
    assert(flushes_started == 3 && driver_inflight && js_pending);

    /* Failure does not discard the next queued snapshot. */
    finish_flush(true, NULL);
    assert(requests[4].calls == 1 && requests[4].success);
    assert(!driver_inflight && !js_pending && !platform_persist_pending());
    assert(callback_count == 5);
    for (int i = 0; i < 5; i++) assert(callback_order[i] == i);

    /* A stray completion while idle must not re-deliver any callbacks. */
    platform_persistence_complete(1);
    assert(callback_count == 5);

    /* Reentrant enqueue also works when no other queued request exists. */
    requests[5].reenter = &requests[6];
    platform_persist(request_completed, &requests[5]);
    assert(flushes_started == 4);
    finish_flush(true, NULL);
    assert(requests[5].calls == 1 && requests[6].calls == 0);
    assert(flushes_started == 5 && driver_inflight && js_pending);
    finish_flush(false, NULL);
    assert(requests[6].calls == 1 && !requests[6].success);
    assert(strstr(requests[6].message, "Browser storage could not be saved") != NULL);
    assert(!js_pending && !platform_persist_pending());

    platform_persist(NULL, NULL);
    assert(flushes_started == 6 && driver_inflight && js_pending);
    finish_flush(true, NULL);
    assert(!js_pending && !platform_persist_pending());
    assert(callback_count == 7);
    for (int i = 0; i < 7; i++) {
        assert(requests[i].calls == 1);
        assert(callback_order[i] == i);
    }
    puts("persistence queue driver: batching, reentrancy, failures, and callbacks passed");
    return 0;
}

#else

static int native_calls;

static void native_completed(bool success, const char *message, void *userdata)
{
    assert(success);
    assert(message != NULL && message[0] == '\0');
    assert(userdata == &native_calls);
    assert(!platform_persist_pending());
    native_calls++;
}

int main(void)
{
    assert(!platform_persist_pending());
    platform_persist(native_completed, &native_calls);
    assert(native_calls == 1);
    platform_persist(NULL, NULL);
    assert(native_calls == 1);
    assert(!platform_persist_pending());
    puts("native persistence: immediate completion passed");
    return 0;
}

#endif
