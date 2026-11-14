#include "zotero.h"
#include "cJSON.h"
#include <inttypes.h>
#include <errno.h>
#include <limits.h>
#include <sqlite3.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "zotero_mapping.h"

#define CE_ZOTERO_UNTITLED "未命名文献"
#define CE_ZOTERO_UNKNOWN_AUTHOR "未知作者"

/* This module deliberately contains only a small, deterministic approximation
 * of three citation styles. It is not a CSL processor. */

typedef struct {
    char *error;
    size_t capacity;
} Report;

static bool fail(Report *report, const char *format, ...) {
    if (report->error && report->capacity) {
        va_list args;
        va_start(args, format);
        vsnprintf(report->error, report->capacity, format, args);
        va_end(args);
        report->error[report->capacity - 1] = '\0';
    }
    return false;
}

static void clear_error(Report *report) {
    if (report->error && report->capacity)
        report->error[0] = '\0';
}

static char *copy_n(const char *source, size_t length) {
    char *result;
    if (!source || length == SIZE_MAX)
        return NULL;
    result = (char *)malloc(length + 1);
    if (!result)
        return NULL;
    if (length)
        memcpy(result, source, length);
    result[length] = '\0';
    return result;
}

static char *copy_string(const char *source) {
    return copy_n(source ? source : "", source ? strlen(source) : 0);
}

/* Public results and library fields have a free() ownership contract. cJSON
 * hooks may allocate from an unrelated arena, so never expose its print buffer. */
static char *print_json_owned(const cJSON *object) {
    char *printed = cJSON_PrintUnformatted(object);
    if (!printed) return NULL;
    char *owned = copy_string(printed);
    cJSON_free(printed);
    return owned;
}

static bool ascii_space(unsigned char value) {
    return value == ' ' || value == '\t' || value == '\n' || value == '\r' ||
           value == '\f' || value == '\v';
}

static bool ascii_digit(unsigned char value) { return value >= '0' && value <= '9'; }

static void trim_in_place(char *text) {
    size_t length, start = 0;
    if (!text)
        return;
    length = strlen(text);
    while (start < length && ascii_space((unsigned char)text[start]))
        start++;
    while (length > start && ascii_space((unsigned char)text[length - 1]))
        length--;
    if (start)
        memmove(text, text + start, length - start);
    text[length - start] = '\0';
}

static char *trim_copy(const char *text) {
    size_t start = 0, length;
    if (!text || !*text)
        return copy_string("");
    length = strlen(text);
    while (start < length && ascii_space((unsigned char)text[start]))
        start++;
    while (length > start && ascii_space((unsigned char)text[length - 1]))
        length--;
    return copy_n(text + start, length - start);
}

static bool utf8_valid_n(const char *text, size_t length) {
    const unsigned char *bytes = (const unsigned char *)text;
    size_t position = 0;
    while (position < length) {
        unsigned char first = bytes[position++];
        unsigned extra;
        uint32_t codepoint;
        if (first < 0x80)
            continue;
        if (first >= 0xc2 && first <= 0xdf) {
            codepoint = first & 0x1f;
            extra = 1;
        } else if (first >= 0xe0 && first <= 0xef) {
            codepoint = first & 0x0f;
            extra = 2;
        } else if (first >= 0xf0 && first <= 0xf4) {
            codepoint = first & 0x07;
            extra = 3;
        } else
            return false;
        if (extra > length - position)
            return false;
        for (unsigned i = 0; i < extra; i++) {
            unsigned char next = bytes[position++];
            if ((next & 0xc0) != 0x80)
                return false;
            codepoint = (codepoint << 6) | (next & 0x3f);
        }
        if ((extra == 1 && codepoint < 0x80) || (extra == 2 && codepoint < 0x800) ||
            (extra == 3 && codepoint < 0x10000) || codepoint > 0x10ffff ||
            (codepoint >= 0xd800 && codepoint <= 0xdfff))
            return false;
    }
    return true;
}

static bool utf8_valid(const char *text) {
    return text && utf8_valid_n(text, strlen(text));
}

static bool grow_array(void **pointer, size_t *capacity, size_t needed, size_t element_size) {
    size_t next;
    void *replacement;
    if (needed <= *capacity)
        return true;
    next = *capacity ? *capacity : 8;
    while (next < needed) {
        if (next > SIZE_MAX / 2) {
            next = needed;
            break;
        }
        next *= 2;
    }
    if (element_size == 0 || next > SIZE_MAX / element_size)
        return false;
    replacement = realloc(*pointer, next * element_size);
    if (!replacement)
        return false;
    *pointer = replacement;
    *capacity = next;
    return true;
}

typedef struct {
    char *data;
    size_t length;
    size_t capacity;
    bool failed;
} TextBuffer;

static bool text_reserve(TextBuffer *buffer, size_t needed) {
    size_t capacity;
    char *replacement;
    if (buffer->failed)
        return false;
    if (needed <= buffer->capacity)
        return true;
    capacity = buffer->capacity ? buffer->capacity : 64;
    while (capacity < needed) {
        if (capacity > SIZE_MAX / 2) {
            capacity = needed;
            break;
        }
        capacity *= 2;
    }
    if (capacity == SIZE_MAX) {
        buffer->failed = true;
        return false;
    }
    replacement = (char *)realloc(buffer->data, capacity);
    if (!replacement) {
        buffer->failed = true;
        return false;
    }
    buffer->data = replacement;
    buffer->capacity = capacity;
    return true;
}

static bool text_add(TextBuffer *buffer, const char *text, size_t length) {
    if (buffer->failed)
        return false;
    if (length > SIZE_MAX - buffer->length - 1) {
        buffer->failed = true;
        return false;
    }
    if (!text_reserve(buffer, buffer->length + length + 1))
        return false;
    if (length)
        memcpy(buffer->data + buffer->length, text, length);
    buffer->length += length;
    buffer->data[buffer->length] = '\0';
    return true;
}

static bool text_add_string(TextBuffer *buffer, const char *text) {
    return text_add(buffer, text ? text : "", text ? strlen(text) : 0);
}

static void text_discard(TextBuffer *buffer) {
    free(buffer->data);
    buffer->data = NULL;
    buffer->length = 0;
    buffer->capacity = 0;
    buffer->failed = false;
}

static char *text_take(TextBuffer *buffer) {
    char *result;
    if (buffer->failed) {
        text_discard(buffer);
        return NULL;
    }
    if (!buffer->data) {
        buffer->data = copy_string("");
        if (!buffer->data)
            return NULL;
        buffer->capacity = 1;
    }
    result = buffer->data;
    buffer->data = NULL;
    buffer->length = 0;
    buffer->capacity = 0;
    return result;
}

static char *i64_copy(int64_t value) {
    char text[32];
    int written = snprintf(text, sizeof text, "%" PRId64, value);
    if (written < 0 || (size_t)written >= sizeof text)
        return NULL;
    return copy_string(text);
}

static char *json_number_copy(double value) {
    char text[64];
    int written;
    /* Values that are exactly representable as an integer are rendered
     * without a decimal point or locale-dependent grouping. */
    if (value >= -9007199254740992.0 && value <= 9007199254740992.0) {
        int64_t integer = (int64_t)value;
        if ((double)integer == value) {
            written = snprintf(text, sizeof text, "%" PRId64, integer);
            if (written < 0 || (size_t)written >= sizeof text)
                return NULL;
            return copy_string(text);
        }
    }
    written = snprintf(text, sizeof text, "%.17g", value);
    if (written < 0 || (size_t)written >= sizeof text)
        return NULL;
    return copy_string(text);
}

static void item_free(CeZoteroItem *item) {
    if (!item)
        return;
    free(item->id);
    free(item->item_key);
    free(item->title);
    free(item->author);
    free(item->year);
    free(item->cite_key);
    free(item->item_uri);
    free(item->pdf_uri);
    free(item->csl_json);
    memset(item, 0, sizeof *item);
}

void ce_zotero_library_free(CeZoteroLibrary *library) {
    size_t i;
    if (!library)
        return;
    for (i = 0; i < library->item_count; i++)
        item_free(&library->items[i]);
    free(library->items);
    for (i = 0; i < library->collection_count; i++) {
        free(library->collections[i].key);
        free(library->collections[i].name);
        free(library->collections[i].children);
        free(library->collections[i].item_indices);
    }
    free(library->collections);
    free(library->roots);
    memset(library, 0, sizeof *library);
}

/* ------------------------------------------------------------------------- */
/* CSL JSON import                                                           */
/* ------------------------------------------------------------------------- */

static const cJSON *object_field(const cJSON *object, const char *key) {
    return object ? cJSON_GetObjectItemCaseSensitive(object, key) : NULL;
}

static bool scalar_valid(const cJSON *value, bool allow_number) {
    return !value || cJSON_IsNull(value) || cJSON_IsString(value) ||
           (allow_number && cJSON_IsNumber(value));
}

static bool validate_csl_item(const cJSON *item, Report *report) {
    static const char *string_fields[] = {"title", "URL", "citationKey", "citekey", "pdfUri"};
    const cJSON *value, *person, *part, *component;
    size_t i;

    if (!cJSON_IsObject(item))
        return fail(report, "CSL item must be a JSON object");
    for (i = 0; i < sizeof string_fields / sizeof *string_fields; i++) {
        value = object_field(item, string_fields[i]);
        if (!scalar_valid(value, false))
            return fail(report, "CSL field '%s' must be a string", string_fields[i]);
    }
    if (!scalar_valid(object_field(item, "id"), true))
        return fail(report, "CSL field 'id' must be a string or number");
    if (!scalar_valid(object_field(item, "year"), true))
        return fail(report, "CSL field 'year' must be a string or number");

    value = object_field(item, "author");
    if (value && !cJSON_IsNull(value)) {
        if (!cJSON_IsArray(value))
            return fail(report, "CSL field 'author' must be an array");
        cJSON_ArrayForEach (person, value) {
            if (!cJSON_IsObject(person))
                return fail(report, "CSL author entry must be an object");
            for (i = 0; i < 3; i++) {
                static const char *name_fields[] = {"literal", "family", "given"};
                const cJSON *name = object_field(person, name_fields[i]);
                if (!scalar_valid(name, false))
                    return fail(report, "CSL author field '%s' must be a string",
                                name_fields[i]);
            }
        }
    }

    value = object_field(item, "issued");
    if (value && !cJSON_IsNull(value)) {
        if (!cJSON_IsObject(value))
            return fail(report, "CSL field 'issued' must be an object");
        part = object_field(value, "date-parts");
        if (part && !cJSON_IsNull(part)) {
            if (!cJSON_IsArray(part))
                return fail(report, "CSL field 'issued.date-parts' must be an array");
            cJSON_ArrayForEach (component, part) {
                if (!cJSON_IsArray(component))
                    return fail(report, "CSL date-parts entries must be arrays");
                cJSON_ArrayForEach (person, component) {
                    if (!scalar_valid(person, true))
                        return fail(report, "CSL date-parts values must be strings or numbers");
                }
            }
        }
    }
    return true;
}

