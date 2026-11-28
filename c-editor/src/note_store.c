#ifndef _WIN32
#define _POSIX_C_SOURCE 200809L
#ifdef __APPLE__
#define _DARWIN_C_SOURCE
#endif
#endif
#include "note_store.h"

#include <errno.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0600
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <wchar.h>
typedef struct StoreState {
    wchar_t *root;
    HANDLE directory;
    unsigned long sequence;
} StoreState;
#else
#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
typedef struct StoreState {
    int directory;
    unsigned long sequence;
} StoreState;
#endif

static NoteResult fail(NoteStore *store, NoteResult result, const char *message) {
    if (store) snprintf(store->error, sizeof(store->error), "%s", message);
    return result;
}

static NoteResult success(NoteStore *store) {
    store->error[0] = '\0';
    return NOTE_OK;
}

static int valid_utf8(const char *text) {
    const unsigned char *p = (const unsigned char *)text;
    while (*p) {
        uint32_t code;
        unsigned count;
        uint32_t minimum;
        if (*p < 0x80) { ++p; continue; }
        if (*p >= 0xc2 && *p <= 0xdf) { code = *p & 0x1f; count = 1; minimum = 0x80; }
        else if (*p >= 0xe0 && *p <= 0xef) { code = *p & 0x0f; count = 2; minimum = 0x800; }
        else if (*p >= 0xf0 && *p <= 0xf4) { code = *p & 7; count = 3; minimum = 0x10000; }
        else return 0;
        ++p;
        while (count--) {
            if ((*p & 0xc0) != 0x80) return 0;
            code = (code << 6) | (*p++ & 0x3f);
        }
        if (code < minimum || code > 0x10ffff || (code >= 0xd800 && code <= 0xdfff)) return 0;
    }
    return 1;
}

static unsigned char ascii_lower(unsigned char value) {
    return value >= 'A' && value <= 'Z' ? (unsigned char)(value + ('a' - 'A')) : value;
}

static int equal_ci(const char *a, const char *b) {
    while (*a && *b) {
        if (ascii_lower((unsigned char)*a++) != ascii_lower((unsigned char)*b++)) return 0;
    }
    return *a == *b;
}

static int supported_extension(const char *name) {
    const char *dot = strrchr(name, '.');
    return dot && (equal_ci(dot, ".md") || equal_ci(dot, ".txt") || equal_ci(dot, ".json"));
}

static int valid_base_name(const char *name) {
    size_t length, stem_length;
    char stem[16];
    const char *dot;
    if (!name || !(length = strlen(name)) || length > 255 || !valid_utf8(name)) return 0;
    if (!strcmp(name, ".") || !strcmp(name, "..") || name[length - 1] == '.' || name[length - 1] == ' ') return 0;
    for (size_t i = 0; i < length; ++i) {
        unsigned char c = (unsigned char)name[i];
        if (c < 32 || c == 127 || strchr("/\\:*?\"<>|", c)) return 0;
    }
    dot = strchr(name, '.');
    stem_length = dot ? (size_t)(dot - name) : length;
    while (stem_length && name[stem_length - 1] == ' ') --stem_length;
    if (stem_length < sizeof(stem)) {
        memcpy(stem, name, stem_length);
        stem[stem_length] = 0;
        if (equal_ci(stem, "con") || equal_ci(stem, "prn") || equal_ci(stem, "aux") ||
            equal_ci(stem, "nul") || equal_ci(stem, "clock$") ||
            equal_ci(stem, "conin$") || equal_ci(stem, "conout$")) return 0;
        if (stem_length == 4 && stem[3] >= '1' && stem[3] <= '9') {
            stem[3] = 0;
            if (equal_ci(stem, "com") || equal_ci(stem, "lpt")) return 0;
        }
        if (stem_length == 5 && (unsigned char)stem[3] == 0xc2 &&
            ((unsigned char)stem[4] == 0xb9 || (unsigned char)stem[4] == 0xb2 || (unsigned char)stem[4] == 0xb3)) {
            stem[3] = 0;
            if (equal_ci(stem, "com") || equal_ci(stem, "lpt")) return 0;
        }
    }
    return 1;
}

int note_store_valid_name(const char *name) {
    return valid_base_name(name) && supported_extension(name);
}
int note_store_valid_export_name(const char *name) {
    if(!valid_base_name(name)) return 0;
    const char *dot=strrchr(name,'.');
    return supported_extension(name) || (dot && (equal_ci(dot,".html") || equal_ci(dot,".zip")));
}

