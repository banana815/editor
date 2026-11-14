#include "platform_export.h"
#include "note_store.h"
#include <stdlib.h>
#include <string.h>
#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#endif

struct PlatformExport {
    SDL_AtomicInt status;
    char *data, *filename;
    char message[512];
};

static void completed(PlatformExport *job, PlatformExportStatus status, const char *message) {
    SDL_strlcpy(job->message, message, sizeof(job->message));
    /* Release the result only after the worker has finished using the job. */
    SDL_SetAtomicInt(&job->status, status);
}

#ifdef __EMSCRIPTEN__
EM_JS(int, download_json, (const char *filename, const char *data), {
    var url = null, link = null;
    try {
        var blob = new Blob([UTF8ToString(data)], {type:'application/json;charset=utf-8'});
        url = URL.createObjectURL(blob);
        link = document.createElement('a');
        link.href = url; link.download = UTF8ToString(filename);
        link.style.display = 'none'; document.body.appendChild(link);
        link.click(); link.remove();
        // Keep the URL alive until the browser has consumed the download.
        setTimeout(function() { URL.revokeObjectURL(url); }, 60000);
        return 1;
    } catch (error) {
        if (link) link.remove();
        if (url) URL.revokeObjectURL(url);
        return 0;
    }
});
#elif defined(SDL_PLATFORM_IOS)
void platform_export_ios_begin(SDL_Window *window, PlatformExport *job,
                               const char *filename, const char *json);
void platform_export_ios_complete(PlatformExport *job, PlatformExportStatus status, const char *message) {
    completed(job, status, message);
}
#else
static const SDL_DialogFileFilter filters[] = {{"JSON document", "json"}};

static void SDLCALL selected_file(void *userdata, const char *const *paths, int filter) {
    (void)filter;
    PlatformExport *job = userdata;
    if (!paths) { completed(job, PLATFORM_EXPORT_FAILED, SDL_GetError()); return; }
    if (!paths[0]) { completed(job, PLATFORM_EXPORT_CANCELED, "已取消导出"); return; }
#ifdef __ANDROID__
    /* Android's document provider returns a URI, not a native filesystem path. */
    if (!SDL_SaveFile(paths[0], job->data, strlen(job->data))) {
        completed(job, PLATFORM_EXPORT_FAILED, SDL_GetError()); return;
    }
#else
    /* Use the same exclusive-create / atomic-replace implementation as notes.
     * A failed export must not truncate an existing selected document. */
    char *path = SDL_strdup(paths[0]);
    if (!path) { completed(job, PLATFORM_EXPORT_FAILED, "内存不足，未导出"); return; }
    char *slash = strrchr(path, '/');
#ifdef _WIN32
    char *backslash = strrchr(path, '\\');
    if (backslash && (!slash || backslash > slash)) slash = backslash;
#endif
    const char *name = slash ? slash + 1 : path;
    const char *extension = strrchr(name, '.');
    if (!extension || SDL_strcasecmp(extension, ".json") || !note_store_valid_name(name)) {
        SDL_free(path);
        completed(job, PLATFORM_EXPORT_FAILED, "请选择有效的 .json 文件名"); return;
    }
    char *parent = slash ? SDL_strndup(path, (size_t)(slash - path) + 1) : SDL_strdup(".");
    NoteStore output = {0};
    NoteResult result = parent ? note_store_init(&output, parent) : NOTE_NO_MEMORY;
    if (result == NOTE_OK) {
        result = note_store_create(&output, name, job->data, strlen(job->data));
        if (result == NOTE_EXISTS) result = note_store_save(&output, name, job->data, strlen(job->data));
    }
    char error[256];
    SDL_strlcpy(error, *output.error ? output.error : "无法导出文件", sizeof(error));
    note_store_destroy(&output);
    SDL_free(parent); SDL_free(path);
    if (result != NOTE_OK) { completed(job, PLATFORM_EXPORT_FAILED, error); return; }
#endif
    completed(job, PLATFORM_EXPORT_DONE, "JSON 文档已导出");
}
#endif

PlatformExport *platform_export_json(SDL_Window *window, const char *filename, const char *json) {
    if (!filename || !json) return NULL;
    PlatformExport *job = SDL_calloc(1, sizeof(*job));
    if (!job) return NULL;
    job->data = SDL_strdup(json); job->filename = SDL_strdup(filename);
    if (!job->data || !job->filename) {
        SDL_free(job->data); SDL_free(job->filename); SDL_free(job); return NULL;
    }
    SDL_SetAtomicInt(&job->status, PLATFORM_EXPORT_PENDING);
#ifdef __EMSCRIPTEN__
    (void)window;
    if (download_json(job->filename, job->data))
        completed(job, PLATFORM_EXPORT_DONE, "已请求下载 JSON 文档");
    else completed(job, PLATFORM_EXPORT_FAILED, "无法开始下载 JSON 文档");
#elif defined(SDL_PLATFORM_IOS)
    platform_export_ios_begin(window, job, job->filename, job->data);
#else
    SDL_ShowSaveFileDialog(selected_file, job, window, filters, 1, job->filename);
#endif
    return job;
}
PlatformExportStatus platform_export_status(PlatformExport *job, const char **message) {
    if (!job) return PLATFORM_EXPORT_FAILED;
    PlatformExportStatus status = (PlatformExportStatus)SDL_GetAtomicInt(&job->status);
    if (message) *message = status == PLATFORM_EXPORT_PENDING ? "请选择导出位置" : job->message;
    return status;
}
void platform_export_free(PlatformExport *job) {
    if (!job || platform_export_status(job, NULL) == PLATFORM_EXPORT_PENDING) return;
    SDL_free(job->data); SDL_free(job->filename); SDL_free(job);
}