static char *scalar_copy(const cJSON *value, bool allow_number) {
    if (!value || cJSON_IsNull(value))
        return copy_string("");
    if (cJSON_IsString(value))
        return trim_copy(value->valuestring);
    if (allow_number && cJSON_IsNumber(value))
        return json_number_copy(value->valuedouble);
    return NULL;
}

static char *title_copy(const char *raw) {
    char *title = trim_copy(raw);
    if (!title)
        return NULL;
    if (!*title) {
        free(title);
        return copy_string(CE_ZOTERO_UNTITLED);
    }
    return title;
}

static bool append_author(TextBuffer *authors, const char *name) {
    if (!name || !*name)
        return true;
    if (authors->length)
        text_add_string(authors, "; ");
    return text_add_string(authors, name);
}

static char *csl_author(const cJSON *item) {
    const cJSON *author = object_field(item, "author");
    TextBuffer names = {0};
    char *result;
    if (author && cJSON_IsArray(author)) {
        const cJSON *person;
        cJSON_ArrayForEach (person, author) {
            const cJSON *literal_value = object_field(person, "literal");
            char *literal = literal_value ? scalar_copy(literal_value, false) : NULL;
            char *family = NULL, *given = NULL;
            bool ok = true;

            if (literal_value && !literal) {
                text_discard(&names);
                return NULL;
            }
            if (literal && *literal) {
                ok = append_author(&names, literal);
                free(literal);
                if (!ok) {
                    text_discard(&names);
                    return NULL;
                }
                continue;
            }
            free(literal);
            family = scalar_copy(object_field(person, "family"), false);
            given = scalar_copy(object_field(person, "given"), false);
            if (!family || !given) {
                free(family);
                free(given);
                text_discard(&names);
                return NULL;
            }
            trim_in_place(family);
            trim_in_place(given);
            if (*family || *given) {
                if (names.length)
                    text_add_string(&names, "; ");
                if (*family)
                    text_add_string(&names, family);
                if (*family && *given)
                    text_add_string(&names, " ");
                if (*given)
                    text_add_string(&names, given);
                if (names.failed) {
                    free(family);
                    free(given);
                    text_discard(&names);
                    return NULL;
                }
            }
            free(family);
            free(given);
        }
    }
    result = text_take(&names);
    if (!result)
        return NULL;
    if (!*result) {
        free(result);
        return copy_string(CE_ZOTERO_UNKNOWN_AUTHOR);
    }
    return result;
}

static char *csl_year(const cJSON *item) {
    const cJSON *issued = object_field(item, "issued");
    char *result = NULL;
    if (issued && cJSON_IsObject(issued)) {
        const cJSON *parts = object_field(issued, "date-parts");
        if (parts && cJSON_IsArray(parts)) {
            const cJSON *first = cJSON_GetArrayItem(parts, 0);
            if (first && cJSON_IsArray(first)) {
                const cJSON *year = cJSON_GetArrayItem(first, 0);
                result = scalar_copy(year, true);
                if (!result)
                    return NULL;
                if (*result)
                    return result;
                free(result);
            }
        }
    }
    return scalar_copy(object_field(item, "year"), true);
}

static char *csl_cite_key(const cJSON *item) {
    static const char *keys[] = {"citationKey", "citekey", "id"};
    size_t i;
    for (i = 0; i < sizeof keys / sizeof *keys; i++) {
        char *value = scalar_copy(object_field(item, keys[i]), true);
        if (!value)
            return NULL;
        if (*value)
            return value;
        free(value);
    }
    return copy_string("");
}

static bool zotero_key_valid(const char *key) {
    if (!key || !*key || strlen(key) != 8)
        return false;
    for (size_t i = 0; i < 8; i++)
        if (!((key[i] >= 'A' && key[i] <= 'Z') || (key[i] >= '0' && key[i] <= '9')))
            return false;
    return true;
}

/* CSL ids are arbitrary citation identifiers, not necessarily Zotero keys.
 * Only recognize a Zotero URI, or a bare eight-character Zotero-style key. */
static bool zotero_identity(const char *id, char library_path[64], const char **key) {
    const char *path = NULL, *items;
    if (!id || !*id)
        return false;
    if (zotero_key_valid(id)) {
        strcpy(library_path, "library");
        *key = id;
        return true;
    }
    const char *prefixes[] = {"zotero://select/", "zotero://open-pdf/",
                              "https://www.zotero.org/", "http://www.zotero.org/",
                              "https://zotero.org/", "http://zotero.org/"};
    for (size_t i = 0; i < sizeof prefixes / sizeof *prefixes; i++) {
        size_t n = strlen(prefixes[i]);
        if (!strncmp(id, prefixes[i], n)) {
            path = id + n;
            break;
        }
    }
    if (!path)
        return false;
    items = strstr(path, "/items/");
    if (!strncmp(path, "items/", 6)) {
        *key = path + 6;
        strcpy(library_path, "library");
    } else if (items && (!strncmp(path, "library/", 8) || !strncmp(path, "users/", 6))) {
        *key = items + 7;
        strcpy(library_path, "library");
    } else if (items && !strncmp(path, "groups/", 7)) {
        size_t n = (size_t)(items - path);
        if (n <= 7 || n >= 64)
            return false;
        for (const char *p = path + 7; p < items; p++)
            if (!ascii_digit((unsigned char)*p))
                return false;
        memcpy(library_path, path, n);
        library_path[n] = 0;
        *key = items + 7;
    } else {
        return false;
    }
    return zotero_key_valid(*key);
}

static char *zotero_uri(const char *action, const char *library_path, const char *key) {
    TextBuffer text = {0};
    if (*library_path && key && *key) {
        text_add_string(&text, "zotero://");
        text_add_string(&text, action);
        text_add_string(&text, "/");
        text_add_string(&text, library_path);
        text_add_string(&text, "/items/");
        text_add_string(&text, key);
    }
    return text_take(&text);
}

static bool set_csl_uris(CeZoteroItem *item, const cJSON *object, Report *report) {
    const cJSON *url_value = object_field(object, "URL");
    char *url = NULL, library_path[64] = "";
    const char *key = NULL;
    if (url_value && !cJSON_IsNull(url_value)) {
        url = scalar_copy(url_value, false);
        if (!url)
            return fail(report, "Out of memory");
    }
    bool recognized = zotero_identity(url, library_path, &key) ||
                      (cJSON_IsString(object_field(object, "id")) &&
                       zotero_identity(item->id, library_path, &key));
    char *item_key = copy_string(recognized ? key : "");
    item->item_uri = zotero_uri("select", recognized ? library_path : "", key);
    free(item->item_key);
    item->item_key = item_key;
    free(url);
    if (!item->item_uri || !item->item_key)
        return fail(report, "Out of memory");
    /* A parent bibliographic key is not a PDF attachment key. Only an explicit
     * attachment URI can supply that missing relationship in CSL input. */
    const cJSON *pdf = object_field(object, "pdfUri");
    const char *pdf_uri = cJSON_IsString(pdf) ? pdf->valuestring : "";
    recognized = !strncmp(pdf_uri, "zotero://open-pdf/", 18) &&
                 zotero_identity(pdf_uri, library_path, &key);
    item->pdf_uri = zotero_uri("open-pdf", recognized ? library_path : "", key);
    if (!item->pdf_uri)
        return fail(report, "Out of memory");
    return true;
}

static bool parse_csl_item(const cJSON *object, CeZoteroItem *item, Report *report) {
    char *raw_id = NULL, *cite_key = NULL, *raw_title = NULL, *year = NULL;
    bool ok = false;

    memset(item, 0, sizeof *item);
    raw_id = scalar_copy(object_field(object, "id"), true);
    cite_key = csl_cite_key(object);
    raw_title = scalar_copy(object_field(object, "title"), false);
    year = csl_year(object);
    if (!raw_id || !cite_key || !raw_title || !year)
        goto out;
    if (*raw_id) {
        item->id = raw_id;
        raw_id = NULL;
    } else {
        item->id = copy_string(cite_key);
        if (!item->id)
            goto out;
    }
    item->cite_key = cite_key;
    cite_key = NULL;
    item->item_key = copy_string("");
    item->title = title_copy(raw_title);
    item->author = csl_author(object);
    item->year = year;
    year = NULL;
    if (!item->item_key || !item->title || !item->author)
        goto out;
    if (!set_csl_uris(item, object, report))
        goto out;
    item->csl_json = print_json_owned(object);
    if (!item->csl_json)
        goto out;
    ok = true;
out:
    free(raw_id);
    free(cite_key);
    free(raw_title);
    free(year);
    if (!ok) {
        item_free(item);
        if (!report->error || !report->capacity || !report->error[0])
            return fail(report, "Out of memory");
        return false;
    }
    return true;
}

static bool contains_escaped_nul(const char *json) {
    const char *position;
    for (position = json; *position; position++) {
        if (*position != '\\')
            continue;
        if (position[1] == '\\') {
            position++;
            continue;
        }
        if (position[1] == 'u' && !strncmp(position + 2, "0000", 4))
            return true;
        if (position[1])
            position++;
    }
    return false;
}