int note_store_valid_asset_name(const char *name) {
    if(!name || strlen(name)!=68 || name[64]!='.') return 0;
    for(size_t i=0;i<64;i++) if(!((name[i]>='0' && name[i]<='9') || (name[i]>='a' && name[i]<='f'))) return 0;
    return !strcmp(name+65,"png") || !strcmp(name+65,"jpg") || !strcmp(name+65,"bmp");
}
static NoteResult validate_asset(NoteStore *store,const char *name) {
    if(!store || !store->internal) return fail(store,NOTE_IO_ERROR,"Asset storage is not open");
    if(!note_store_valid_asset_name(name)) return fail(store,NOTE_INVALID_NAME,"Invalid asset filename");
    return NOTE_OK;
}

NoteResult note_store_normalize_name(const char *input, char **name) {
    size_t length;
    int append;
    if (!name) return NOTE_INVALID_NAME;
    *name = NULL;
    if (!valid_base_name(input)) return NOTE_INVALID_NAME;
    length = strlen(input);
    append = !supported_extension(input);
    if (length + (append ? 3u : 0u) > 255) return NOTE_INVALID_NAME;
    *name = (char *)malloc(length + (append ? 4u : 1u));
    if (!*name) return NOTE_NO_MEMORY;
    memcpy(*name, input, length);
    memcpy(*name + length, append ? ".md" : "", append ? 4u : 1u);
    return NOTE_OK;
}

void note_store_free(void *allocation) { free(allocation); }

static NoteResult validate(NoteStore *store, const char *name) {
    if (!store || !store->internal) return fail(store, NOTE_IO_ERROR, "Note storage is not open");
    if (!note_store_valid_name(name)) return fail(store, NOTE_INVALID_NAME, "Invalid note filename");
    return NOTE_OK;
}
static NoteResult validate_export(NoteStore *store,const char *name) {
    if(!store || !store->internal) return fail(store,NOTE_IO_ERROR,"Export directory is not open");
    if(!note_store_valid_export_name(name)) return fail(store,NOTE_INVALID_NAME,"Invalid export filename");
    return NOTE_OK;
}

static NoteResult add_name(NoteStore *store, NoteList *list, const char *name) {
    char **names;
    char *copy;
    size_t length = strlen(name);
    if (list->count >= SIZE_MAX / sizeof(*names) - 1) return fail(store, NOTE_NO_MEMORY, "Too many notes");
    copy = (char *)malloc(length + 1);
    if (!copy) return fail(store, NOTE_NO_MEMORY, "Cannot allocate note filename");
    memcpy(copy, name, length + 1);
    names = (char **)realloc(list->names, (list->count + 1) * sizeof(*names));
    if (!names) { free(copy); return fail(store, NOTE_NO_MEMORY, "Cannot allocate note list"); }
    list->names = names;
    list->names[list->count++] = copy;
    return NOTE_OK;
}

static int compare_names(const void *a, const void *b) {
    return strcmp(*(char *const *)a, *(char *const *)b);
}

void note_store_list_free(NoteList *list) {
    if (!list) return;
    for (size_t i = 0; i < list->count; ++i) free(list->names[i]);
    free(list->names);
    list->names = NULL;
    list->count = 0;
}

#ifdef _WIN32

static NoteResult windows_error(NoteStore *store, const char *operation, DWORD error) {
    char message[256];
    NoteResult result = NOTE_IO_ERROR;
    if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND) result = NOTE_NOT_FOUND;
    if (error == ERROR_FILE_EXISTS || error == ERROR_ALREADY_EXISTS) result = NOTE_EXISTS;
    snprintf(message, sizeof(message), "%s (Windows error %lu)", operation, (unsigned long)error);
    return fail(store, result, message);
}

static wchar_t *wide(const char *text) {
    int count;
    wchar_t *result;
    if (!text || !valid_utf8(text)) return NULL;
    count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text, -1, NULL, 0);
    if (!count || (size_t)count > SIZE_MAX / sizeof(wchar_t)) return NULL;
    result = (wchar_t *)malloc((size_t)count * sizeof(wchar_t));
    if (result && !MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text, -1, result, count)) { free(result); return NULL; }
    return result;
}

static char *utf8(const wchar_t *text) {
    int count = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text, -1, NULL, 0, NULL, NULL);
    char *result;
    if (!count) return NULL;
    result = (char *)malloc((size_t)count);
    if (result && !WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text, -1, result, count, NULL, NULL)) { free(result); return NULL; }
    return result;
}

