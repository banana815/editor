#include "platform_import.h"
#include <string.h>
#ifdef __ANDROID__
#include <jni.h>
#endif
#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#endif

struct PlatformImport {
    SDL_AtomicInt status;
    PlatformImportKind requested;
    PlatformImportFile file;
    char message[512];
};

static void completed(PlatformImport *job, PlatformImportStatus status, const char *message) {
    SDL_strlcpy(job->message, message && *message ? message : "无法读取所选文件", sizeof(job->message));
    /* Publish only after the result and message are complete. The callback
     * must not touch job after this store: the main thread may release it. */
    SDL_SetAtomicInt(&job->status, status);
}
void platform_import_complete_cancel(PlatformImport *job) {
    completed(job, PLATFORM_IMPORT_CANCELED, "已取消导入");
}
void platform_import_complete_error(PlatformImport *job, const char *message) {
    completed(job, PLATFORM_IMPORT_FAILED, message);
}
static bool is_sqlite(const void *bytes, size_t size) {
    return size >= 16 && memcmp(bytes, "SQLite format 3\0", 16) == 0;
}
/* Takes both allocations, including on failure. The data has one extra byte. */
static void adopt_bytes(PlatformImport *job, char *filename, unsigned char *data, size_t size) {
    if (size > PLATFORM_IMPORT_MAX_BYTES || !filename || !data) {
        SDL_free(filename); SDL_free(data);
        platform_import_complete_error(job, size > PLATFORM_IMPORT_MAX_BYTES
            ? "文件超过 128 MiB，请选择较小的导出文件" : "内存不足，无法读取文件");
        return;
    }
    bool sqlite = is_sqlite(data, size);
    if (job->requested == PLATFORM_IMPORT_SQLITE && !sqlite) {
        SDL_free(filename); SDL_free(data);
        platform_import_complete_error(job, "所选文件不是 SQLite 数据库");
        return;
    }
    data[size] = 0;
    job->file = (PlatformImportFile){
        .kind = job->requested == PLATFORM_IMPORT_ZOTERO
            ? (sqlite ? PLATFORM_IMPORT_SQLITE : PLATFORM_IMPORT_JSON) : job->requested,
        .filename = filename, .data = data, .size = size,
    };
    completed(job, PLATFORM_IMPORT_DONE, "文件已读取");
}
void platform_import_complete_bytes(PlatformImport *job, const char *filename,
                                    const void *bytes, size_t size) {
    if (size > PLATFORM_IMPORT_MAX_BYTES || (!bytes && size)) {
        platform_import_complete_error(job, size > PLATFORM_IMPORT_MAX_BYTES
            ? "文件超过 128 MiB，请选择较小的导出文件" : "无法读取所选文件");
        return;
    }
    char *name = SDL_strdup(filename && *filename ? filename : "所选文件");
    unsigned char *copy = SDL_malloc(size + 1);
    if (copy && size) memcpy(copy, bytes, size);
    adopt_bytes(job, name, copy, size);
}

#ifdef __EMSCRIPTEN__
EMSCRIPTEN_KEEPALIVE void *platform_import_web_alloc(size_t size) { return SDL_malloc(size); }
EMSCRIPTEN_KEEPALIVE void platform_import_web_free(void *pointer) { SDL_free(pointer); }
/* All pointers supplied here are owned, not borrowed from the JS callback. */
EMSCRIPTEN_KEEPALIVE void platform_import_web_finish(PlatformImport *job, int status,
                                                     char *name, unsigned char *data,
                                                     size_t size, char *message) {
    if (status == PLATFORM_IMPORT_DONE) {
        SDL_free(message);
        adopt_bytes(job, name, data, size);
        return;
    }
    SDL_free(name); SDL_free(data);
    char error[512];
    SDL_strlcpy(error, message ? message : "内存不足或无法打开文件选择器", sizeof(error));
    SDL_free(message);
    if (status == PLATFORM_IMPORT_CANCELED) platform_import_complete_cancel(job);
    else platform_import_complete_error(job, error);
}

