#ifndef _WIN32
#define _POSIX_C_SOURCE 200809L
#ifdef __APPLE__
#define _DARWIN_C_SOURCE
#endif
#endif
#include "note_store.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#include <wchar.h>
#else
#include <signal.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

#ifdef _WIN32
static wchar_t *test_wide(const char *value) {
    int length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value, -1, NULL, 0);
    wchar_t *result;
    assert(length > 0);
    result = (wchar_t *)malloc((size_t)length * sizeof(wchar_t));
    assert(result);
    assert(MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value, -1, result, length));
    return result;
}
#endif

/* Tests operate exclusively in a new, empty directory beneath argv[1] or the
 * OS temporary directory. They never discover or open the user's notes. */
static char *temporary_directory(const char *parent) {
    char *result;
#ifdef _WIN32
    char *fallback = NULL;
    if (!parent) {
        wchar_t wide_path[32768];
        DWORD length = GetTempPathW(32768, wide_path);
        int bytes;
        assert(length && length < 32768);
        bytes = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wide_path, -1, NULL, 0, NULL, NULL);
        assert(bytes > 0);
        fallback = (char *)malloc((size_t)bytes);
        assert(fallback);
        assert(WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wide_path, -1, fallback, bytes, NULL, NULL));
        parent = fallback;
    }
    result = (char *)malloc(strlen(parent) + 80);
    assert(result);
    for (unsigned i = 0; i < 10000; ++i) {
        wchar_t *wide_path;
        BOOL created;
        DWORD error;
        sprintf(result, "%s/note-store-test-%lu-%u", parent, (unsigned long)GetCurrentProcessId(), i);
        wide_path = test_wide(result);
        created = CreateDirectoryW(wide_path, NULL);
        error = GetLastError();
        free(wide_path);
        if (created) { free(fallback); return result; }
        assert(error == ERROR_ALREADY_EXISTS);
    }
    assert(!"Cannot create test directory");
#else
    if (!parent || !*parent) parent = getenv("TMPDIR");
    if (!parent || !*parent) parent = "/tmp";
    result = (char *)malloc(strlen(parent) + 40);
    assert(result);
    sprintf(result, "%s/note-store-test-XXXXXX", parent);
    assert(mkdtemp(result));
    return result;
#endif
    return NULL;
}

static void remove_directory(const char *path) {
#ifdef _WIN32
    wchar_t *wide_path = test_wide(path);
    assert(RemoveDirectoryW(wide_path));
    free(wide_path);
#else
    assert(rmdir(path) == 0);
#endif
}

static void check_content(NoteStore *store, const char *name, const void *expected, size_t length) {
    char *actual = NULL;
    size_t size = 0;
    assert(note_store_read(store, name, &actual, &size) == NOTE_OK);
    assert(size == length && actual[size] == '\0');
    assert(!length || memcmp(actual, expected, length) == 0);
    note_store_free(actual);
}

static void test_names(void) {
    const char *invalid[] = {
        "", ".", "..", "../escape.md", "sub/note.md", "sub\\note.md",
        "/absolute.md", "C:\\absolute.md", "a:b.md", "a?.md", "a*.md",
        "a\n.md", "a.md ", "a.md.", "con.md", "PRN.txt", "NUL.json",
        "com1.md", "LPT9.txt", "CONIN$.md", "aux .md", "COM¹.md", "LPT².txt", "a.png",
        "\xc0\xaf.md", "\xed\xa0\x80.md", "\xf4\x90\x80\x80.md", "\xe4"
    };
    char *name = NULL;
    for (size_t i = 0; i < sizeof(invalid) / sizeof(*invalid); ++i) assert(!note_store_valid_name(invalid[i]));
    assert(note_store_valid_name("中文笔记.md"));
    assert(note_store_valid_name("😀.TXT"));
    assert(note_store_valid_name("ordinary.name.JSON"));
    assert(note_store_normalize_name("中文笔记", &name) == NOTE_OK);
    assert(!strcmp(name, "中文笔记.md")); note_store_free(name);
    assert(note_store_normalize_name("Mixed.TXT", &name) == NOTE_OK);
    assert(!strcmp(name, "Mixed.TXT")); note_store_free(name);
    assert(note_store_normalize_name("../outside", &name) == NOTE_INVALID_NAME && !name);
    assert(note_store_normalize_name("COM1", &name) == NOTE_INVALID_NAME && !name);
    {
        char long_name[260];
        memset(long_name, 'a', sizeof(long_name)); long_name[sizeof(long_name) - 1] = 0;
        assert(note_store_normalize_name(long_name, &name) == NOTE_INVALID_NAME && !name);
    }
}