static wchar_t *joined(StoreState *state, const wchar_t *name) {
    size_t a = wcslen(state->root), b = wcslen(name);
    wchar_t *result;
    if (b > SIZE_MAX / sizeof(wchar_t) - 2 || a > SIZE_MAX / sizeof(wchar_t) - b - 2) return NULL;
    result = (wchar_t *)malloc((a + b + 2) * sizeof(wchar_t));
    if (result) { memcpy(result, state->root, a * sizeof(wchar_t)); result[a] = L'\\'; memcpy(result + a + 1, name, (b + 1) * sizeof(wchar_t)); }
    return result;
}

static wchar_t *note_path(StoreState *state, const char *name) {
    wchar_t *part = wide(name), *path;
    if (!part) return NULL;
    path = joined(state, part);
    free(part);
    return path;
}

static int windows_mkdirs(wchar_t *path) {
    size_t length = wcslen(path);
    size_t first = 1;
    if (length >= 3 && path[1] == L':' && (path[2] == L'/' || path[2] == L'\\')) first = 3;
    else if (length >= 2 && path[0] == L'\\' && path[1] == L'\\') {
        /* UNC paths have a server and share root which cannot be created. */
        first = 2;
        if (length >= 8 && !wcsncmp(path, L"\\\\?\\UNC\\", 8)) first = 8;
        else if (length >= 7 && !wcsncmp(path, L"\\\\?\\", 4) && path[5] == L':') first = 7;
        if (first != 7) {
            while (first < length && path[first] != L'\\' && path[first] != L'/') ++first;
            if (first < length) ++first;
            while (first < length && path[first] != L'\\' && path[first] != L'/') ++first;
            if (first < length) ++first;
        }
    }
    for (size_t i = first; i <= length; ++i) {
        wchar_t previous = path[i];
        DWORD attributes;
        if (previous && previous != L'/' && previous != L'\\') continue;
        if (i == 2 && path[1] == L':') continue;
        path[i] = 0;
        attributes = GetFileAttributesW(path);
        if (attributes != INVALID_FILE_ATTRIBUTES) {
            path[i] = previous;
            if (!(attributes & FILE_ATTRIBUTE_DIRECTORY)) { SetLastError(ERROR_DIRECTORY); return 0; }
            continue;
        }
        if (!CreateDirectoryW(path, NULL)) {
            DWORD error = GetLastError();
            if (error != ERROR_ALREADY_EXISTS) { path[i] = previous; SetLastError(error); return 0; }
        }
        path[i] = previous;
    }
    return 1;
}