EM_JS(void, platform_import_js_begin, (PlatformImport *job, int kind, size_t limit), {
    var input = null, reader = null, finished = false, reading = false, timer = null;
    function textPointer(text) {
        var bytes = new TextEncoder().encode(String(text).slice(0, 4096));
        var pointer = _platform_import_web_alloc(bytes.length + 1);
        if (!pointer) return 0;
        HEAPU8.set(bytes, pointer); HEAPU8[pointer + bytes.length] = 0;
        return pointer;
    }
    function cleanup() {
        if (timer !== null) clearTimeout(timer);
        window.removeEventListener('focus', regainedFocus);
        if (input) { input.onchange = null; input.oncancel = null; input.remove(); }
        if (reader) { reader.onload = null; reader.onerror = null; reader.onabort = null; }
    }
    function finish(status, filename, bytes, message) {
        if (finished) return;
        finished = true;
        cleanup();
        var name = 0, data = 0, error = 0, length = bytes ? bytes.byteLength : 0;
        try {
            if (status === 1) {
                if (length > limit) throw new Error('文件超过 128 MiB，请选择较小的导出文件');
                name = textPointer(filename);
                data = _platform_import_web_alloc(length + 1);
                if (!name || !data) throw new Error('内存不足，无法读取文件');
                // Both allocations may grow WASM memory; use the current view.
                HEAPU8.set(bytes, data); HEAPU8[data + length] = 0;
            } else if (message) error = textPointer(message);
        } catch (failure) {
            if (name) _platform_import_web_free(name);
            if (data) _platform_import_web_free(data);
            name = data = 0; length = 0; status = 3;
            try { error = textPointer(failure.message || '无法读取所选文件'); } catch (_) {}
        }
        _platform_import_web_finish(job, status, name, data, length, error);
    }
    function selected() {
        if (finished || reading) return;
        if (!input.files || !input.files.length) { finish(2); return; }
        var file = input.files[0];
        if (!Number.isSafeInteger(file.size) || file.size < 0 || file.size > limit) {
            finish(3, null, null, '文件超过 128 MiB，请选择较小的导出文件'); return;
        }
        reading = true;
        window.removeEventListener('focus', regainedFocus);
        if (timer !== null) { clearTimeout(timer); timer = null; }
        try {
            reader = new FileReader();
            reader.onload = function() {
                try {
                    if (!(reader.result instanceof ArrayBuffer) || reader.result.byteLength !== file.size)
                        throw new Error('文件读取不完整，请重新选择文件');
                    finish(1, file.name || '所选文件', new Uint8Array(reader.result));
                } catch (error) { finish(3, null, null, error.message || '无法读取所选文件'); }
            };
            reader.onerror = function() { finish(3, null, null, '无法读取所选文件，请检查文件是否仍可访问'); };
            reader.onabort = function() { finish(2); };
            reader.readAsArrayBuffer(file);
        } catch (error) { finish(3, null, null, error.message || '无法读取所选文件'); }
    }
    function regainedFocus() {
        // Older engines omit the input cancel event. Allow their change event
        // to run first; a file already selected is read rather than canceled.
        if (!finished && !reading) timer = setTimeout(selected, 350);
    }
    try {
        input = document.createElement('input');
        input.type = 'file'; input.multiple = false;
        input.setAttribute('data-editor-import', "");
        input.accept = kind === 4 ? '.png,.jpg,.jpeg,.bmp,image/png,image/jpeg,image/bmp'
            : kind === 5 ? '.zip,application/zip' : kind === 0 ? '.json,.csljson,application/json'
            : kind === 1 ? '.sqlite,.db,application/x-sqlite3'
            : kind === 3 ? '.json,.html,.htm,.md,.txt,application/json,text/html,text/plain'
            : '.json,.csljson,.sqlite,.db,application/json,application/x-sqlite3';
        input.style.display = 'none';
        input.onchange = selected;
        input.oncancel = function() { if (!reading) finish(2); };
        document.body.appendChild(input);
        window.addEventListener('focus', regainedFocus);
        input.click();
    } catch (error) { finish(3, null, null, error.message || '无法打开文件选择器'); }
})
#elif !defined(SDL_PLATFORM_IOS)
static const SDL_DialogFileFilter json_filters[] = {{"CSL JSON", "json;csljson"}};
static const SDL_DialogFileFilter sqlite_filters[] = {{"SQLite database", "sqlite;db"}};
static const SDL_DialogFileFilter zotero_filters[] = {
    {"Zotero CSL JSON / SQLite", "json;csljson;sqlite;db"}, {"All files", "*"}
};
static const SDL_DialogFileFilter image_filters[] = {{"Images", "png;jpg;jpeg;bmp"}};
static const SDL_DialogFileFilter backup_filters[] = {{"Workspace backup", "zip"}};
static const SDL_DialogFileFilter document_filters[] = {
    {"Notes (JSON / HTML / text)", "json;html;htm;md;txt"}, {"All files", "*"}
};
typedef struct { PlatformImport *job; char *path; } ReadJob;
#ifdef __ANDROID__
/* Document-provider URIs commonly end in an opaque numeric ID. Ask the
 * provider for its display name; do not present that ID as a filename. */