bool ce_zotero_import_csl(const char *json, CeZoteroLibrary *library, char *error,
                          size_t error_capacity) {
    Report report = {error, error_capacity};
    CeZoteroLibrary temporary = {0};
    cJSON *root = NULL;
    size_t item_capacity = 0;
    const cJSON *element;

    clear_error(&report);
    if (!library)
        return fail(&report, "Invalid library output");
    if (!json)
        return fail(&report, "CSL JSON is required");
    if (!utf8_valid(json))
        return fail(&report, "CSL JSON is not valid UTF-8");
    if (contains_escaped_nul(json))
        return fail(&report, "Embedded NUL characters are not supported");
    root = cJSON_ParseWithOpts(json, NULL, 1);
    if (!root)
        return fail(&report, "Invalid CSL JSON");
    if (!cJSON_IsArray(root)) {
        fail(&report, "CSL JSON must be an array of objects");
        goto bad;
    }
    cJSON_ArrayForEach (element, root) {
        CeZoteroItem item;
        if (!validate_csl_item(element, &report))
            goto bad;
        if (!parse_csl_item(element, &item, &report))
            goto bad;
        if (!grow_array((void **)&temporary.items, &item_capacity, temporary.item_count + 1,
                        sizeof *temporary.items)) {
            item_free(&item);
            fail(&report, "Out of memory");
            goto bad;
        }
        temporary.items[temporary.item_count++] = item;
    }
    cJSON_Delete(root);
    ce_zotero_library_free(library);
    *library = temporary;
    clear_error(&report);
    return true;

bad:
    cJSON_Delete(root);
    ce_zotero_library_free(&temporary);
    return false;
}

/* ------------------------------------------------------------------------- */
/* SQLite loading                                                            */
/* ------------------------------------------------------------------------- */

typedef struct {
    bool items_library_id;
    bool collections_parent;
    bool collections_key;
    bool collections_library_id;
    bool deleted_items, deleted_collections, groups, libraries, attachments;
    bool item_creators_order;
    bool creator_roles;
    bool modern_creators;
    bool modern_creator_field_mode;
    bool legacy_creators;
    bool legacy_creator_field_mode;
} SqliteSchema;

static bool sqlite_error(Report *report, sqlite3 *database, const char *context) {
    const char *message = database ? sqlite3_errmsg(database) : "unknown SQLite error";
    return fail(report, "%s: %s", context, message ? message : "unknown SQLite error");
}

static bool prepare_sql(sqlite3 *database, const char *sql, sqlite3_stmt **statement,
                        Report *report) {
    if (sqlite3_prepare_v2(database, sql, -1, statement, NULL) != SQLITE_OK)
        return sqlite_error(report, database, "Cannot read Zotero database");
    return true;
}

static bool exec_sql(sqlite3 *database, const char *sql, Report *report) {
    char *message = NULL;
    int result = sqlite3_exec(database, sql, NULL, NULL, &message);
    if (result != SQLITE_OK) {
        fail(report, "SQLite error: %s", message ? message : sqlite3_errmsg(database));
        sqlite3_free(message);
        return false;
    }
    sqlite3_free(message);
    return true;
}

static bool table_exists(sqlite3 *database, const char *table, bool *exists, Report *report) {
    sqlite3_stmt *statement = NULL;
    int result;
    bool ok = false;
    *exists = false;
    if (!prepare_sql(database,
                     "SELECT 1 FROM sqlite_master WHERE type='table' AND name=?1", &statement,
                     report))
        return false;
    if (sqlite3_bind_text(statement, 1, table, -1, SQLITE_STATIC) != SQLITE_OK) {
        sqlite_error(report, database, "Cannot inspect Zotero schema");
        goto done;
    }
    result = sqlite3_step(statement);
    if (result == SQLITE_ROW)
        *exists = true;
    else if (result != SQLITE_DONE) {
        sqlite_error(report, database, "Cannot inspect Zotero schema");
        goto done;
    }
    ok = true;
done:
    sqlite3_finalize(statement);
    return ok;
}

static bool column_exists(sqlite3 *database, const char *table, const char *column,
                          bool *exists, Report *report) {
    char sql[160];
    sqlite3_stmt *statement = NULL;
    int result, written;
    bool ok = false;
    *exists = false;
    written = snprintf(sql, sizeof sql, "PRAGMA table_info(%s)", table);
    if (written < 0 || (size_t)written >= sizeof sql)
        return fail(report, "Zotero schema table name is too long");
    if (!prepare_sql(database, sql, &statement, report))
        return false;
    result = sqlite3_step(statement);
    while (result == SQLITE_ROW) {
        const unsigned char *name = sqlite3_column_text(statement, 1);
        if (name && !sqlite3_stricmp((const char *)name, column))
            *exists = true;
        result = sqlite3_step(statement);
    }
    if (result != SQLITE_DONE) {
        sqlite_error(report, database, "Cannot inspect Zotero schema");
        goto done;
    }
    ok = true;
done:
    sqlite3_finalize(statement);
    return ok;
}

static bool require_table(sqlite3 *database, const char *table, Report *report) {
    bool exists = false;
    if (!table_exists(database, table, &exists, report))
        return false;
    if (!exists)
        return fail(report, "Zotero schema error: missing table '%s'", table);
    return true;
}

static bool require_column(sqlite3 *database, const char *table, const char *column,
                           Report *report) {
    bool exists = false;
    if (!column_exists(database, table, column, &exists, report))
        return false;
    if (!exists)
        return fail(report, "Zotero schema error: missing column '%s.%s'", table, column);
    return true;
}

static bool optional_column(sqlite3 *database, const char *table, const char *column,
                            bool *exists, Report *report) {
    return column_exists(database, table, column, exists, report);
}

static bool inspect_schema(sqlite3 *database, SqliteSchema *schema, Report *report) {
    bool exists = false;
    static const char *required_tables[] = {
        "items",          "itemTypes", "fields",         "itemData",
        "itemDataValues", "creators",  "itemCreators",   "collections",
        "collectionItems"};
    size_t i;

    memset(schema, 0, sizeof *schema);
    for (i = 0; i < sizeof required_tables / sizeof *required_tables; i++)
        if (!require_table(database, required_tables[i], report))
            return false;

    if (!require_column(database, "items", "itemID", report) ||
        !require_column(database, "items", "itemTypeID", report) ||
        !require_column(database, "items", "key", report) ||
        !optional_column(database, "items", "libraryID", &schema->items_library_id, report))
        return false;
    if (!require_column(database, "itemTypes", "itemTypeID", report) ||
        !require_column(database, "itemTypes", "typeName", report))
        return false;
    if (!require_column(database, "fields", "fieldID", report) ||
        !require_column(database, "fields", "fieldName", report))
        return false;
    if (!require_column(database, "itemData", "itemID", report) ||
        !require_column(database, "itemData", "fieldID", report) ||
        !require_column(database, "itemData", "valueID", report))
        return false;
    if (!require_column(database, "itemDataValues", "valueID", report) ||
        !require_column(database, "itemDataValues", "value", report))
        return false;
    if (!require_column(database, "itemCreators", "itemID", report) ||
        !require_column(database, "itemCreators", "creatorID", report) ||
        !optional_column(database, "itemCreators", "orderIndex",
                         &schema->item_creators_order, report))
        return false;
    if (!require_column(database, "creators", "creatorID", report))
        return false;
    if (!table_exists(database, "creatorTypes", &exists, report))
        return false;
    if (exists) {
        if (!require_column(database, "creatorTypes", "creatorTypeID", report) ||
            !require_column(database, "creatorTypes", "creatorType", report) ||
            !optional_column(database, "itemCreators", "creatorTypeID", &schema->creator_roles, report))
            return false;
    }

    {
        bool first_name = false, last_name = false;
        if (!optional_column(database, "creators", "firstName", &first_name, report) ||
            !optional_column(database, "creators", "lastName", &last_name, report))
            return false;
        if (first_name || last_name) {
            if (!first_name || !last_name)
                return fail(report, "Zotero schema error: creators has incomplete modern columns");
            schema->modern_creators = true;
            if (!optional_column(database, "creators", "fieldMode",
                                 &schema->modern_creator_field_mode, report))
                return false;
        } else {
            bool legacy_reference = false;
            if (!optional_column(database, "creators", "creatorDataID", &legacy_reference,
                                 report))
                return false;
            if (!legacy_reference)
                return fail(report, "Zotero schema error: unsupported creators table");
            if (!require_table(database, "creatorData", report) ||
                !require_column(database, "creatorData", "creatorDataID", report) ||
                !require_column(database, "creatorData", "firstName", report) ||
                !require_column(database, "creatorData", "lastName", report) ||
                !optional_column(database, "creatorData", "fieldMode",
                                 &schema->legacy_creator_field_mode, report))
                return false;
            schema->legacy_creators = true;
        }
    }

    if (!require_column(database, "collections", "collectionID", report) ||
        !require_column(database, "collections", "collectionName", report) ||
        !optional_column(database, "collections", "parentCollectionID",
                         &schema->collections_parent, report) ||
        !optional_column(database, "collections", "key", &schema->collections_key, report) ||
        !optional_column(database, "collections", "libraryID", &schema->collections_library_id,
                         report))
        return false;
    if (!require_column(database, "collectionItems", "collectionID", report) ||
        !require_column(database, "collectionItems", "itemID", report))
        return false;

    if (!table_exists(database, "deletedItems", &exists, report))
        return false;
    if (exists) {
        schema->deleted_items = true;
        if (!require_column(database, "deletedItems", "itemID", report))
            return false;
    }
    if (!table_exists(database, "deletedCollections", &schema->deleted_collections, report) ||
        (schema->deleted_collections &&
         !require_column(database, "deletedCollections", "collectionID", report)))
        return false;
    if (!table_exists(database, "groups", &schema->groups, report) ||
        (schema->groups &&
         (!require_column(database, "groups", "libraryID", report) ||
          !require_column(database, "groups", "groupID", report))))
        return false;
    if (!table_exists(database, "libraries", &schema->libraries, report) ||
        (schema->libraries &&
         (!require_column(database, "libraries", "libraryID", report) ||
          !require_column(database, "libraries", "type", report))))
        return false;
    if (!table_exists(database, "itemAttachments", &schema->attachments, report))
        return false;
    if (schema->attachments &&
        (!require_column(database, "itemAttachments", "itemID", report) ||
         !require_column(database, "itemAttachments", "parentItemID", report) ||
         !require_column(database, "itemAttachments", "contentType", report) ||
         !require_column(database, "itemAttachments", "linkMode", report)))
        return false;
    return true;
}

