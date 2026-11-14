#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <stdio.h>
#include <string.h>
#ifndef _WIN32
#include <sys/stat.h>
#endif
#include "platform_import.h"

static SDL_DialogFileCallback pending_callback;
static void *pending_userdata;
static void fake_dialog(SDL_DialogFileCallback callback, void *userdata, SDL_Window *window,
                        const SDL_DialogFileFilter *filters, int count,
                        const char *location, bool multiple) {
    (void)window; (void)filters;
    assert(count > 0 && !location && !multiple);
    pending_callback = callback; pending_userdata = userdata;
}
/* Exercise production ownership and async IO without opening a system picker. */
#define SDL_ShowOpenFileDialog fake_dialog
#include "../src/platform_import.c"
#undef SDL_ShowOpenFileDialog

static PlatformImport *begin(PlatformImportKind kind) {
    PlatformImport *job = platform_import_open(NULL, kind);
    assert(job && platform_import_status(job, NULL) == PLATFORM_IMPORT_PENDING);
    return job;
}
static void await_read(PlatformImport *job) {
    Uint64 deadline = SDL_GetTicks() + 10000;
    while (platform_import_status(job, NULL) == PLATFORM_IMPORT_PENDING && SDL_GetTicks() < deadline) SDL_Delay(1);
    assert(platform_import_status(job, NULL) != PLATFORM_IMPORT_PENDING);
}
typedef struct {
    const unsigned char *bytes;
    size_t length, offset, fail_after, read_count;
    Sint64 reported;
    int closed;
    bool fail_close;
} TestStream;
static Sint64 SDLCALL fixture_size(void *userdata) {
    TestStream *source = userdata;
    if (source->reported < 0) SDL_SetError("provider has no length");
    return source->reported;
}
static size_t SDLCALL fixture_read(void *userdata, void *out, size_t requested, SDL_IOStatus *status) {
    TestStream *source = userdata;
    size_t count = SDL_min(requested, (size_t)3); /* Short, progressive reads. */
    count = SDL_min(count, source->length - source->offset);
    count = SDL_min(count, source->fail_after - source->offset);
    memcpy(out, source->bytes + source->offset, count);
    source->offset += count; source->read_count += count;
    if (source->offset == source->fail_after) {
        *status = SDL_IO_STATUS_ERROR; SDL_SetError("injected provider IO failure");
    } else if (!count) *status = SDL_IO_STATUS_EOF;
    return count;
}
static bool SDLCALL fixture_close(void *userdata) {
    TestStream *source = userdata;
    ++source->closed;
    return source->fail_close ? SDL_SetError("injected close failure") : true;
}
static void test_bounded_streams(void) {
    const unsigned char bytes[] = {0, 1, 2, 3, 0, 5, 6, 7, 8, 9};
    SDL_IOStreamInterface interface;
    SDL_INIT_INTERFACE(&interface);
    interface.size = fixture_size; interface.read = fixture_read; interface.close = fixture_close;
    for (int test = 0; test < 7; ++test) {
        TestStream source = {.bytes = bytes, .length = 8, .fail_after = SIZE_MAX, .reported = -1};
        if (test == 1) { source.length = 10; source.reported = 2; } /* Grows past a reported small size. */
        if (test == 2) source.fail_after = 3;
        if (test == 3) source.fail_close = true;
        if (test == 4) source.reported = 9; /* Reject before any read. */
        if (test == 5) source.length = 0;
        if (test == 6) source.offset = 2; /* Native SQLite header sniff retains its prefix. */
        SDL_IOStream *stream = SDL_OpenIO(&interface, &source);
        assert(stream);
        char error[512] = ""; size_t size = 99;
        unsigned char *result = read_bounded(stream, bytes, test == 6 ? 2 : 0, 8, &size, error, sizeof(error));
        assert(source.closed == 1 && source.read_count <= 9);
        if (test == 0 || test == 5 || test == 6) {
            assert(result && size == source.length && !memcmp(result, bytes, size) && result[size] == 0);
            SDL_free(result);
        } else {
            assert(!result && size == 0 && *error);
            if (test == 1) assert(source.read_count == 9 && strstr(error, "128"));
            if (test == 2) assert(strstr(error, "provider IO failure"));
            if (test == 3) assert(strstr(error, "close failure"));
            if (test == 4) assert(source.read_count == 0);
        }
    }
}
int main(int argc, char **argv) {
    assert(argc == 2 && SDL_Init(0));
    test_bounded_streams();
    PlatformImportFile file = {0};
    PlatformImport *job = begin(PLATFORM_IMPORT_ZOTERO);
    assert(!platform_import_take_result(job, &file));
    platform_import_free(job); /* A pending callback must keep its job alive. */
    const char *canceled[] = {NULL};
    pending_callback(pending_userdata, canceled, -1);
    assert(platform_import_status(job, NULL) == PLATFORM_IMPORT_CANCELED);
    platform_import_free(job);

    job = begin(PLATFORM_IMPORT_ZOTERO);
    SDL_SetError("fixture picker unavailable");
    pending_callback(pending_userdata, NULL, -1);
    const char *message = NULL;
    assert(platform_import_status(job, &message) == PLATFORM_IMPORT_FAILED);
    assert(strstr(message, "fixture picker unavailable"));
    platform_import_free(job);
    job = begin(PLATFORM_IMPORT_JSON);
    platform_import_complete_error(job, "");
    assert(platform_import_status(job, &message) == PLATFORM_IMPORT_FAILED && *message);
    platform_import_free(job);

    unsigned char binary[] = {'a', 0, 'b', 0xff};
    job = begin(PLATFORM_IMPORT_JSON);
    platform_import_complete_bytes(job, "文献🙂.json", binary, sizeof(binary));
    assert(platform_import_take_result(job, &file));
    assert(file.kind == PLATFORM_IMPORT_JSON && !file.native_path && file.size == sizeof(binary));
    assert(!strcmp(file.filename, "文献🙂.json"));
    assert(!memcmp(file.data, binary, sizeof(binary)) && file.data[file.size] == 0);
    assert(file.data != binary && !platform_import_take_result(job, &(PlatformImportFile){0}));
    platform_import_free(job);
    assert(file.data[2] == 'b'); /* Ownership survives freeing the job. */
    platform_import_file_free(&file);
    assert(!file.data && !file.filename);

    job = begin(PLATFORM_IMPORT_ZOTERO);
    platform_import_complete_bytes(job, "huge", binary, PLATFORM_IMPORT_MAX_BYTES + 1);
    assert(platform_import_status(job, &message) == PLATFORM_IMPORT_FAILED && strstr(message, "128"));
    platform_import_free(job);
    job = begin(PLATFORM_IMPORT_SQLITE);
    platform_import_complete_bytes(job, "invalid.sqlite", binary, sizeof(binary));
    assert(platform_import_status(job, NULL) == PLATFORM_IMPORT_FAILED);
    platform_import_free(job);
    job = begin(PLATFORM_IMPORT_JSON);
    platform_import_complete_bytes(job, "empty.json", NULL, 0);
    assert(platform_import_take_result(job, &file) && file.size == 0 && file.data[0] == 0);
    platform_import_file_free(&file); platform_import_free(job);

    char path[2048];
    SDL_snprintf(path, sizeof(path), "%s/import-fixture-%llu.json", argv[1], (unsigned long long)SDL_GetTicksNS());
    assert(SDL_SaveFile(path, binary, sizeof(binary)));
    job = begin(PLATFORM_IMPORT_ZOTERO);
    const char *selected[] = {path, NULL};
    pending_callback(pending_userdata, selected, 0);
    await_read(job);
    assert(platform_import_take_result(job, &file));
    assert(file.data && !file.native_path && file.size == sizeof(binary));
    assert(!memcmp(file.data, binary, sizeof(binary)));
    platform_import_file_free(&file); platform_import_free(job);

    /* The magic, rather than filename extension, identifies a selected SQLite
     * file. Keep its original path so its WAL stays visible to SQLite. */
    const unsigned char sqlite[] = "SQLite format 3\0fixture database bytes";
    assert(SDL_SaveFile(path, sqlite, sizeof(sqlite)));
    job = begin(PLATFORM_IMPORT_ZOTERO);
    pending_callback(pending_userdata, selected, 0);
    await_read(job);
    assert(platform_import_take_result(job, &file));
    assert(file.kind == PLATFORM_IMPORT_SQLITE && file.native_path && !file.data);
    assert(!strcmp(file.native_path, path));
    platform_import_file_free(&file); platform_import_free(job);
    job = begin(PLATFORM_IMPORT_DOCUMENT);
    pending_callback(pending_userdata, selected, 0);
    await_read(job);
    assert(platform_import_take_result(job, &file));
    assert(file.kind == PLATFORM_IMPORT_DOCUMENT && !file.native_path && file.size == sizeof(sqlite));
    assert(!memcmp(file.data, sqlite, sizeof(sqlite)) && file.data[file.size] == 0);
    platform_import_file_free(&file); platform_import_free(job);
    size_t size;
    void *unchanged = SDL_LoadFile(path, &size);
    assert(unchanged && size == sizeof(sqlite) && !memcmp(unchanged, sqlite, size));
    SDL_free(unchanged);
    assert(SDL_RemovePath(path));

    job = begin(PLATFORM_IMPORT_ZOTERO);
    pending_callback(pending_userdata, selected, 0); /* Removed fixture. */
    await_read(job);
    assert(platform_import_status(job, NULL) == PLATFORM_IMPORT_FAILED);
    platform_import_free(job);
    job = begin(PLATFORM_IMPORT_ZOTERO);
    const char *directory[] = {argv[1], NULL};
    pending_callback(pending_userdata, directory, 0);
    await_read(job);
    assert(platform_import_status(job, NULL) == PLATFORM_IMPORT_FAILED);
    platform_import_free(job);
#ifndef _WIN32
    assert(mkfifo(path, 0600) == 0);
    job = begin(PLATFORM_IMPORT_ZOTERO);
    pending_callback(pending_userdata, selected, 0);
    await_read(job);
    assert(platform_import_status(job, NULL) == PLATFORM_IMPORT_FAILED);
    platform_import_free(job);
    assert(SDL_RemovePath(path));
#endif
    assert(!platform_import_open(NULL, (PlatformImportKind)99));
    SDL_Quit();
    puts("Import ownership, cancellation, bounded unknown/growing streams, IO failures and original SQLite path passed");
    return 0;
}