static char *android_display_name(const char *path, PlatformImportKind kind) {
    JNIEnv *env = SDL_GetAndroidJNIEnv();
    char *name = NULL;
    jobject cursor = NULL;
    jmethodID close_cursor = NULL;
    if (!env) goto fallback;
    if ((*env)->PushLocalFrame(env, 24) < 0) {
        (*env)->ExceptionClear(env); goto fallback;
    }
#define JNI_FAILED() ((*env)->ExceptionCheck(env))
    jobject activity = SDL_GetAndroidActivity();
    jclass uri_class = (*env)->FindClass(env, "android/net/Uri");
    if (!activity || !uri_class || JNI_FAILED()) goto cleanup;
    jmethodID parse = (*env)->GetStaticMethodID(env, uri_class, "parse", "(Ljava/lang/String;)Landroid/net/Uri;");
    jstring uri_text = (*env)->NewStringUTF(env, path);
    if (!parse || !uri_text || JNI_FAILED()) goto cleanup;
    jobject uri = (*env)->CallStaticObjectMethod(env, uri_class, parse, uri_text);
    jclass activity_class = (*env)->GetObjectClass(env, activity);
    if (!uri || !activity_class || JNI_FAILED()) goto cleanup;
    jmethodID get_resolver = (*env)->GetMethodID(env, activity_class, "getContentResolver", "()Landroid/content/ContentResolver;");
    if (!get_resolver || JNI_FAILED()) goto cleanup;
    jobject resolver = (*env)->CallObjectMethod(env, activity, get_resolver);
    jclass resolver_class = (*env)->FindClass(env, "android/content/ContentResolver");
    jclass string_class = (*env)->FindClass(env, "java/lang/String");
    jclass cursor_class = (*env)->FindClass(env, "android/database/Cursor");
    if (!resolver || !resolver_class || !string_class || !cursor_class || JNI_FAILED()) goto cleanup;
    close_cursor = (*env)->GetMethodID(env, cursor_class, "close", "()V");
    jmethodID query = (*env)->GetMethodID(env, resolver_class, "query", "(Landroid/net/Uri;[Ljava/lang/String;Ljava/lang/String;[Ljava/lang/String;Ljava/lang/String;)Landroid/database/Cursor;");
    jstring column = (*env)->NewStringUTF(env, "_display_name");
    if (!query || !close_cursor || !column || JNI_FAILED()) goto cleanup;
    jobjectArray projection = (*env)->NewObjectArray(env, 1, string_class, column);
    if (!projection || JNI_FAILED()) goto cleanup;
    cursor = (*env)->CallObjectMethod(env, resolver, query, uri, projection, (jobject)NULL, (jobject)NULL, (jobject)NULL);
    if (!cursor || JNI_FAILED()) goto cleanup;
    jmethodID first = (*env)->GetMethodID(env, cursor_class, "moveToFirst", "()Z");
    jmethodID column_index = (*env)->GetMethodID(env, cursor_class, "getColumnIndex", "(Ljava/lang/String;)I");
    jmethodID get_string = (*env)->GetMethodID(env, cursor_class, "getString", "(I)Ljava/lang/String;");
    if (!first || !column_index || !get_string || JNI_FAILED()) goto cleanup;
    if (!(*env)->CallBooleanMethod(env, cursor, first) || JNI_FAILED()) goto cleanup;
    jint index = (*env)->CallIntMethod(env, cursor, column_index, column);
    if (index < 0 || JNI_FAILED()) goto cleanup;
    jstring value = (*env)->CallObjectMethod(env, cursor, get_string, index);
    if (!value || JNI_FAILED()) goto cleanup;
    /* GetStringUTFChars uses modified UTF-8, which corrupts emoji. Use the
     * platform UTF-8 encoder to preserve supplementary code points. */
    jmethodID get_bytes = (*env)->GetMethodID(env, string_class, "getBytes", "(Ljava/lang/String;)[B");
    jstring utf8 = (*env)->NewStringUTF(env, "UTF-8");
    if (!get_bytes || !utf8 || JNI_FAILED()) goto cleanup;
    jbyteArray bytes = (*env)->CallObjectMethod(env, value, get_bytes, utf8);
    if (!bytes || JNI_FAILED()) goto cleanup;
    jsize size = (*env)->GetArrayLength(env, bytes);
    if (size <= 0 || size > 16384 || JNI_FAILED()) goto cleanup;
    name = SDL_malloc((size_t)size + 1);
    if (!name) goto cleanup;
    (*env)->GetByteArrayRegion(env, bytes, 0, size, (jbyte *)name);
    if (JNI_FAILED() || memchr(name, 0, (size_t)size)) { SDL_free(name); name = NULL; }
    else name[size] = 0;
cleanup:
    if (JNI_FAILED()) (*env)->ExceptionClear(env);
    if (cursor && close_cursor) (*env)->CallVoidMethod(env, cursor, close_cursor);
    if (JNI_FAILED()) (*env)->ExceptionClear(env);
    (*env)->PopLocalFrame(env, NULL);
#undef JNI_FAILED
fallback:
    return name ? name : SDL_strdup(kind == PLATFORM_IMPORT_SQLITE ? "所选数据库.sqlite" : "所选文件");
}
#else
static const char *basename_utf8(const char *path) {
    const char *slash = strrchr(path, '/');
#ifdef _WIN32
    const char *backslash = strrchr(path, '\\');
    if (backslash && (!slash || backslash > slash)) slash = backslash;
#endif
    return slash && slash[1] ? slash + 1 : path;
}
#endif
/* Owns and closes stream on every path. Reported sizes are only a hint: a
 * provider may not know its length, and a local file may grow during reading. */