static bool column_text(sqlite3_stmt *statement, int column, char **out, Report *report) {
    const unsigned char *text;
    int bytes;
    *out = NULL;
    text = sqlite3_column_text(statement, column);
    if (!text) {
        if (sqlite3_column_type(statement, column) != SQLITE_NULL)
            return fail(report, "SQLite could not decode text (out of memory)");
        *out = copy_string("");
        if (!*out)
            return fail(report, "Out of memory");
        return true;
    }
    bytes = sqlite3_column_bytes(statement, column);
    if (bytes < 0)
        return fail(report, "SQLite returned invalid text length");
    if (memchr(text, 0, (size_t)bytes))
        return fail(report, "Zotero text contains an embedded NUL byte");
    if (!utf8_valid_n((const char *)text, (size_t)bytes))
        return fail(report, "Zotero database contains invalid UTF-8 text");
    *out = copy_n((const char *)text, (size_t)bytes);
    if (!*out)
        return fail(report, "Out of memory");
    return true;
}

static char *year_from_date(const char *date) {
    size_t length, i;
    if (!date || !*date)
        return copy_string("");
    length = strlen(date);
    for (i = 0; i + 4 <= length; i++) {
        if (ascii_digit((unsigned char)date[i]) && ascii_digit((unsigned char)date[i + 1]) &&
            ascii_digit((unsigned char)date[i + 2]) && ascii_digit((unsigned char)date[i + 3]) &&
            (i == 0 || !ascii_digit((unsigned char)date[i - 1])) &&
            (i + 4 == length || !ascii_digit((unsigned char)date[i + 4])))
            return copy_n(date + i, 4);
    }
    return copy_string("");
}

static bool set_sqlite_uris(CeZoteroItem *item, const char *library_path,
                           const char *pdf_key, Report *report) {
    if (*library_path && item->item_key && *item->item_key) {
        TextBuffer uri = {0}, pdf = {0};
        text_add_string(&uri, "zotero://select/");
        text_add_string(&uri, library_path);
        text_add_string(&uri, "/items/");
        text_add_string(&uri, item->item_key);
        item->item_uri = text_take(&uri);
        if (pdf_key && *pdf_key) {
            text_add_string(&pdf, "zotero://open-pdf/");
            text_add_string(&pdf, library_path);
            text_add_string(&pdf, "/items/");
            text_add_string(&pdf, pdf_key);
        }
        item->pdf_uri = text_take(&pdf);
    } else {
        item->item_uri = copy_string("");
        item->pdf_uri = copy_string("");
    }
    if (!item->item_uri || !item->pdf_uri)
        return fail(report, "Out of memory");
    return true;
}

static bool find_item(const CeZoteroItem *items, size_t count, int64_t id, size_t *index) {
    size_t low = 0, high = count;
    while (low < high) {
        size_t middle = low + (high - low) / 2;
        if (items[middle].database_id < id)
            low = middle + 1;
        else
            high = middle;
    }
    if (low < count && items[low].database_id == id) {
        *index = low;
        return true;
    }
    return false;
}

static bool find_collection(const CeZoteroCollection *collections, size_t count, int64_t id,
                            size_t *index) {
    size_t low = 0, high = count;
    while (low < high) {
        size_t middle = low + (high - low) / 2;
        if (collections[middle].id < id)
            low = middle + 1;
        else
            high = middle;
    }
    if (low < count && collections[low].id == id) {
        *index = low;
        return true;
    }
    return false;
}

static bool load_items(sqlite3 *database, const SqliteSchema *schema, CeZoteroLibrary *library,
                       Report *report) {
    char sql[4096], attachment_query[1024];
    sqlite3_stmt *statement = NULL;
    size_t capacity = 0;
    int written, result;

    if (schema->attachments) {
        snprintf(attachment_query, sizeof attachment_query,
            "(SELECT a.key FROM itemAttachments att JOIN items a ON a.itemID=att.itemID "
            "WHERE att.parentItemID=i.itemID AND LOWER(att.contentType)='application/pdf' "
            "AND att.linkMode IN (0,1,2) %s %s ORDER BY a.itemID LIMIT 1)",
            schema->items_library_id ? "AND a.libraryID=i.libraryID" : "",
            schema->deleted_items ? "AND NOT EXISTS (SELECT 1 FROM deletedItems da WHERE da.itemID=a.itemID)" : "");
    } else {
        strcpy(attachment_query, "NULL");
    }
    written = snprintf(
        sql, sizeof sql,
        "SELECT i.itemID, %s, i.key, "
        "(SELECT v.value FROM itemData d "
        " JOIN fields f ON f.fieldID=d.fieldID "
        " JOIN itemDataValues v ON v.valueID=d.valueID "
        " WHERE d.itemID=i.itemID AND f.fieldName='title' ORDER BY v.valueID LIMIT 1), "
        "(SELECT v.value FROM itemData d "
        " JOIN fields f ON f.fieldID=d.fieldID "
        " JOIN itemDataValues v ON v.valueID=d.valueID "
        " WHERE d.itemID=i.itemID AND f.fieldName='date' ORDER BY v.valueID LIMIT 1), "
        "%s, %s, %s "
        "FROM items i JOIN itemTypes t ON t.itemTypeID=i.itemTypeID "
        "WHERE COALESCE(LOWER(t.typeName),'') NOT IN ('note','attachment','annotation') %s "
        "ORDER BY i.itemID",
        schema->items_library_id ? "COALESCE(i.libraryID,0)" : "0",
        schema->groups && schema->items_library_id
            ? "(SELECT g.groupID FROM groups g WHERE g.libraryID=i.libraryID)" : "NULL",
        schema->libraries && schema->items_library_id
            ? "(SELECT l.type FROM libraries l WHERE l.libraryID=i.libraryID)" : "NULL",
        attachment_query,
        schema->deleted_items
            ? "AND NOT EXISTS (SELECT 1 FROM deletedItems del WHERE del.itemID=i.itemID)"
            : "");
    if (written < 0 || (size_t)written >= sizeof sql)
        return fail(report, "Zotero item query is too long");
    if (!prepare_sql(database, sql, &statement, report))
        return false;
    result = sqlite3_step(statement);
    while (result == SQLITE_ROW) {
        CeZoteroItem item;
        char *raw_title = NULL, *raw_date = NULL, *library_type = NULL, *pdf_key = NULL;
        char library_path[64] = "";
        bool item_ok = true;

        memset(&item, 0, sizeof item);
        item.database_id = sqlite3_column_int64(statement, 0);
        item.library_id = sqlite3_column_int64(statement, 1);
        item.id = i64_copy(item.database_id);
        if (!item.id || !column_text(statement, 2, &item.item_key, report) ||
            !column_text(statement, 3, &raw_title, report) ||
            !column_text(statement, 4, &raw_date, report) ||
            !column_text(statement, 6, &library_type, report) ||
            !column_text(statement, 7, &pdf_key, report)) {
            item_ok = false;
        }
        if (item_ok) {
            item.title = title_copy(raw_title);
            item.year = year_from_date(raw_date);
            item.cite_key = copy_string(item.item_key);
            item.author = copy_string(CE_ZOTERO_UNKNOWN_AUTHOR);
            if (!item.title || !item.year || !item.cite_key || !item.author)
                item_ok = false;
        }
        free(raw_title);
        free(raw_date);
        if (item_ok) {
            if (sqlite3_column_type(statement, 5) != SQLITE_NULL) {
                int64_t group_id = sqlite3_column_int64(statement, 5);
                if (group_id > 0)
                    snprintf(library_path, sizeof library_path, "groups/%" PRId64, group_id);
            } else if (!strcmp(library_type, "user") ||
                       (!*library_type && item.library_id <= 1)) {
                strcpy(library_path, "library");
            }
        }
        if (item_ok && !set_sqlite_uris(&item, library_path, pdf_key, report))
            item_ok = false;
        free(library_type);
        free(pdf_key);
        if (item_ok &&
            !grow_array((void **)&library->items, &capacity, library->item_count + 1,
                        sizeof *library->items)) {
            fail(report, "Out of memory");
            item_ok = false;
        }
        if (!item_ok) {
            if (!report->error || !report->capacity || !report->error[0])
                fail(report, "Out of memory");
            item_free(&item);
            sqlite3_finalize(statement);
            return false;
        }
        library->items[library->item_count++] = item;
        result = sqlite3_step(statement);
    }
    if (result != SQLITE_DONE) {
        sqlite_error(report, database, "Cannot read Zotero items");
        sqlite3_finalize(statement);
        return false;
    }
    sqlite3_finalize(statement);
    return true;
}

static bool json_put(cJSON *object, const char *key, cJSON *value) {
    if (!object || !value) { cJSON_Delete(value); return false; }
    bool ok = cJSON_GetObjectItemCaseSensitive(object, key)
        ? cJSON_ReplaceItemInObjectCaseSensitive(object, key, value)
        : cJSON_AddItemToObject(object, key, value);
    if (!ok) cJSON_Delete(value);
    return ok;
}

static const ZoteroTypeMap *type_mapping(const char *type) {
    for (size_t i = 0; i < sizeof zotero_type_map / sizeof *zotero_type_map; ++i)
        if (!strcmp(type, zotero_type_map[i].zotero)) return &zotero_type_map[i];
    return NULL;
}

static const char *creator_mapping(const char *role, const char *type) {
    for (size_t i = 0; i < sizeof zotero_creator_map / sizeof *zotero_creator_map; ++i)
        if (!strcmp(role, zotero_creator_map[i].zotero)) return zotero_creator_map[i].csl;
    const ZoteroTypeMap *mapping = type_mapping(type);
    return mapping && !strcmp(role, mapping->primary) ? "author" : NULL;
}

static cJSON *metadata_date(const char *value) {
    cJSON *date = cJSON_CreateObject();
    size_t len = strlen(value), date_len = len;
    if (len > 10 && (value[10] == ' ' || value[10] == 'T')) date_len = 10;
    bool iso = date_len == 4 || date_len == 7 || date_len == 10;
    for (size_t i = 0; iso && i < date_len; ++i)
        if (i == 4 || i == 7) iso = value[i] == '-';
        else iso = ascii_digit((unsigned char)value[i]);
    int year = iso ? (value[0]-'0')*1000+(value[1]-'0')*100+(value[2]-'0')*10+value[3]-'0' : 0;
    int month = iso && date_len >= 7 ? (value[5]-'0')*10+value[6]-'0' : 0;
    int day = iso && date_len == 10 ? (value[8]-'0')*10+value[9]-'0' : 0;
    static const int month_days[] = {0,31,28,31,30,31,30,31,31,30,31,30,31};
    iso = iso && year > 0 && (date_len < 7 || (month >= 1 && month <= 12));
    if (iso && date_len == 10) {
        int days = month_days[month] + (month == 2 && year%4 == 0 && (year%100 != 0 || year%400 == 0));
        iso = day >= 1 && day <= days;
    }
    if (!date) return NULL;
    if (!iso) {
        /* Retain non-ISO dates for the CSL processor's date parser. The original
         * field is also retained verbatim in custom.zotero.fields. */
        if (cJSON_AddStringToObject(date, "raw", value)) return date;
    } else {
        int parts[] = {year, month, day};
        cJSON *array = cJSON_AddArrayToObject(date, "date-parts");
        cJSON *row = cJSON_CreateIntArray(parts, date_len == 4 ? 1 : date_len == 7 ? 2 : 3);
        if (array && row && cJSON_AddItemToArray(array, row)) return date;
        cJSON_Delete(row);
    }
    cJSON_Delete(date);
    return NULL;
}