NoteResult note_store_init(NoteStore *store, const char *root) {
    StoreState *state;
    wchar_t *path;
    DWORD length;
    if (!store || store->internal || !root || !*root || !valid_utf8(root)) return fail(store, NOTE_IO_ERROR, "Invalid or already open storage root");
    path = wide(root);
    if (!path) return fail(store, NOTE_NO_MEMORY, "Cannot convert storage path");
    {
        DWORD full_length = GetFullPathNameW(path, 0, NULL, NULL);
        wchar_t *full;
        if (!full_length || (size_t)full_length > SIZE_MAX / sizeof(wchar_t)) { free(path); return fail(store, NOTE_IO_ERROR, "Cannot resolve storage path"); }
        full = (wchar_t *)malloc((size_t)full_length * sizeof(wchar_t));
        if (!full) { free(path); return fail(store, NOTE_NO_MEMORY, "Cannot allocate storage path"); }
        if (!GetFullPathNameW(path, full_length, full, NULL)) { free(path); free(full); return fail(store, NOTE_IO_ERROR, "Cannot resolve storage path"); }
        free(path); path = full;
    }
    if (!windows_mkdirs(path)) { DWORD error = GetLastError(); free(path); return windows_error(store, "Cannot create note directory", error); }
    state = (StoreState *)calloc(1, sizeof(*state));
    if (!state) { free(path); return fail(store, NOTE_NO_MEMORY, "Cannot allocate note storage"); }
    /* Keep the root from being renamed/replaced while path-based Windows
     * operations are in progress. POSIX uses directory-relative descriptors. */
    state->directory = CreateFileW(path, FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, NULL);
    free(path);
    if (state->directory == INVALID_HANDLE_VALUE) { DWORD error = GetLastError(); free(state); return windows_error(store, "Cannot open note directory", error); }
    {
        BY_HANDLE_FILE_INFORMATION info;
        if (!GetFileInformationByHandle(state->directory, &info) || !(info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || (info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) {
            CloseHandle(state->directory); free(state); return fail(store, NOTE_IO_ERROR, "Storage root must be a real directory");
        }
    }
    length = GetFinalPathNameByHandleW(state->directory, NULL, 0, FILE_NAME_NORMALIZED);
    if (!length || (size_t)length + 1 > SIZE_MAX / sizeof(wchar_t)) { CloseHandle(state->directory); free(state); return fail(store, NOTE_IO_ERROR, "Cannot resolve note directory"); }
    state->root = (wchar_t *)malloc(((size_t)length + 1) * sizeof(wchar_t));
    if (!state->root) { CloseHandle(state->directory); free(state); return fail(store, NOTE_NO_MEMORY, "Cannot allocate storage path"); }
    if (!GetFinalPathNameByHandleW(state->directory, state->root, length + 1, FILE_NAME_NORMALIZED)) { CloseHandle(state->directory); free(state->root); free(state); return fail(store, NOTE_IO_ERROR, "Cannot resolve note directory"); }
    store->internal = state;
    return success(store);
}

void note_store_destroy(NoteStore *store) {
    StoreState *state = store ? (StoreState *)store->internal : NULL;
    if (state) { CloseHandle(state->directory); free(state->root); free(state); }
    if (store) { store->internal = NULL; store->error[0] = 0; }
}

static int ordinary_file(HANDLE file) {
    BY_HANDLE_FILE_INFORMATION info;
    return GetFileInformationByHandle(file, &info) && !(info.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT));
}

NoteResult note_store_list(NoteStore *store, NoteList *list) {
    StoreState *state;
    WIN32_FIND_DATAW found;
    HANDLE search;
    wchar_t *pattern;
    NoteResult result = NOTE_OK;
    DWORD last_error;
    if (!list) return fail(store, NOTE_IO_ERROR, "Missing note list output");
    list->names = NULL; list->count = 0;
    if (!store || !(state = (StoreState *)store->internal)) return fail(store, NOTE_IO_ERROR, "Note storage is not open");
    pattern = joined(state, L"*");
    if (!pattern) return fail(store, NOTE_NO_MEMORY, "Cannot allocate list path");
    search = FindFirstFileW(pattern, &found);
    free(pattern);
    if (search == INVALID_HANDLE_VALUE) {
        DWORD error = GetLastError();
        return error == ERROR_FILE_NOT_FOUND ? success(store) : windows_error(store, "Cannot list notes", error);
    }
    do {
        char *name;
        if (found.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) continue;
        name = utf8(found.cFileName);
        if (!name) { result = fail(store, NOTE_NO_MEMORY, "Cannot convert note filename"); break; }
        if (note_store_valid_name(name)) result = add_name(store, list, name);
        free(name);
        if (result != NOTE_OK) break;
    } while (FindNextFileW(search, &found));
    last_error = GetLastError();
    FindClose(search);
    if (result == NOTE_OK && last_error != ERROR_NO_MORE_FILES) result = windows_error(store, "Cannot finish listing notes", last_error);
    if (result != NOTE_OK) { note_store_list_free(list); return result; }
    if (list->count > 1) qsort(list->names, list->count, sizeof(*list->names), compare_names);
    return success(store);
}

static NoteResult read_file(NoteStore *store, const char *name, char **data, size_t *size,int asset) {
    StoreState *state;
    wchar_t *path;
    HANDLE file;
    size_t used = 0, capacity = 4096;
    char *buffer;
    NoteResult result = asset?validate_asset(store,name):validate(store, name);
    if (data) *data = NULL;
    if (size) *size = 0;
    if (result != NOTE_OK) return result;
    if (!data || !size) return fail(store, NOTE_IO_ERROR, "Missing read output");
    state = (StoreState *)store->internal;
    path = note_path(state, name);
    if (!path) return fail(store, NOTE_NO_MEMORY, "Cannot allocate note path");
    file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_SEQUENTIAL_SCAN, NULL);
    free(path);
    if (file == INVALID_HANDLE_VALUE) return windows_error(store, "Cannot open note", GetLastError());
    if (!ordinary_file(file)) { CloseHandle(file); return fail(store, NOTE_IO_ERROR, "Note must be a regular file, not a link"); }
    buffer = (char *)malloc(capacity);
    if (!buffer) { CloseHandle(file); return fail(store, NOTE_NO_MEMORY, "Cannot allocate note contents"); }
    for (;;) {
        DWORD amount = 0, request;
        if (used == capacity - 1) {
            char *grown;
            if (capacity > SIZE_MAX / 2) { result = fail(store, NOTE_NO_MEMORY, "Note is too large"); break; }
            if(asset && capacity>=16u*1024u*1024u+2) { result=fail(store,NOTE_IO_ERROR,"Image asset exceeds 16 MiB"); break; }
            capacity *= 2;
            if(asset && capacity>16u*1024u*1024u+2) capacity=16u*1024u*1024u+2;
            grown = (char *)realloc(buffer, capacity);
            if (!grown) { result = fail(store, NOTE_NO_MEMORY, "Cannot allocate note contents"); break; }
            buffer = grown;
        }
        request = capacity - used - 1 > 0x40000000u ? 0x40000000u : (DWORD)(capacity - used - 1);
        if (!ReadFile(file, buffer + used, request, &amount, NULL)) { result = windows_error(store, "Cannot read note", GetLastError()); break; }
        if (!amount) break;
        used += amount;
    }
    CloseHandle(file);
    if (result != NOTE_OK) { free(buffer); return result; }
    if(asset && used>16u*1024u*1024u) { free(buffer); return fail(store,NOTE_IO_ERROR,"Image asset exceeds 16 MiB"); }
    buffer[used] = 0; *data = buffer; *size = used;
    return success(store);
}

