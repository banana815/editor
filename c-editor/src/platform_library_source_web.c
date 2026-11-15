#include "platform_library_source_internal.h"
#include <emscripten.h>
#include <stdlib.h>

/* The application need not export libc malloc/free to JavaScript. Keep this
 * backend's matched allocation pair available in optimized standalone builds. */
EMSCRIPTEN_KEEPALIVE void *ce_library_web_allocate(size_t size) { return malloc(size); }
EMSCRIPTEN_KEEPALIVE void ce_library_web_release(void *memory) { free(memory); }

EMSCRIPTEN_KEEPALIVE int ce_library_web_response(PlatformLibrarySource *job, unsigned status) {
    return platform_library_source_response(job, status);
}
EMSCRIPTEN_KEEPALIVE int ce_library_web_bytes(PlatformLibrarySource *job, const void *bytes, size_t size) {
    return platform_library_source_append(job, bytes, size);
}
EMSCRIPTEN_KEEPALIVE void ce_library_web_success(PlatformLibrarySource *job) {
    platform_library_source_success(job);
}
EMSCRIPTEN_KEEPALIVE void ce_library_web_error(PlatformLibrarySource *job, int reason) {
    platform_library_source_error(job, reason == 1 ? "Library download exceeded its byte limit" :
        reason == 2 ? "Library download timed out" : reason == 3 ? "Cannot allocate library download buffer" :
        "Library request failed: network, CORS, or local-network permission denied; use file import if this server blocks browser access");
}
EMSCRIPTEN_KEEPALIVE void ce_library_web_done(PlatformLibrarySource *job) {
    platform_library_source_backend_done(job);
}
EM_JS(int, library_web_start, (PlatformLibrarySource *job, const char *url, size_t limit, unsigned timeout), {
    if (typeof fetch !== 'function' || typeof AbortController !== 'function') return 0;
    let tasks, controller, task, address;
    try {
        tasks = Module['librarySourceTasks'] || (Module['librarySourceTasks'] = new Map());
        controller = new AbortController();
        task = {controller, timer: 0, stopped: false};
        address = UTF8ToString(url);
        tasks.set(job, task);
        task.timer = setTimeout(() => stop(2), timeout);
    } catch (_) {
        if (tasks) tasks.delete(job);
        if (task && task.timer) clearTimeout(task.timer);
        return 0; // No fetch/callback has started; C releases the backend ref.
    }
    function stop(reason) {
        if (task.stopped) return;
        task.stopped = true;
        _ce_library_web_error(job, reason);
        controller.abort();
    }
    // The C backend reference remains alive through finally, including abort
    // races. No callback can access job after ce_library_web_done.
    (async function() {
        let reader = null;
        try {
            const response = await fetch(address, {method: 'GET', credentials: 'omit',
                redirect: 'error', cache: 'no-store', referrerPolicy: 'no-referrer',
                headers: {'Accept': 'application/json, text/plain;q=0.9'}, signal: controller.signal});
            if (task.stopped || !_ce_library_web_response(job, response.status)) return;
            if (!response.body || typeof response.body.getReader !== 'function') {
                if (response.status === 204 || response.status === 205) _ce_library_web_success(job);
                else stop(0);
                return;
            }
            reader = response.body.getReader();
            let total = 0;
            while (!task.stopped) {
                const chunk = await reader.read();
                if (task.stopped) break;
                if (chunk.done) { _ce_library_web_success(job); break; }
                const bytes = chunk.value;
                if (!(bytes instanceof Uint8Array) || bytes.byteLength > limit - total) { stop(1); break; }
                if (!bytes.byteLength) continue;
                const p = _ce_library_web_allocate(bytes.byteLength);
                if (!p) { stop(3); break; }
                let accepted = false;
                try { HEAPU8.set(bytes, p); accepted = !!_ce_library_web_bytes(job, p, bytes.byteLength); }
                finally { _ce_library_web_release(p); }
                if (!accepted) { task.stopped = true; controller.abort(); break; }
                total += bytes.byteLength;
            }
        } catch (_) {
            if (!task.stopped) stop(0);
        } finally {
            task.stopped = true;
            clearTimeout(task.timer);
            // Cancel any unread body even for an HTTP error. Do not await a
            // hostile stream's cancel promise before releasing the C job.
            if (reader) { try { Promise.resolve(reader.cancel()).catch(() => {}); } catch (_) {} }
            controller.abort();
            tasks.delete(job);
            _ce_library_web_done(job);
        }
    })();
    return 1;
})
EM_JS(void, library_web_cancel, (PlatformLibrarySource *job), {
    const tasks = Module['librarySourceTasks'];
    const task = tasks && tasks.get(job);
    if (task && !task.stopped) { task.stopped = true; task.controller.abort(); }
})
static bool web_start(PlatformLibrarySource *job, void *userdata) {
    (void)userdata;
    return library_web_start(job, platform_library_source_url(job),
        platform_library_source_byte_limit(job), platform_library_source_timeout(job)) != 0;
}
static void web_cancel(PlatformLibrarySource *job) { library_web_cancel(job); }
static void web_destroy(PlatformLibrarySource *job) { (void)job; }
const PlatformLibrarySourceTransport platform_library_source_transport = {web_start, web_cancel, web_destroy};