int main(int argc, char **argv) {
    NoteStore store = {0};
    NoteList list = {0};
    char *root = temporary_directory(argc > 1 ? argv[1] : NULL);
    char *nested = (char *)malloc(strlen(root) + 50);
    char *data = NULL;
    size_t size = 0;
    const char initial[] = "<p>中文与 UTF-8 😀\n<strong>keep &amp; markup</strong></p>";
    const unsigned char binary[] = {'A', 0, 'B', 0xff, '\n'};
    assert(nested);
    test_names();
    sprintf(nested, "%s/nested/中文资料", root);
    assert(note_store_init(&store, nested) == NOTE_OK);
    assert(note_store_list(&store, &list) == NOTE_OK && list.count == 0);
    note_store_list_free(&list);
    assert(note_store_create(&store, "中文笔记.md", initial, sizeof(initial) - 1) == NOTE_OK);
    assert(note_store_create(&store, "a.txt", NULL, 0) == NOTE_OK);
    assert(note_store_create(&store, "z.json", binary, sizeof(binary)) == NOTE_OK);
    assert(note_store_create(&store, "中文笔记.md", "changed", 7) == NOTE_EXISTS);
    assert(store.error[0]);
    check_content(&store, "中文笔记.md", initial, sizeof(initial) - 1);
    check_content(&store, "z.json", binary, sizeof(binary));
    check_content(&store, "a.txt", "", 0);
    assert(!store.error[0]);
    /* Repeated listing must not share a directory offset from the previous call. */
    for (int i = 0; i < 3; ++i) {
        assert(note_store_list(&store, &list) == NOTE_OK && list.count == 3);
        assert(!strcmp(list.names[0], "a.txt"));
        assert(!strcmp(list.names[1], "z.json"));
        assert(!strcmp(list.names[2], "中文笔记.md"));
        note_store_list_free(&list);
    }
    assert(note_store_save(&store, "missing.md", "x", 1) == NOTE_NOT_FOUND);
    assert(note_store_read(&store, "missing.md", &data, &size) == NOTE_NOT_FOUND && !data && !size);
    assert(note_store_save(&store, "中文笔记.md", NULL, 1) == NOTE_IO_ERROR);
    check_content(&store, "中文笔记.md", initial, sizeof(initial) - 1);
    assert(note_store_create(&store, "../outside.md", "bad", 3) == NOTE_INVALID_NAME);
    assert(note_store_read(&store, "../outside.md", &data, &size) == NOTE_INVALID_NAME);
    assert(note_store_save(&store, "../outside.md", "bad", 3) == NOTE_INVALID_NAME);
    assert(note_store_delete(&store, "../outside.md") == NOTE_INVALID_NAME);
    {
        size_t length = 2 * 1024 * 1024 + 53;
        char *large = (char *)malloc(length);
        assert(large);
        for (size_t i = 0; i < length; ++i) large[i] = (char)(i % 251);
        assert(note_store_save(&store, "中文笔记.md", large, length) == NOTE_OK);
        check_content(&store, "中文笔记.md", large, length);
        free(large);
    }
    assert(note_store_save(&store, "中文笔记.md", "short", 5) == NOTE_OK);
    check_content(&store, "中文笔记.md", "short", 5);
#ifndef _WIN32
    {
        /* Force a real partial-write failure, not just input validation, and
         * verify that atomic save leaves the previous note intact. */
        struct rlimit original_limit, limit;
        void (*previous_handler)(int);
        char too_large[8192];
        memset(too_large, 'x', sizeof(too_large));
        assert(getrlimit(RLIMIT_FSIZE, &original_limit) == 0);
        limit = original_limit;
        limit.rlim_cur = 1024;
        previous_handler = signal(SIGXFSZ, SIG_IGN);
        assert(previous_handler != SIG_ERR);
        assert(setrlimit(RLIMIT_FSIZE, &limit) == 0);
        assert(note_store_save(&store, "中文笔记.md", too_large, sizeof(too_large)) == NOTE_IO_ERROR);
        assert(setrlimit(RLIMIT_FSIZE, &original_limit) == 0);
        assert(signal(SIGXFSZ, previous_handler) != SIG_ERR);
        check_content(&store, "中文笔记.md", "short", 5);
    }
    {
        char *outside = (char *)malloc(strlen(root) + 50);
        char *link = (char *)malloc(strlen(nested) + 50);
        FILE *file;
        char preserved[16] = {0};
        assert(outside && link);
        sprintf(outside, "%s/outside.txt", root);
        sprintf(link, "%s/link.md", nested);
        file = fopen(outside, "wb"); assert(file);
        assert(fwrite("untouched", 1, 9, file) == 9); assert(fclose(file) == 0);
        assert(symlink(outside, link) == 0);
        assert(note_store_read(&store, "link.md", &data, &size) == NOTE_IO_ERROR);
        assert(note_store_save(&store, "link.md", "bad", 3) == NOTE_IO_ERROR);
        assert(note_store_delete(&store, "link.md") == NOTE_IO_ERROR);
        assert(note_store_create(&store, "link.md", "bad", 3) == NOTE_EXISTS);
        assert(note_store_list(&store, &list) == NOTE_OK && list.count == 3);
        note_store_list_free(&list);
        file = fopen(outside, "rb"); assert(file);
        assert(fread(preserved, 1, sizeof(preserved), file) == 9); assert(fclose(file) == 0);
        assert(!strcmp(preserved, "untouched"));
        assert(unlink(link) == 0); assert(unlink(outside) == 0);
        sprintf(link, "%s/fifo.md", nested);
        assert(mkfifo(link, 0600) == 0);
        assert(note_store_read(&store, "fifo.md", &data, &size) == NOTE_IO_ERROR);
        assert(note_store_save(&store, "fifo.md", "bad", 3) == NOTE_IO_ERROR);
        assert(unlink(link) == 0);
        sprintf(link, "%s/folder.md", nested);
        assert(mkdir(link, 0700) == 0);
        assert(note_store_save(&store, "folder.md", "bad", 3) == NOTE_IO_ERROR);
        assert(note_store_delete(&store, "folder.md") == NOTE_IO_ERROR);
        assert(note_store_list(&store, &list) == NOTE_OK && list.count == 3);
        note_store_list_free(&list);
        assert(rmdir(link) == 0);
        free(outside); free(link);
    }
#endif
    note_store_destroy(&store);
    assert(note_store_init(&store, nested) == NOTE_OK);
    check_content(&store, "中文笔记.md", "short", 5);
    assert(note_store_save(&store, "中文笔记.md", NULL, 0) == NOTE_OK);
    check_content(&store, "中文笔记.md", "", 0);
    assert(note_store_delete(&store, "a.txt") == NOTE_OK);
    assert(note_store_delete(&store, "a.txt") == NOTE_NOT_FOUND);
    assert(note_store_delete(&store, "z.json") == NOTE_OK);
    assert(note_store_delete(&store, "中文笔记.md") == NOTE_OK);
    assert(note_store_list(&store, &list) == NOTE_OK && list.count == 0);
    note_store_list_free(&list);
    note_store_destroy(&store);
    note_store_destroy(&store);
    assert(note_store_list(&store, &list) == NOTE_IO_ERROR);
    remove_directory(nested);
    sprintf(nested, "%s/nested", root); remove_directory(nested);
    remove_directory(root);
    free(nested); free(root);
    puts("note_store: all tests passed");
    return 0;
}