static void metadata_free(cJSON **metadata, size_t count) {
    if (metadata) for (size_t i = 0; i < count; ++i) cJSON_Delete(metadata[i]);
    free(metadata);
}

static bool load_metadata(sqlite3 *database, CeZoteroLibrary *library,
                           cJSON ***output, Report *report) {
    *output = NULL;
    if (!library->item_count) return true;
    if (library->item_count > SIZE_MAX / sizeof(cJSON *)) return fail(report, "Out of memory");
    cJSON **metadata = calloc(library->item_count, sizeof *metadata);
    if (!metadata) return fail(report, "Out of memory");
    sqlite3_stmt *statement = NULL;
    bool success = false;
    if (!prepare_sql(database,
        "SELECT i.itemID,t.typeName,f.fieldName,v.value FROM items i "
        "JOIN itemTypes t ON t.itemTypeID=i.itemTypeID "
        "LEFT JOIN itemData d ON d.itemID=i.itemID "
        "LEFT JOIN fields f ON f.fieldID=d.fieldID "
        "LEFT JOIN itemDataValues v ON v.valueID=d.valueID ORDER BY i.itemID,d.fieldID",
        &statement, report)) goto done;
    int result;
    while ((result = sqlite3_step(statement)) == SQLITE_ROW) {
        size_t index;
        if (!find_item(library->items, library->item_count, sqlite3_column_int64(statement,0), &index)) continue;
        char *type = NULL, *name = NULL, *value = NULL;
        if (!column_text(statement,1,&type,report) || !column_text(statement,2,&name,report) ||
            !column_text(statement,3,&value,report)) { free(type); free(name); free(value); goto done; }
        cJSON *object = metadata[index];
        if (!object) {
            metadata[index] = object = cJSON_CreateObject();
            const ZoteroTypeMap *mapping = type_mapping(type);
            cJSON *custom = object ? cJSON_AddObjectToObject(object,"custom") : NULL;
            cJSON *zotero = custom ? cJSON_AddObjectToObject(custom,"zotero") : NULL;
            if (!object || !zotero || !cJSON_AddObjectToObject(zotero,"fields") ||
                !cJSON_AddStringToObject(zotero,"itemType",type) ||
                !cJSON_AddStringToObject(object,"id",library->items[index].id) ||
                !cJSON_AddStringToObject(object,"type",mapping ? mapping->csl : "document")) {
                free(type); free(name); free(value); goto oom;
            }
        }
        if (*name) {
            cJSON *fields = cJSON_GetObjectItemCaseSensitive(cJSON_GetObjectItemCaseSensitive(
                cJSON_GetObjectItemCaseSensitive(object,"custom"),"zotero"),"fields");
            if (!json_put(fields,name,cJSON_CreateString(value))) { free(type); free(name); free(value); goto oom; }
            for (size_t m = 0; *value && m < sizeof zotero_field_map / sizeof *zotero_field_map; ++m) {
                const ZoteroFieldMap *map = &zotero_field_map[m];
                if (strcmp(name,map->zotero) || (*map->type && strcmp(type,map->type))) continue;
                if (cJSON_GetObjectItemCaseSensitive(object,map->csl) && !*map->type) continue;
                cJSON *field = map->date ? metadata_date(value) : cJSON_CreateString(value);
                if (!json_put(object,map->csl,field)) { free(type); free(name); free(value); goto oom; }
                char **display = !strcmp(map->csl,"title") ? &library->items[index].title
                    : !strcmp(map->csl,"issued") ? &library->items[index].year
                    : !strcmp(map->csl,"citation-key") ? &library->items[index].cite_key : NULL;
                if (display) {
                    char *next = !strcmp(map->csl,"issued") ? year_from_date(value) : trim_copy(value);
                    if (!next) { free(type); free(name); free(value); goto oom; }
                    free(*display); *display = next;
                }
            }
        }
        free(type); free(name); free(value);
    }
    if (result != SQLITE_DONE) { sqlite_error(report,database,"Cannot read Zotero metadata"); goto done; }
    success = true;
    *output = metadata;
    goto done;
oom: fail(report,"Out of memory while reading Zotero metadata");
done:
    sqlite3_finalize(statement);
    if (!success) metadata_free(metadata,library->item_count);
    return success;
}

static bool load_creators(sqlite3 *database, const SqliteSchema *schema,
                          CeZoteroLibrary *library, cJSON **metadata, Report *report) {
    char sql[1024];
    sqlite3_stmt *statement = NULL;
    TextBuffer *authors = NULL;
    size_t i;
    int written, result;
    bool ok = false;

    if (!library->item_count)
        return true;
    if (library->item_count > SIZE_MAX / sizeof *authors)
        return fail(report, "Out of memory");
    authors = (TextBuffer *)calloc(library->item_count, sizeof *authors);
    if (!authors)
        return fail(report, "Out of memory");

    if (schema->modern_creators) {
        written = snprintf(
            sql, sizeof sql,
            "SELECT ic.itemID, c.firstName, c.lastName, %s, %s, t.typeName "
            "FROM itemCreators ic JOIN creators c ON c.creatorID=ic.creatorID "
            "JOIN items i ON i.itemID=ic.itemID JOIN itemTypes t ON t.itemTypeID=i.itemTypeID %s "
            "ORDER BY ic.itemID, %s",
            schema->modern_creator_field_mode ? "COALESCE(c.fieldMode,0)" : "0",
            schema->creator_roles ? "ct.creatorType" : "'author'",
            schema->creator_roles ? "LEFT JOIN creatorTypes ct ON ct.creatorTypeID=ic.creatorTypeID" : "",
            schema->item_creators_order ? "ic.orderIndex, ic.creatorID" : "ic.creatorID");
    } else {
        written = snprintf(
            sql, sizeof sql,
            "SELECT ic.itemID, cd.firstName, cd.lastName, %s, %s, t.typeName "
            "FROM itemCreators ic "
            "JOIN creators c ON c.creatorID=ic.creatorID "
            "JOIN creatorData cd ON cd.creatorDataID=c.creatorDataID "
            "JOIN items i ON i.itemID=ic.itemID JOIN itemTypes t ON t.itemTypeID=i.itemTypeID %s "
            "ORDER BY ic.itemID, %s",
            schema->legacy_creator_field_mode ? "COALESCE(cd.fieldMode,0)" : "0",
            schema->creator_roles ? "ct.creatorType" : "'author'",
            schema->creator_roles ? "LEFT JOIN creatorTypes ct ON ct.creatorTypeID=ic.creatorTypeID" : "",
            schema->item_creators_order ? "ic.orderIndex, ic.creatorID" : "ic.creatorID");
    }
    if (written < 0 || (size_t)written >= sizeof sql) {
        fail(report, "Zotero creator query is too long");
        goto done;
    }
    if (!prepare_sql(database, sql, &statement, report))
        goto done;
    result = sqlite3_step(statement);
    while (result == SQLITE_ROW) {
        int64_t item_id = sqlite3_column_int64(statement, 0);
        int mode = sqlite3_column_int(statement, 3);
        size_t item_index;
        char *first = NULL, *last = NULL, *role = NULL, *type = NULL;
        if (!find_item(library->items, library->item_count, item_id, &item_index)) {
            result = sqlite3_step(statement);
            continue;
        }
        if (!column_text(statement, 1, &first, report) ||
            !column_text(statement, 2, &last, report) ||
            !column_text(statement, 4, &role, report) ||
            !column_text(statement, 5, &type, report) || !first || !last) {
            free(first);
            free(last);
            free(role); free(type);
            goto done;
        }
        trim_in_place(first);
        trim_in_place(last);
        cJSON *zotero = cJSON_GetObjectItemCaseSensitive(cJSON_GetObjectItemCaseSensitive(
            metadata[item_index],"custom"),"zotero");
        cJSON *raw_names = cJSON_GetObjectItemCaseSensitive(zotero,"creators");
        if (!raw_names) raw_names = cJSON_AddArrayToObject(zotero,"creators");
        cJSON *raw_name = cJSON_CreateObject();
        if (!raw_name || !raw_names || !cJSON_AddStringToObject(raw_name,"firstName",first) ||
            !cJSON_AddStringToObject(raw_name,"lastName",last) ||
            !cJSON_AddStringToObject(raw_name,"creatorType",role) ||
            !cJSON_AddNumberToObject(raw_name,"fieldMode",mode) ||
            !cJSON_AddItemToArray(raw_names,raw_name)) {
            cJSON_Delete(raw_name); free(first); free(last); free(role); free(type);
            fail(report,"Out of memory while retaining creator metadata"); goto done;
        }
        const char *mapped = creator_mapping(role,type);
        if (mapped && (*first || *last)) {
            cJSON *names = cJSON_GetObjectItemCaseSensitive(metadata[item_index],mapped);
            if (!names) names = cJSON_AddArrayToObject(metadata[item_index],mapped);
            cJSON *name = cJSON_CreateObject();
            bool valid = name && (mode == 1
                ? cJSON_AddStringToObject(name,"literal",*last ? last : first) != NULL
                : cJSON_AddStringToObject(name,"family",last) && cJSON_AddStringToObject(name,"given",first));
            if (!valid || !names || !cJSON_AddItemToArray(names,name)) {
                cJSON_Delete(name); free(first); free(last); free(role); free(type);
                fail(report,"Out of memory while reading structured creators"); goto done;
            }
        }
        /* Editors/translators belong to distinct CSL roles, never the author
         * display string or approximate author field. */
        bool is_author = mapped && !strcmp(mapped,"author");
        free(role); free(type);
        if (!is_author) { free(first); free(last); result = sqlite3_step(statement); continue; }
        if (mode == 1) {
            const char *literal = *last ? last : first;
            if (*literal) {
                if (authors[item_index].length)
                    text_add_string(&authors[item_index], "; ");
                text_add_string(&authors[item_index], literal);
            }
        } else if (*last || *first) {
            if (authors[item_index].length)
                text_add_string(&authors[item_index], "; ");
            if (*last)
                text_add_string(&authors[item_index], last);
            if (*last && *first)
                text_add_string(&authors[item_index], " ");
            if (*first)
                text_add_string(&authors[item_index], first);
        }
        free(first);
        free(last);
        if (authors[item_index].failed) {
            fail(report, "Out of memory");
            goto done;
        }
        result = sqlite3_step(statement);
    }
    if (result != SQLITE_DONE) {
        sqlite_error(report, database, "Cannot read Zotero creators");
        goto done;
    }
    for (i = 0; i < library->item_count; i++) {
        if (authors[i].length) {
            char *author = text_take(&authors[i]);
            if (!author) {
                fail(report, "Out of memory");
                goto done;
            }
            free(library->items[i].author);
            library->items[i].author = author;
        }
    }
    ok = true;
done:
    sqlite3_finalize(statement);
    if (authors) {
        for (i = 0; i < library->item_count; i++)
            text_discard(&authors[i]);
        free(authors);
    }
    return ok;
}