static NoteResult windows_write(NoteStore *store, HANDLE file, const void *data, size_t size) {
    const unsigned char *bytes = (const unsigned char *)data;
    while (size) {
        DWORD amount = 0, chunk = size > 0x40000000u ? 0x40000000u : (DWORD)size;
        if (!WriteFile(file, bytes, chunk, &amount, NULL) || !amount) return windows_error(store, "Cannot write note", GetLastError());
        bytes += amount; size -= amount;
    }
    if (!FlushFileBuffers(file)) return windows_error(store, "Cannot flush note", GetLastError());
    return NOTE_OK;
}

static NoteResult create_file(NoteStore *store, const char *name, const void *data, size_t size,int exporting) {
    wchar_t *path;
    HANDLE file;
    NoteResult result = exporting==2?validate_asset(store,name):exporting?validate_export(store,name):validate(store, name);
    if (result != NOTE_OK) return result;
    if (size && !data) return fail(store, NOTE_IO_ERROR, "Missing note data");
    path = note_path((StoreState *)store->internal, name);
    if (!path) return fail(store, NOTE_NO_MEMORY, "Cannot allocate note path");
    file = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE) { DWORD error = GetLastError(); free(path); return windows_error(store, "Cannot create note", error); }
    result = windows_write(store, file, data, size);
    if (!CloseHandle(file) && result == NOTE_OK) result = windows_error(store, "Cannot close note", GetLastError());
    if (result != NOTE_OK) DeleteFileW(path);
    free(path);
    return result == NOTE_OK ? success(store) : result;
}

static NoteResult check_windows_target(NoteStore *store, const wchar_t *path) {
    DWORD attributes = GetFileAttributesW(path);
    if (attributes == INVALID_FILE_ATTRIBUTES) return windows_error(store, "Cannot inspect note", GetLastError());
    if (attributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) return fail(store, NOTE_IO_ERROR, "Note must be a regular file, not a link");
    return NOTE_OK;
}