static unsigned char *read_bounded(SDL_IOStream *stream, const void *prefix,
                                   size_t prefix_size, size_t limit, size_t *size,
                                   char *error, size_t error_capacity) {
    unsigned char *data = NULL;
    size_t used = prefix_size, capacity = SDL_min(limit, (size_t)65536) + 1;
    *size = 0;
    Sint64 reported = SDL_GetIOSize(stream);
    if (prefix_size > limit || (reported >= 0 && (Uint64)reported > limit)) {
        SDL_strlcpy(error, "文件超过 128 MiB，请选择较小的导出文件", error_capacity); goto done;
    }
    if (capacity <= prefix_size) capacity = prefix_size + 1;
    data = SDL_malloc(capacity);
    if (!data) { SDL_strlcpy(error, "内存不足，无法读取文件", error_capacity); goto done; }
    if (prefix_size) memcpy(data, prefix, prefix_size);
    for (;;) {
        unsigned char extra;
        bool probing = used == limit;
        if (!probing && used == capacity - 1) {
            size_t grown = SDL_min(limit + 1, capacity * 2);
            unsigned char *next = SDL_realloc(data, grown);
            if (!next) { SDL_strlcpy(error, "内存不足，无法读取文件", error_capacity); goto failed; }
            data = next; capacity = grown;
        }
        size_t requested = probing ? 1 : SDL_min(capacity - used - 1, (size_t)65536);
        SDL_ClearError();
        size_t count = SDL_ReadIO(stream, probing ? &extra : data + used, requested);
        SDL_IOStatus status = SDL_GetIOStatus(stream);
        if (probing && count) {
            SDL_strlcpy(error, "文件超过 128 MiB，请选择较小的导出文件", error_capacity); goto failed;
        }
        used += count;
        if (status == SDL_IO_STATUS_EOF) break;
        if (status != SDL_IO_STATUS_READY || !count) {
            SDL_strlcpy(error, *SDL_GetError() ? SDL_GetError() : "读取文件失败，请重新选择文件", error_capacity);
            goto failed;
        }
    }
    data[used] = 0; *size = used;
    goto done;
failed:
    SDL_free(data); data = NULL;
done:
    if (!SDL_CloseIO(stream) && data) {
        SDL_strlcpy(error, *SDL_GetError() ? SDL_GetError() : "无法关闭所选文件", error_capacity);
        SDL_free(data); data = NULL; *size = 0;
    }
    return data;
}
static int SDLCALL read_selected(void *userdata) {
    ReadJob *read = userdata;
    PlatformImport *job = read->job;
    char *path = read->path;
    SDL_free(read);
#ifndef __ANDROID__
    SDL_PathInfo info;
    if (!SDL_GetPathInfo(path, &info)) {
        SDL_free(path); platform_import_complete_error(job, SDL_GetError()); return 0;
    }
    if (info.type != SDL_PATHTYPE_FILE) {
        SDL_free(path); platform_import_complete_error(job, "请选择普通文件，不能导入目录或特殊设备"); return 0;
    }
#endif
    SDL_IOStream *stream = SDL_IOFromFile(path, "rb");
    if (!stream) {
        SDL_free(path); platform_import_complete_error(job, SDL_GetError()); return 0;
    }
    unsigned char signature[16];
    size_t prefix_size = 0;
#ifndef __ANDROID__
    prefix_size = SDL_ReadIO(stream, signature, sizeof(signature));
    if (SDL_GetIOStatus(stream) != SDL_IO_STATUS_READY && SDL_GetIOStatus(stream) != SDL_IO_STATUS_EOF) {
        char error[512]; SDL_strlcpy(error, SDL_GetError(), sizeof(error));
        SDL_CloseIO(stream); SDL_free(path); platform_import_complete_error(job, error); return 0;
    }
    if (job->requested == PLATFORM_IMPORT_SQLITE ||
        (job->requested == PLATFORM_IMPORT_ZOTERO && is_sqlite(signature, prefix_size))) {
        SDL_CloseIO(stream);
        char *name = SDL_strdup(basename_utf8(path));
        if (!name) { SDL_free(path); platform_import_complete_error(job, "内存不足"); return 0; }
        /* Pass the original path to SQLite's read-only transaction. A copied
         * main database could omit committed data in its WAL sidecar. */
        job->file = (PlatformImportFile){.kind = PLATFORM_IMPORT_SQLITE, .filename = name, .native_path = path};
        completed(job, PLATFORM_IMPORT_DONE, "已选择数据库"); return 0;
    }
#endif
    size_t size = 0;
    char error[512];
    /* Keep this same handle, including for Android content:// providers whose
     * length is unknown. Never allocate from an untrusted reported size. */
    unsigned char *data = read_bounded(stream, signature, prefix_size,
        PLATFORM_IMPORT_MAX_BYTES, &size, error, sizeof(error));
    if (!data) { SDL_free(path); platform_import_complete_error(job, error); return 0; }
#ifdef __ANDROID__
    char *name = android_display_name(path, job->requested);
#else
    char *name = SDL_strdup(basename_utf8(path));
#endif
    SDL_free(path);
    adopt_bytes(job, name, data, size);
    return 0;
}
static void SDLCALL selected_file(void *userdata, const char *const *paths, int filter) {
    (void)filter;
    PlatformImport *job = userdata;
    if (!paths) { platform_import_complete_error(job, SDL_GetError()); return; }
    if (!paths[0]) { platform_import_complete_cancel(job); return; }
    ReadJob *read = SDL_malloc(sizeof(*read));
    char *path = SDL_strdup(paths[0]);
    if (!read || !path) { SDL_free(read); SDL_free(path); platform_import_complete_error(job, "内存不足"); return; }
    *read = (ReadJob){job, path};
    SDL_Thread *thread = SDL_CreateThread(read_selected, "import selected file", read);
    if (thread) SDL_DetachThread(thread);
    else { SDL_free(read); SDL_free(path); platform_import_complete_error(job, SDL_GetError()); }
}
#endif