static bool load_collections(sqlite3 *database, const SqliteSchema *schema,
                             CeZoteroLibrary *library, Report *report) {
    char sql[512];
    sqlite3_stmt *statement = NULL;
    size_t capacity = 0;
    int written, result;

    written = snprintf(
        sql, sizeof sql,
        "SELECT collectionID, collectionName, %s, %s, %s FROM collections c %s "
        "ORDER BY collectionID",
        schema->collections_parent ? "parentCollectionID" : "NULL",
        schema->collections_key ? "key" : "NULL",
        schema->collections_library_id ? "COALESCE(libraryID,0)" : "0",
        schema->deleted_collections
            ? "WHERE NOT EXISTS (SELECT 1 FROM deletedCollections dc WHERE dc.collectionID=c.collectionID)"
            : "");
    if (written < 0 || (size_t)written >= sizeof sql)
        return fail(report, "Zotero collection query is too long");
    if (!prepare_sql(database, sql, &statement, report))
        return false;
    result = sqlite3_step(statement);
    while (result == SQLITE_ROW) {
        CeZoteroCollection collection;
        bool collection_ok = true;
        memset(&collection, 0, sizeof collection);
        collection.id = sqlite3_column_int64(statement, 0);
        collection.parent_index = SIZE_MAX;
        collection.library_id = sqlite3_column_int64(statement, 4);
        if (sqlite3_column_type(statement, 2) != SQLITE_NULL) {
            collection.has_parent = true;
            collection.parent_id = sqlite3_column_int64(statement, 2);
        }
        if (!column_text(statement, 1, &collection.name, report) ||
            !column_text(statement, 3, &collection.key, report))
            collection_ok = false;
        if (collection_ok &&
            !grow_array((void **)&library->collections, &capacity,
                        library->collection_count + 1, sizeof *library->collections)) {
            fail(report, "Out of memory");
            collection_ok = false;
        }
        if (!collection_ok) {
            free(collection.name);
            free(collection.key);
            sqlite3_finalize(statement);
            return false;
        }
        library->collections[library->collection_count++] = collection;
        result = sqlite3_step(statement);
    }
    if (result != SQLITE_DONE) {
        sqlite_error(report, database, "Cannot read Zotero collections");
        sqlite3_finalize(statement);
        return false;
    }
    sqlite3_finalize(statement);
    return true;
}

static int compare_size_t(const void *left, const void *right) {
    size_t a = *(const size_t *)left, b = *(const size_t *)right;
    return a < b ? -1 : a > b ? 1 : 0;
}

static bool load_collection_items(sqlite3 *database, CeZoteroLibrary *library, Report *report) {
    sqlite3_stmt *statement = NULL;
    size_t *capacities = NULL;
    int result;
    bool ok = false;
    size_t i;

    if (!library->collection_count)
        return true;
    if (library->collection_count > SIZE_MAX / sizeof *capacities)
        return fail(report, "Out of memory");
    capacities = (size_t *)calloc(library->collection_count, sizeof *capacities);
    if (!capacities)
        return fail(report, "Out of memory");
    if (!prepare_sql(database,
                     "SELECT collectionID, itemID FROM collectionItems "
                     "ORDER BY collectionID, itemID",
                     &statement, report))
        goto done;
    result = sqlite3_step(statement);
    while (result == SQLITE_ROW) {
        int64_t collection_id = sqlite3_column_int64(statement, 0);
        int64_t item_id = sqlite3_column_int64(statement, 1);
        size_t collection_index, item_index;
        CeZoteroCollection *collection;
        if (!find_collection(library->collections, library->collection_count, collection_id,
                             &collection_index) ||
            !find_item(library->items, library->item_count, item_id, &item_index)) {
            result = sqlite3_step(statement);
            continue;
        }
        collection = &library->collections[collection_index];
        if (collection->library_id != library->items[item_index].library_id) {
            result = sqlite3_step(statement);
            continue;
        }
        if (!grow_array((void **)&collection->item_indices,
                        &capacities[collection_index], collection->item_count + 1,
                        sizeof *collection->item_indices)) {
            fail(report, "Out of memory");
            goto done;
        }
        collection->item_indices[collection->item_count++] = item_index;
        result = sqlite3_step(statement);
    }
    if (result != SQLITE_DONE) {
        sqlite_error(report, database, "Cannot read Zotero collection membership");
        goto done;
    }
    for (i = 0; i < library->collection_count; i++) {
        CeZoteroCollection *collection = &library->collections[i];
        size_t write = 0, read;
        if (collection->item_count > 1)
            qsort(collection->item_indices, collection->item_count,
                  sizeof *collection->item_indices, compare_size_t);
        for (read = 0; read < collection->item_count; read++) {
            if (!write || collection->item_indices[read] != collection->item_indices[write - 1])
                collection->item_indices[write++] = collection->item_indices[read];
        }
        collection->item_count = write;
    }
    ok = true;
done:
    sqlite3_finalize(statement);
    free(capacities);
    return ok;
}

static bool link_collections(CeZoteroLibrary *library, Report *report) {
    size_t count = library->collection_count;
    size_t *child_counts = NULL, *fill = NULL;
    size_t i, roots = 0;

    library->repaired_parents = 0;
    if (!count)
        return true;

    for (i = 0; i < count; i++) {
        CeZoteroCollection *collection = &library->collections[i];
        size_t parent;
        if (!collection->has_parent) {
            collection->parent_index = SIZE_MAX;
            continue;
        }
        if (!find_collection(library->collections, count, collection->parent_id, &parent)) {
            collection->parent_index = SIZE_MAX;
            library->repaired_parents++;
            continue;
        }
        if (library->collections[parent].library_id != collection->library_id) {
            collection->parent_index = SIZE_MAX;
            library->repaired_parents++;
            continue;
        }
        collection->parent_index = parent;
    }

    /* Break each cycle at its lowest collection index. All preliminary parent
     * links are installed before this pass, so the traversal sees the whole
     * graph and the result is independent of SQL row order. */
    for (i = 0; i < count; i++) {
        CeZoteroCollection *collection = &library->collections[i];
        size_t current;
        if (collection->parent_index == SIZE_MAX)
            continue;
        current = collection->parent_index;
        for (size_t steps = 0; steps < count; steps++) {
            if (current == i) {
                collection->parent_index = SIZE_MAX;
                library->repaired_parents++;
                break;
            }
            if (current == SIZE_MAX || current >= count)
                break;
            current = library->collections[current].parent_index;
        }
    }

    child_counts = (size_t *)calloc(count, sizeof *child_counts);
    fill = (size_t *)calloc(count, sizeof *fill);
    if (!child_counts || !fill) {
        free(child_counts);
        free(fill);
        return fail(report, "Out of memory");
    }
    for (i = 0; i < count; i++) {
        size_t parent = library->collections[i].parent_index;
        if (parent != SIZE_MAX && parent < count)
            child_counts[parent]++;
    }
    for (i = 0; i < count; i++) {
        if (child_counts[i]) {
            if (child_counts[i] > SIZE_MAX / sizeof(size_t)) {
                free(child_counts);
                free(fill);
                return fail(report, "Out of memory");
            }
            library->collections[i].children =
                (size_t *)malloc(child_counts[i] * sizeof *library->collections[i].children);
            if (!library->collections[i].children) {
                free(child_counts);
                free(fill);
                return fail(report, "Out of memory");
            }
            library->collections[i].child_count = child_counts[i];
        }
    }
    for (i = 0; i < count; i++) {
        size_t parent = library->collections[i].parent_index;
        if (parent != SIZE_MAX && parent < count)
            library->collections[parent].children[fill[parent]++] = i;
    }
    for (i = 0; i < count; i++)
        if (library->collections[i].parent_index == SIZE_MAX)
            roots++;
    if (roots) {
        size_t cursor = 0;
        if (roots > SIZE_MAX / sizeof *library->roots) {
            free(child_counts);
            free(fill);
            return fail(report, "Out of memory");
        }
        library->roots = (size_t *)malloc(roots * sizeof *library->roots);
        if (!library->roots) {
            free(child_counts);
            free(fill);
            return fail(report, "Out of memory");
        }
        for (i = 0; i < count; i++)
            if (library->collections[i].parent_index == SIZE_MAX)
                library->roots[cursor++] = i;
        library->root_count = roots;
    }
    free(child_counts);
    free(fill);
    return true;
}

/* Takes ownership of database on every path. Both import routes use the same
 * consistent transaction and only replace the output after a complete read. */