static NoteResult save_file(NoteStore *store, const char *name, const void *data, size_t size,int exporting) {
    StoreState *state;
    wchar_t *path, *temporary = NULL;
    HANDLE file = INVALID_HANDLE_VALUE;
    NoteResult result = exporting==2?validate_asset(store,name):exporting?validate_export(store,name):validate(store, name);
    if (result != NOTE_OK) return result;
    if (size && !data) return fail(store, NOTE_IO_ERROR, "Missing note data");
    state = (StoreState *)store->internal;
    path = note_path(state, name);
    if (!path) return fail(store, NOTE_NO_MEMORY, "Cannot allocate note path");
    result = check_windows_target(store, path);
    if (result != NOTE_OK) { free(path); return result; }
    for (unsigned attempt = 0; attempt < 256; ++attempt) {
        wchar_t name_buffer[96];
        swprintf(name_buffer, 96, L".editor-save-%lu-%lu.tmp", (unsigned long)GetCurrentProcessId(), ++state->sequence);
        free(temporary); temporary = joined(state, name_buffer);
        if (!temporary) { result = fail(store, NOTE_NO_MEMORY, "Cannot allocate temporary path"); break; }
        file = CreateFileW(temporary, GENERIC_WRITE, 0, NULL, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL);
        if (file != INVALID_HANDLE_VALUE) break;
        if (GetLastError() != ERROR_FILE_EXISTS && GetLastError() != ERROR_ALREADY_EXISTS) break;
    }
    if (file == INVALID_HANDLE_VALUE) {
        if (result == NOTE_OK) result = windows_error(store, "Cannot create temporary note", GetLastError());
        free(temporary); free(path); return result;
    }
    result = windows_write(store, file, data, size);
    if (!CloseHandle(file) && result == NOTE_OK) result = windows_error(store, "Cannot close temporary note", GetLastError());
    if (result == NOTE_OK) result = check_windows_target(store, path);
    if (result == NOTE_OK && !ReplaceFileW(path, temporary, NULL, 0, NULL, NULL)) result = windows_error(store, "Cannot replace note", GetLastError());
    if (result != NOTE_OK) DeleteFileW(temporary);
    free(temporary); free(path);
    return result == NOTE_OK ? success(store) : result;
}

NoteResult note_store_delete(NoteStore *store, const char *name) {
    wchar_t *path;
    NoteResult result = validate(store, name);
    if (result != NOTE_OK) return result;
    path = note_path((StoreState *)store->internal, name);
    if (!path) return fail(store, NOTE_NO_MEMORY, "Cannot allocate note path");
    result = check_windows_target(store, path);
    if (result == NOTE_OK && !DeleteFileW(path)) result = windows_error(store, "Cannot delete note", GetLastError());
    free(path);
    return result == NOTE_OK ? success(store) : result;
}

#else

static NoteResult posix_error(NoteStore *store, const char *operation) {
    int error = errno;
    char message[256];
    NoteResult result = error == ENOENT ? NOTE_NOT_FOUND : error == EEXIST ? NOTE_EXISTS : NOTE_IO_ERROR;
    snprintf(message, sizeof(message), "%s: %s", operation, strerror(error));
    return fail(store, result, message);
}

static int make_directories(char *path) {
    size_t length = strlen(path);
    for (size_t i = 1; i <= length; ++i) {
        char previous = path[i];
        struct stat info;
        if (previous && previous != '/') continue;
        path[i] = 0;
        if (mkdir(path, 0700) != 0 && (errno != EEXIST || stat(path, &info) != 0 || !S_ISDIR(info.st_mode))) {
            int error = errno; path[i] = previous; errno = error; return 0;
        }
        path[i] = previous;
    }
    return 1;
}