PlatformImport *platform_import_open(SDL_Window *window, PlatformImportKind kind) {
    if (kind < PLATFORM_IMPORT_JSON || kind > PLATFORM_IMPORT_BACKUP) return NULL;
    PlatformImport *job = SDL_calloc(1, sizeof(*job));
    if (!job) return NULL;
    job->requested = kind;
    SDL_SetAtomicInt(&job->status, PLATFORM_IMPORT_PENDING);
#ifdef __EMSCRIPTEN__
    (void)window;
    platform_import_js_begin(job, kind, PLATFORM_IMPORT_MAX_BYTES);
#elif defined(SDL_PLATFORM_IOS)
    platform_import_ios_begin(window, job, kind);
#else
    const SDL_DialogFileFilter *filters = kind == PLATFORM_IMPORT_JSON ? json_filters
        : kind == PLATFORM_IMPORT_SQLITE ? sqlite_filters
        : kind == PLATFORM_IMPORT_IMAGE ? image_filters
        : kind == PLATFORM_IMPORT_BACKUP ? backup_filters
        : kind == PLATFORM_IMPORT_DOCUMENT ? document_filters : zotero_filters;
    SDL_ShowOpenFileDialog(selected_file, job, window, filters,
        kind == PLATFORM_IMPORT_ZOTERO || kind == PLATFORM_IMPORT_DOCUMENT ? 2 : 1, NULL, false);
#endif
    return job;
}
PlatformImportStatus platform_import_status(PlatformImport *job, const char **message) {
    if (!job) { if (message) *message = "无法开始导入文件"; return PLATFORM_IMPORT_FAILED; }
    PlatformImportStatus status = (PlatformImportStatus)SDL_GetAtomicInt(&job->status);
    if (message) *message = status == PLATFORM_IMPORT_PENDING ? "请选择文件，读取完成后即可导入" : job->message;
    return status;
}
bool platform_import_take_result(PlatformImport *job, PlatformImportFile *out) {
    if (!out || platform_import_status(job, NULL) != PLATFORM_IMPORT_DONE || !job->file.filename) return false;
    *out = job->file;
    SDL_zero(job->file);
    return true;
}
void platform_import_file_free(PlatformImportFile *file) {
    if (!file) return;
    SDL_free(file->filename); SDL_free(file->native_path); SDL_free(file->data);
    SDL_zero(*file);
}
void platform_import_free(PlatformImport *job) {
    if (!job || platform_import_status(job, NULL) == PLATFORM_IMPORT_PENDING) return;
    platform_import_file_free(&job->file);
    SDL_free(job);
}
