#include "zotero.h"
#include "cJSON.h"
#include <inttypes.h>
#include <sqlite3.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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
    if (!text)
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
    item->csl_json = cJSON_PrintUnformatted(object);
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

static bool load_creators(sqlite3 *database, const SqliteSchema *schema,
                          CeZoteroLibrary *library, Report *report) {
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
            "SELECT ic.itemID, c.firstName, c.lastName, %s "
            "FROM itemCreators ic JOIN creators c ON c.creatorID=ic.creatorID "
            "ORDER BY ic.itemID, %s",
            schema->modern_creator_field_mode ? "COALESCE(c.fieldMode,0)" : "0",
            schema->item_creators_order ? "ic.orderIndex, ic.creatorID" : "ic.creatorID");
    } else {
        written = snprintf(
            sql, sizeof sql,
            "SELECT ic.itemID, cd.firstName, cd.lastName, %s "
            "FROM itemCreators ic "
            "JOIN creators c ON c.creatorID=ic.creatorID "
            "JOIN creatorData cd ON cd.creatorDataID=c.creatorDataID "
            "ORDER BY ic.itemID, %s",
            schema->legacy_creator_field_mode ? "COALESCE(cd.fieldMode,0)" : "0",
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
        char *first = NULL, *last = NULL;
        if (!find_item(library->items, library->item_count, item_id, &item_index)) {
            result = sqlite3_step(statement);
            continue;
        }
        if (!column_text(statement, 1, &first, report) ||
            !column_text(statement, 2, &last, report) || !first || !last) {
            free(first);
            free(last);
            goto done;
        }
        trim_in_place(first);
        trim_in_place(last);
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

bool ce_zotero_load_sqlite(const char *path, CeZoteroLibrary *library, char *error,
                           size_t error_capacity) {
    Report report = {error, error_capacity};
    CeZoteroLibrary temporary = {0};
    SqliteSchema schema;
    sqlite3 *database = NULL;
    bool transaction = false, success = false;
    int result;

    clear_error(&report);
    if (!library)
        return fail(&report, "Invalid library output");
    if (!path || !*path)
        return fail(&report, "SQLite database path is required");
    if (!utf8_valid(path))
        return fail(&report, "SQLite database path must be valid UTF-8");
    result = sqlite3_open_v2(path, &database, SQLITE_OPEN_READONLY, NULL);
    if (result != SQLITE_OK) {
        fail(&report, "Cannot open Zotero database read-only: %s",
             database ? sqlite3_errmsg(database) : sqlite3_errstr(result));
        goto done;
    }
    if (!exec_sql(database, "BEGIN DEFERRED", &report))
        goto done;
    transaction = true;
    if (!inspect_schema(database, &schema, &report) || !load_items(database, &schema, &temporary, &report) ||
        !load_creators(database, &schema, &temporary, &report) ||
        !load_collections(database, &schema, &temporary, &report) ||
        !load_collection_items(database, &temporary, &report) ||
        !link_collections(&temporary, &report))
        goto done;
    if (!exec_sql(database, "COMMIT", &report))
        goto done;
    transaction = false;
    success = true;

done:
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