NoteResult note_store_init(NoteStore *store, const char *root) {
    StoreState *state;
    char *path;
    size_t length;
    int fd;
    if (!store || store->internal || !root || !*root || !valid_utf8(root)) return fail(store, NOTE_IO_ERROR, "Invalid or already open storage root");
    length = strlen(root);
    path = (char *)malloc(length + 1);
    if (!path) return fail(store, NOTE_NO_MEMORY, "Cannot allocate storage path");
    memcpy(path, root, length + 1);
    while (length > 1 && path[length - 1] == '/') path[--length] = 0;
    if (!make_directories(path)) { free(path); return posix_error(store, "Cannot create note directory"); }
    fd = open(path, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    free(path);
    if (fd < 0) return posix_error(store, "Cannot open note directory");
    state = (StoreState *)calloc(1, sizeof(*state));
    if (!state) { close(fd); return fail(store, NOTE_NO_MEMORY, "Cannot allocate note storage"); }
    state->directory = fd;
    store->internal = state;
    return success(store);
}

void note_store_destroy(NoteStore *store) {
    StoreState *state = store ? (StoreState *)store->internal : NULL;
    if (state) { close(state->directory); free(state); }
    if (store) { store->internal = NULL; store->error[0] = 0; }
}

NoteResult note_store_list(NoteStore *store, NoteList *list) {
    StoreState *state;
    DIR *directory;
    struct dirent *entry;
    NoteResult result = NOTE_OK;
    int fd;
    if (!list) return fail(store, NOTE_IO_ERROR, "Missing note list output");
    list->names = NULL; list->count = 0;
    if (!store || !(state = (StoreState *)store->internal)) return fail(store, NOTE_IO_ERROR, "Note storage is not open");
    fd = openat(state->directory, ".", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (fd < 0) return posix_error(store, "Cannot open note list");
    directory = fdopendir(fd);
    if (!directory) { int error = errno; close(fd); errno = error; return posix_error(store, "Cannot list notes"); }
    for (;;) {
        struct stat info;
        errno = 0;
        entry = readdir(directory);
        if (!entry) { if (errno) result = posix_error(store, "Cannot finish listing notes"); break; }
        if (!note_store_valid_name(entry->d_name)) continue;
        if (fstatat(state->directory, entry->d_name, &info, AT_SYMLINK_NOFOLLOW) != 0) {
            if (errno == ENOENT) continue;
            result = posix_error(store, "Cannot inspect note"); break;
        }
        if (!S_ISREG(info.st_mode)) continue;
        result = add_name(store, list, entry->d_name);
        if (result != NOTE_OK) break;
    }
    if (closedir(directory) != 0 && result == NOTE_OK) result = posix_error(store, "Cannot close note list");
    if (result != NOTE_OK) { note_store_list_free(list); return result; }
    if (list->count > 1) qsort(list->names, list->count, sizeof(*list->names), compare_names);
    return success(store);
}

static NoteResult read_file(NoteStore *store, const char *name, char **data, size_t *size,int asset) {
    int fd;
    struct stat info;
    size_t used = 0, capacity = 4096;
    char *buffer;
    NoteResult result = asset?validate_asset(store,name):validate(store, name);
    if (data) *data = NULL;
    if (size) *size = 0;
    if (result != NOTE_OK) return result;
    if (!data || !size) return fail(store, NOTE_IO_ERROR, "Missing read output");
    fd = openat(((StoreState *)store->internal)->directory, name, O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
    if (fd < 0) return posix_error(store, "Cannot open note");
    if (fstat(fd, &info) != 0 || !S_ISREG(info.st_mode)) { close(fd); return fail(store, NOTE_IO_ERROR, "Note must be a regular file, not a link"); }
    buffer = (char *)malloc(capacity);
    if (!buffer) { close(fd); return fail(store, NOTE_NO_MEMORY, "Cannot allocate note contents"); }
    for (;;) {
        ssize_t amount;
        size_t request;
        if (used == capacity - 1) {
            char *grown;
            if (capacity > SIZE_MAX / 2) { result = fail(store, NOTE_NO_MEMORY, "Note is too large"); break; }
            if(asset && capacity>=16u*1024u*1024u+2) { result=fail(store,NOTE_IO_ERROR,"Image asset exceeds 16 MiB"); break; }
            capacity *= 2;
            if(asset && capacity>16u*1024u*1024u+2) capacity=16u*1024u*1024u+2;
            grown = (char *)realloc(buffer, capacity);
            if (!grown) { result = fail(store, NOTE_NO_MEMORY, "Cannot allocate note contents"); break; }
            buffer = grown;
        }
        request = capacity - used - 1;
        if (request > 0x40000000u) request = 0x40000000u;
        amount = read(fd, buffer + used, request);
        if (amount < 0) {
            if (errno == EINTR) continue;
            result = posix_error(store, "Cannot read note"); break;
        }
        if (!amount) break;
        used += (size_t)amount;
    }
    if (close(fd) != 0 && result == NOTE_OK) result = posix_error(store, "Cannot close note");
    if (result != NOTE_OK) { free(buffer); return result; }
    if(asset && used>16u*1024u*1024u) { free(buffer); return fail(store,NOTE_IO_ERROR,"Image asset exceeds 16 MiB"); }
    buffer[used] = 0; *data = buffer; *size = used;
    return success(store);
}

static NoteResult posix_write(NoteStore *store, int fd, const void *data, size_t size) {
    const unsigned char *bytes = (const unsigned char *)data;
    while (size) {
        size_t chunk = size > 0x40000000u ? 0x40000000u : size;
        ssize_t amount = write(fd, bytes, chunk);
        if (amount < 0) {
            if (errno == EINTR) continue;
            return posix_error(store, "Cannot write note");
        }
        if (!amount) return fail(store, NOTE_IO_ERROR, "Writing note made no progress");
        bytes += amount; size -= (size_t)amount;
    }
    if (fsync(fd) != 0) return posix_error(store, "Cannot flush note");
    return NOTE_OK;
}

static NoteResult create_file(NoteStore *store, const char *name, const void *data, size_t size,int exporting) {
    StoreState *state;
    int fd;
    NoteResult result = exporting==2?validate_asset(store,name):exporting?validate_export(store,name):validate(store, name);
    if (result != NOTE_OK) return result;
    if (size && !data) return fail(store, NOTE_IO_ERROR, "Missing note data");
    state = (StoreState *)store->internal;
    fd = openat(state->directory, name, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (fd < 0) return posix_error(store, "Cannot create note");
    result = posix_write(store, fd, data, size);
    if (close(fd) != 0 && result == NOTE_OK) result = posix_error(store, "Cannot close note");
    if (result != NOTE_OK) unlinkat(state->directory, name, 0);
    else if (fsync(state->directory) != 0) result = posix_error(store, "Cannot flush note directory");
    return result == NOTE_OK ? success(store) : result;
}

static NoteResult check_posix_target(NoteStore *store, const char *name, struct stat *info) {
    if (fstatat(((StoreState *)store->internal)->directory, name, info, AT_SYMLINK_NOFOLLOW) != 0) return posix_error(store, "Cannot inspect note");
    if (!S_ISREG(info->st_mode)) return fail(store, NOTE_IO_ERROR, "Note must be a regular file, not a link");
    return NOTE_OK;
}

static NoteResult save_file(NoteStore *store, const char *name, const void *data, size_t size,int exporting) {
    StoreState *state;
    struct stat info;
    char temporary[96];
    int fd = -1;
    NoteResult result = exporting==2?validate_asset(store,name):exporting?validate_export(store,name):validate(store, name);
    if (result != NOTE_OK) return result;
    if (size && !data) return fail(store, NOTE_IO_ERROR, "Missing note data");
    state = (StoreState *)store->internal;
    result = check_posix_target(store, name, &info);
    if (result != NOTE_OK) return result;
    for (unsigned attempt = 0; attempt < 256; ++attempt) {
        snprintf(temporary, sizeof(temporary), ".editor-save-%ld-%lu.tmp", (long)getpid(), ++state->sequence);
        fd = openat(state->directory, temporary, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
        if (fd >= 0 || errno != EEXIST) break;
    }
    if (fd < 0) return posix_error(store, "Cannot create temporary note");
    if (fchmod(fd, info.st_mode & 0777) != 0) result = posix_error(store, "Cannot preserve note permissions");
    if (result == NOTE_OK) result = posix_write(store, fd, data, size);
    if (close(fd) != 0 && result == NOTE_OK) result = posix_error(store, "Cannot close temporary note");
    if (result == NOTE_OK) result = check_posix_target(store, name, &info);
    if (result == NOTE_OK && renameat(state->directory, temporary, state->directory, name) != 0) result = posix_error(store, "Cannot replace note");
    if (result != NOTE_OK) unlinkat(state->directory, temporary, 0);
    else if (fsync(state->directory) != 0) result = posix_error(store, "Cannot flush note directory");
    return result == NOTE_OK ? success(store) : result;
}

NoteResult note_store_delete(NoteStore *store, const char *name) {
    StoreState *state;
    struct stat info;
    NoteResult result = validate(store, name);
    if (result != NOTE_OK) return result;
    state = (StoreState *)store->internal;
    result = check_posix_target(store, name, &info);
    if (result != NOTE_OK) return result;
    if (unlinkat(state->directory, name, 0) != 0) return posix_error(store, "Cannot delete note");
    if (fsync(state->directory) != 0) return posix_error(store, "Cannot flush note directory");
    return success(store);
}
#endif

NoteResult note_store_create(NoteStore *store,const char *name,const void *data,size_t size) {
    return create_file(store,name,data,size,0);
}
NoteResult note_store_save(NoteStore *store,const char *name,const void *data,size_t size) {
    return save_file(store,name,data,size,0);
}
NoteResult note_store_export(NoteStore *store,const char *name,const void *data,size_t size) {
    NoteResult result=create_file(store,name,data,size,1);
    return result==NOTE_EXISTS?save_file(store,name,data,size,1):result;
}

NoteResult note_store_read(NoteStore *store,const char *name,char **data,size_t *size) { return read_file(store,name,data,size,0); }
NoteResult note_store_read_asset(NoteStore *store,const char *name,char **data,size_t *size) { return read_file(store,name,data,size,1); }
NoteResult note_store_create_asset(NoteStore *store,const char *name,const void *data,size_t size) {
    if(size>16u*1024u*1024u) return fail(store,NOTE_IO_ERROR,"Image asset exceeds 16 MiB");
    return create_file(store,name,data,size,2);
}