static bool load_database(sqlite3 *database, CeZoteroLibrary *library,
                          char *error, size_t error_capacity) {
    Report report = {error, error_capacity};
    CeZoteroLibrary temporary = {0};
    SqliteSchema schema;
    cJSON **metadata = NULL;
    bool transaction = false, success = false;

    sqlite3_db_config(database, SQLITE_DBCONFIG_DEFENSIVE, 1, NULL);
    sqlite3_db_config(database, SQLITE_DBCONFIG_TRUSTED_SCHEMA, 0, NULL);
    if (!exec_sql(database, "PRAGMA query_only=ON", &report))
        goto done;
    if (!exec_sql(database, "BEGIN DEFERRED", &report))
        goto done;
    transaction = true;
    if (!inspect_schema(database, &schema, &report) || !load_items(database, &schema, &temporary, &report) ||
        !load_metadata(database, &temporary, &metadata, &report) ||
        !load_creators(database, &schema, &temporary, metadata, &report) ||
        !load_collections(database, &schema, &temporary, &report) ||
        !load_collection_items(database, &temporary, &report) ||
        !link_collections(&temporary, &report))
        goto done;
    if (temporary.item_count && !metadata) {
        fail(&report,"Missing bibliographic metadata");
        goto done;
    }
    for (size_t i = 0; i < temporary.item_count; ++i) {
        temporary.items[i].csl_json = print_json_owned(metadata[i]);
        if (!temporary.items[i].csl_json) { fail(&report,"Out of memory"); goto done; }
    }
    if (!exec_sql(database, "COMMIT", &report))
        goto done;
    transaction = false;
    success = true;

done:
    metadata_free(metadata, temporary.item_count);
    if (transaction)
        (void)sqlite3_exec(database, "ROLLBACK", NULL, NULL, NULL);
    if (database)
        sqlite3_close(database);
    if (!success) {
        ce_zotero_library_free(&temporary);
        return false;
    }
    ce_zotero_library_free(library);
    *library = temporary;
    clear_error(&report);
    return true;
}

bool ce_zotero_load_sqlite(const char *path, CeZoteroLibrary *library, char *error,
                           size_t error_capacity) {
    Report report = {error, error_capacity};
    sqlite3 *database = NULL;
    clear_error(&report);
    if (!library)
        return fail(&report, "Invalid library output");
    if (!path || !*path)
        return fail(&report, "SQLite database path is required");
    if (!utf8_valid(path))
        return fail(&report, "SQLite database path must be valid UTF-8");
    int result = sqlite3_open_v2(path, &database, SQLITE_OPEN_READONLY, NULL);
    if (result != SQLITE_OK) {
        fail(&report, "Cannot open Zotero database read-only: %s",
             database ? sqlite3_errmsg(database) : sqlite3_errstr(result));
        sqlite3_close(database);
        return false;
    }
    return load_database(database, library, error, error_capacity);
}

bool ce_zotero_load_sqlite_bytes(const void *data, size_t size, CeZoteroLibrary *library,
                                 char *error, size_t error_capacity) {
    Report report = {error, error_capacity};
    clear_error(&report);
    if (!library)
        return fail(&report, "Invalid library output");
    if (!data || size < 100 || memcmp(data, "SQLite format 3\0", 16))
        return fail(&report, "请选择完整的 SQLite 数据库或 CSL JSON 文件");
    if (size > (size_t)128 * 1024 * 1024)
        return fail(&report, "导入文件超过 128 MiB；请导出所需文献的 CSL JSON");
    const unsigned char *bytes = data;
    if (bytes[18] != 1 || bytes[19] != 1)
        return fail(&report, "此数据库可能依赖单独的 WAL 日志；请使用桌面原始路径，或导出 CSL JSON / 完整 SQLite 备份");
    sqlite3 *database = NULL;
    int result = sqlite3_open_v2(":memory:", &database,
                                SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_MEMORY, NULL);
    if (result != SQLITE_OK) {
        fail(&report, "Cannot open SQLite snapshot: %s",
             database ? sqlite3_errmsg(database) : sqlite3_errstr(result));
        sqlite3_close(database);
        return false;
    }
    /* SQLite recommends 20 padding bytes for untrusted serialized databases.
     * We own the copy through close, including when deserialize fails. */
    unsigned char *copy = sqlite3_malloc64((sqlite3_uint64)size + 20);
    if (!copy) {
        sqlite3_close(database);
        return fail(&report, "Out of memory while importing SQLite snapshot");
    }
    memcpy(copy, bytes, size);
    memset(copy + size, 0, 20);
    result = sqlite3_deserialize(database, "main", copy, (sqlite3_int64)size,
                                (sqlite3_int64)size + 20, SQLITE_DESERIALIZE_READONLY);
    bool success;
    if (result != SQLITE_OK) {
        success = fail(&report, "Cannot read SQLite snapshot: %s", sqlite3_errmsg(database));
        sqlite3_close(database);
    } else {
        success = load_database(database, library, error, error_capacity);
    }
    sqlite3_free(copy);
    return success;
}

typedef struct { const char *name; size_t offset; } CacheString;
#define CACHE_STRING(member) {#member, offsetof(CeZoteroItem, member)}
static const CacheString cache_item_strings[] = {
    CACHE_STRING(id), CACHE_STRING(item_key), CACHE_STRING(title), CACHE_STRING(author),
    CACHE_STRING(year), CACHE_STRING(cite_key), CACHE_STRING(item_uri), CACHE_STRING(pdf_uri),
    CACHE_STRING(csl_json)
};
#undef CACHE_STRING

static bool cache_integer_write(cJSON *object, const char *name, int64_t value) {
    char digits[32]; snprintf(digits,sizeof digits,"%" PRId64,value);
    return cJSON_AddStringToObject(object,name,digits) != NULL;
}
static bool cache_integer_read(cJSON *object, const char *name, int64_t *value) {
    cJSON *field = cJSON_GetObjectItemCaseSensitive(object,name);
    if (!cJSON_IsString(field) || !*field->valuestring) return false;
    const char *digits=field->valuestring;
    if (*digits!='-' && !ascii_digit((unsigned char)*digits)) return false;
    char *end; errno=0;
    intmax_t parsed=strtoimax(digits,&end,10);
    if (errno || *end || parsed<INT64_MIN || parsed>INT64_MAX) return false;
    *value=(int64_t)parsed; return true;
}
char *ce_zotero_cache_export(const CeZoteroLibrary *library, char *error, size_t capacity) {
    Report report={error,capacity}; clear_error(&report);
    if (!library || (library->item_count && !library->items) ||
        (library->collection_count && !library->collections) ||
        library->item_count>INT_MAX || library->collection_count>INT_MAX) {
        fail(&report,"Invalid library cache source"); return NULL;
    }
    cJSON *root=cJSON_CreateObject();
    cJSON *items=root?cJSON_AddArrayToObject(root,"items"):NULL;
    cJSON *collections=root?cJSON_AddArrayToObject(root,"collections"):NULL;
    char *result=NULL;
    if (!root || !items || !collections || !cJSON_AddNumberToObject(root,"version",1)) goto oom;
    for (size_t i=0;i<library->item_count;++i) {
        const CeZoteroItem *item=&library->items[i];
        cJSON *row=cJSON_CreateObject();
        if (!row || !cJSON_AddItemToArray(items,row)) { cJSON_Delete(row); goto oom; }
        if (!cache_integer_write(row,"database_id",item->database_id) ||
            !cache_integer_write(row,"library_id",item->library_id)) goto oom;
        for (size_t s=0;s<sizeof cache_item_strings/sizeof *cache_item_strings;++s) {
            const CacheString *member=&cache_item_strings[s];
            char *const *value=(char *const *)((const char *)item+member->offset);
            cJSON *field=*value?cJSON_CreateString(*value):cJSON_CreateNull();
            if (!json_put(row,member->name,field)) goto oom;
        }
    }
    for (size_t i=0;i<library->collection_count;++i) {
        const CeZoteroCollection *collection=&library->collections[i];
        if (i && collection->id<=library->collections[i-1].id) {
            fail(&report,"Invalid cached collection order"); goto done;
        }
        cJSON *row=cJSON_CreateObject();
        if (!row || !cJSON_AddItemToArray(collections,row)) { cJSON_Delete(row); goto oom; }
        if (!cache_integer_write(row,"id",collection->id) ||
            !cache_integer_write(row,"library_id",collection->library_id) ||
            !cache_integer_write(row,"parent_id",collection->parent_id) ||
            !cJSON_AddBoolToObject(row,"has_parent",collection->has_parent) ||
            !cJSON_AddStringToObject(row,"key",collection->key?collection->key:"") ||
            !cJSON_AddStringToObject(row,"name",collection->name?collection->name:"")) goto oom;
        cJSON *members=cJSON_AddArrayToObject(row,"items");
        if (!members) goto oom;
        for (size_t j=0;j<collection->item_count;++j) {
            if (!collection->item_indices || collection->item_indices[j]>=library->item_count ||
                collection->item_indices[j]>INT_MAX ||
                library->items[collection->item_indices[j]].library_id!=collection->library_id ||
                (j && collection->item_indices[j]<=collection->item_indices[j-1])) {
                fail(&report,"Invalid cached collection membership"); goto done;
            }
            cJSON *member=cJSON_CreateNumber((double)collection->item_indices[j]);
            if (!member || !cJSON_AddItemToArray(members,member)) { cJSON_Delete(member); goto oom; }
        }
    }
    result=print_json_owned(root);
    if (!result) goto oom;
    goto done;
oom: fail(&report,"Out of memory while serializing library cache");
done: cJSON_Delete(root); return result;
}

bool ce_zotero_cache_import(const char *json, CeZoteroLibrary *library, char *error, size_t capacity) {
    Report report={error,capacity}; clear_error(&report);
    if (!library || !json || !utf8_valid(json) || contains_escaped_nul(json))
        return fail(&report,"Invalid UTF-8 library cache");
    cJSON *root=cJSON_ParseWithOpts(json,NULL,true);
    cJSON *version=cJSON_GetObjectItemCaseSensitive(root,"version");
    cJSON *items=cJSON_GetObjectItemCaseSensitive(root,"items");
    cJSON *collections=cJSON_GetObjectItemCaseSensitive(root,"collections");
    CeZoteroLibrary next={0}; bool success=false;
    if (!cJSON_IsObject(root) || !cJSON_IsNumber(version) || version->valuedouble!=1 ||
        !cJSON_IsArray(items) || !cJSON_IsArray(collections)) goto malformed;
    size_t item_count=(size_t)cJSON_GetArraySize(items);
    size_t collection_count=(size_t)cJSON_GetArraySize(collections);
    if (item_count>SIZE_MAX/sizeof *next.items || collection_count>SIZE_MAX/sizeof *next.collections) goto oom;
    if (item_count && !(next.items=calloc(item_count,sizeof *next.items))) goto oom;
    next.item_count=item_count;
    if (collection_count && !(next.collections=calloc(collection_count,sizeof *next.collections))) goto oom;
    next.collection_count=collection_count;
    cJSON *row=items->child;
    for (size_t i=0;i<next.item_count;++i, row=row->next) {
        CeZoteroItem *item=&next.items[i];
        if (!cJSON_IsObject(row) || !cache_integer_read(row,"database_id",&item->database_id) ||
            !cache_integer_read(row,"library_id",&item->library_id)) goto malformed;
        for (size_t s=0;s<sizeof cache_item_strings/sizeof *cache_item_strings;++s) {
            const CacheString *member=&cache_item_strings[s];
            cJSON *value=cJSON_GetObjectItemCaseSensitive(row,member->name);
            if (cJSON_IsNull(value)) continue;
            if (!cJSON_IsString(value)) goto malformed;
            char **target=(char **)((char *)item+member->offset);
            *target=copy_string(value->valuestring); if (!*target) goto oom;
        }
        /* Fields used by the UI have always been strings; normalize a missing
         * optional input to empty instead of letting a corrupt cache crash it. */
        for (size_t s=0;s+1<sizeof cache_item_strings/sizeof *cache_item_strings;++s) {
            char **target=(char **)((char *)item+cache_item_strings[s].offset);
            if (!*target && !(*target=copy_string(""))) goto oom;
        }
        if (item->csl_json) {
            cJSON *csl=contains_escaped_nul(item->csl_json)?NULL:cJSON_ParseWithOpts(item->csl_json,NULL,true);
            bool valid=csl && validate_csl_item(csl,&report);
            cJSON_Delete(csl); if (!valid) goto malformed;
        }
    }
    row=collections->child;
    for (size_t i=0;i<next.collection_count;++i, row=row->next) {
        CeZoteroCollection *collection=&next.collections[i];
        cJSON *parent=cJSON_GetObjectItemCaseSensitive(row,"has_parent");
        cJSON *key=cJSON_GetObjectItemCaseSensitive(row,"key");
        cJSON *name=cJSON_GetObjectItemCaseSensitive(row,"name");
        cJSON *members=cJSON_GetObjectItemCaseSensitive(row,"items");
        if (!cJSON_IsObject(row) || !cJSON_IsBool(parent) || !cJSON_IsString(key) ||
            !cJSON_IsString(name) || !cJSON_IsArray(members) ||
            !cache_integer_read(row,"id",&collection->id) ||
            !cache_integer_read(row,"library_id",&collection->library_id) ||
            !cache_integer_read(row,"parent_id",&collection->parent_id) ||
            (i && collection->id<=next.collections[i-1].id)) goto malformed;
        collection->has_parent=cJSON_IsTrue(parent);
        collection->key=copy_string(key->valuestring); collection->name=copy_string(name->valuestring);
        if (!collection->key || !collection->name) goto oom;
        collection->item_count=(size_t)cJSON_GetArraySize(members);
        if (collection->item_count>SIZE_MAX/sizeof *collection->item_indices) goto oom;
        if (collection->item_count && !(collection->item_indices=malloc(collection->item_count*sizeof *collection->item_indices))) goto oom;
        cJSON *member=members->child;
        for (size_t j=0;j<collection->item_count;++j, member=member->next) {
            if (!cJSON_IsNumber(member) || member->valueint<0 || member->valuedouble!=member->valueint ||
                (size_t)member->valueint>=next.item_count ||
                next.items[member->valueint].library_id!=collection->library_id ||
                (j && (size_t)member->valueint<=collection->item_indices[j-1])) goto malformed;
            collection->item_indices[j]=(size_t)member->valueint;
        }
    }
    if (!link_collections(&next,&report)) goto done;
    ce_zotero_library_free(library); *library=next; memset(&next,0,sizeof next);
    success=true; clear_error(&report); goto done;
malformed: fail(&report,"Library cache is malformed or uses an unsupported version; original library retained"); goto done;
oom: fail(&report,"Out of memory while loading library cache");
done: cJSON_Delete(root); ce_zotero_library_free(&next); return success;
}

/* ------------------------------------------------------------------------- */
/* Collection item filtering                                                 */
/* ------------------------------------------------------------------------- */

bool ce_zotero_collection_items(const CeZoteroLibrary *library, size_t collection,
                                bool include_descendants, size_t **indices, size_t *count,
                                char *error, size_t error_capacity) {
    Report report = {error, error_capacity};
    size_t *result = NULL, result_count = 0, result_capacity = 0;
    unsigned char *seen_items = NULL;
    bool *visited = NULL;
    size_t *stack = NULL, stack_count = 0, stack_capacity = 0;
    bool ok = false;

    clear_error(&report);
    if (indices)
        *indices = NULL;
    if (count)
        *count = 0;
    if (!library || !indices || !count)
        return fail(&report, "Invalid collection query arguments");
    if (collection >= library->collection_count)
        return fail(&report, "Collection index is out of range");
    if (library->item_count) {
        if (library->item_count > SIZE_MAX / sizeof *seen_items)
            return fail(&report, "Out of memory");
        seen_items = (unsigned char *)calloc(library->item_count, sizeof *seen_items);
        if (!seen_items)
            return fail(&report, "Out of memory");
    }
    if (include_descendants) {
        if (library->collection_count > SIZE_MAX / sizeof *visited) {
            fail(&report, "Out of memory");
            goto done;
        }
        visited = (bool *)calloc(library->collection_count, sizeof *visited);
        if (!visited) {
            fail(&report, "Out of memory");
            goto done;
        }
    }
    if (!grow_array((void **)&stack, &stack_capacity, 1, sizeof *stack)) {
        fail(&report, "Out of memory");
        goto done;
    }
    stack[stack_count++] = collection;

    while (stack_count) {
        size_t current = stack[--stack_count];
        const CeZoteroCollection *node;
        if (current >= library->collection_count)
            continue;
        if (visited) {
            if (visited[current])
                continue;
            visited[current] = true;
        }
        node = &library->collections[current];
        if (node->item_indices) {
            for (size_t i = 0; i < node->item_count; i++) {
                size_t item = node->item_indices[i];
                if (item >= library->item_count)
                    continue;
                if (seen_items[item])
                    continue;
                seen_items[item] = 1;
                if (!grow_array((void **)&result, &result_capacity, result_count + 1,
                                sizeof *result)) {
                    fail(&report, "Out of memory");
                    goto done;
                }
                result[result_count++] = item;
            }
        }
        if (include_descendants && node->children) {
            for (size_t i = 0; i < node->child_count; i++) {
                size_t child = node->children[i];
                if (child >= library->collection_count || (visited && visited[child]))
                    continue;
                if (!grow_array((void **)&stack, &stack_capacity, stack_count + 1,
                                sizeof *stack)) {
                    fail(&report, "Out of memory");
                    goto done;
                }
                stack[stack_count++] = child;
            }
        }
        if (!include_descendants)
            break;
    }
    if (result_count > 1)
        qsort(result, result_count, sizeof *result, compare_size_t);
    {
        size_t write = 0;
        for (size_t read = 0; read < result_count; read++)
            if (!write || result[read] != result[write - 1])
                result[write++] = result[read];
        result_count = write;
    }
    if (!result_count) {
        free(result);
        result = NULL;
    }
    *indices = result;
    *count = result_count;
    clear_error(&report);
    ok = true;
done:
    free(seen_items);
    free(visited);
    free(stack);
    if (!ok)
        free(result);
    return ok;
}

/* ------------------------------------------------------------------------- */
/* Approximate citation formatting                                           */
/* ------------------------------------------------------------------------- */

static bool item_text_valid(const char *text) { return !text || utf8_valid(text); }

char *ce_zotero_format_approximate(const CeZoteroItem *item, size_t number,
                                   CeZoteroApproximateStyle style) {
    const char *author, *title, *year;
    TextBuffer output = {0};
    char number_text[32];
    int written;

    if (!item)
        return NULL;
    if (style != CE_ZOTERO_APPROXIMATE_APA && style != CE_ZOTERO_APPROXIMATE_IEEE &&
        style != CE_ZOTERO_APPROXIMATE_GBT_7714)
        return NULL;
    if (!item_text_valid(item->author) || !item_text_valid(item->title) ||
        !item_text_valid(item->year))
        return NULL;
    author = item->author && *item->author ? item->author : CE_ZOTERO_UNKNOWN_AUTHOR;
    title = item->title && *item->title ? item->title : CE_ZOTERO_UNTITLED;
    year = item->year ? item->year : "";

    switch (style) {
    case CE_ZOTERO_APPROXIMATE_APA:
        if (!text_add_string(&output, author) || !text_add_string(&output, ". "))
            goto failed;
        if (*year) {
            if (!text_add_string(&output, "(") || !text_add_string(&output, year) ||
                !text_add_string(&output, "). "))
                goto failed;
        } else if (!text_add_string(&output, "(n.d.). "))
            goto failed;
        if (!text_add_string(&output, title) || !text_add_string(&output, "."))
            goto failed;
        break;
    case CE_ZOTERO_APPROXIMATE_IEEE:
        written = snprintf(number_text, sizeof number_text, "%zu", number);
        if (written < 0 || (size_t)written >= sizeof number_text)
            goto failed;
        if (!text_add_string(&output, "[") || !text_add_string(&output, number_text) ||
            !text_add_string(&output, "] ") || !text_add_string(&output, author) ||
            !text_add_string(&output, ", \"") || !text_add_string(&output, title) ||
            !text_add_string(&output, "\""))
            goto failed;
        if (*year) {
            if (!text_add_string(&output, ", ") || !text_add_string(&output, year))
                goto failed;
        }
        if (!text_add_string(&output, "."))
            goto failed;
        break;
    case CE_ZOTERO_APPROXIMATE_GBT_7714:
        written = snprintf(number_text, sizeof number_text, "%zu", number);
        if (written < 0 || (size_t)written >= sizeof number_text)
            goto failed;
        if (!text_add_string(&output, "[") || !text_add_string(&output, number_text) ||
            !text_add_string(&output, "] ") || !text_add_string(&output, author) ||
            !text_add_string(&output, ". ") || !text_add_string(&output, title) ||
            !text_add_string(&output, "."))
            goto failed;
        if (*year) {
            if (!text_add_string(&output, " ") || !text_add_string(&output, year) ||
                !text_add_string(&output, "."))
                goto failed;
        }
        break;
    default:
        return NULL;
    }
    return text_take(&output);

failed:
    text_discard(&output);
    return NULL;
}
