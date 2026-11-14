#include "csl_output.h"
#include "utf8proc.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct CeCslOutput {
    CeCslOutputAllocator allocator;
    CeCslOutputRun *runs;
    size_t count, capacity, text_bytes, payload_bytes;
};

static void clear_error(char *error, size_t capacity) {
    if (error && capacity) *error = 0;
}
static bool fail(char *error, size_t capacity, const char *message) {
    if (error && capacity) (void)snprintf(error, capacity, "%s", message);
    return false;
}
static void *default_allocate(void *context, size_t bytes) {
    (void)context;
    return malloc(bytes);
}
static void default_deallocate(void *context, void *memory) {
    (void)context;
    free(memory);
}
static void release(const CeCslOutput *output, const void *memory) {
    if (memory) output->allocator.deallocate(output->allocator.context, (void *)memory);
}
static char *copy_bytes(const CeCslOutput *output, const char *text, size_t length) {
    char *copy = output->allocator.allocate(output->allocator.context, length + 1);
    if (copy) {
        if (length) memcpy(copy, text, length);
        copy[length] = 0;
    }
    return copy;
}
static bool valid_utf8(const char *text, size_t length, bool link) {
    size_t at = 0;
    while (at < length) {
        utf8proc_int32_t cp;
        utf8proc_ssize_t used = utf8proc_iterate((const utf8proc_uint8_t *)text + at,
                                                (utf8proc_ssize_t)(length - at), &cp);
        if (used <= 0 || cp == 0) return false;
        if (link) {
            utf8proc_category_t category = utf8proc_category(cp);
            if (cp == '\\' || category == UTF8PROC_CATEGORY_CC || category == UTF8PROC_CATEGORY_CF ||
                category == UTF8PROC_CATEGORY_ZS || category == UTF8PROC_CATEGORY_ZL ||
                category == UTF8PROC_CATEGORY_ZP) return false;
        } else if (cp < 32 && cp != '\t' && cp != '\n' && cp != '\r') return false;
        at += (size_t)used;
    }
    return true;
}
static unsigned ascii_lower(unsigned ch) {
    return ch >= 'A' && ch <= 'Z' ? ch + ('a' - 'A') : ch;
}
static bool starts_with(const char *value, size_t length, const char *prefix) {
    size_t n = strlen(prefix);
    if (length < n) return false;
    for (size_t i = 0; i < n; ++i)
        if (ascii_lower((unsigned char)value[i]) != (unsigned char)prefix[i]) return false;
    return true;
}
static int hex_digit(unsigned char ch) {
    if (ch >= '0' && ch <= '9') return ch - '0';
    ch = (unsigned char)ascii_lower(ch);
    return ch >= 'a' && ch <= 'f' ? ch - 'a' + 10 : -1;
}
static bool hostname_character(unsigned char ch) {
    return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
           (ch >= '0' && ch <= '9') || ch == '-';
}
static bool valid_link(const char *href, size_t *length) {
    *length = 0;
    if (!href || !*href) return true;
    while (*length <= CE_CSL_OUTPUT_MAX_LINK_BYTES && href[*length]) ++*length;
    size_t size = *length;
    if (size > CE_CSL_OUTPUT_MAX_LINK_BYTES || !valid_utf8(href, size, true)) return false;
    size_t authority = starts_with(href, size, "https://") ? 8 :
                       starts_with(href, size, "http://") ? 7 : 0;
    if (!authority || authority == size) return false;
    size_t end = authority;
    while (end < size && href[end] != '/' && href[end] != '?' && href[end] != '#') ++end;
    size_t host_end = authority;
    while (host_end < end && href[host_end] != ':') ++host_end;
    if (host_end == authority || host_end - authority > 253) return false;
    size_t label = authority;
    for (size_t i = authority; i <= host_end; ++i) {
        if (i == host_end || href[i] == '.') {
            if (i == label || i - label > 63 || href[label] == '-' || href[i - 1] == '-') return false;
            label = i + 1;
        } else if (!hostname_character((unsigned char)href[i])) return false;
    }
    if (host_end < end) {
        size_t digits = end - host_end - 1;
        unsigned port = 0;
        if (!digits || digits > 5) return false;
        for (size_t i = host_end + 1; i < end; ++i) {
            if (href[i] < '0' || href[i] > '9') return false;
            port = port * 10 + (unsigned)(href[i] - '0');
        }
        if (!port || port > 65535) return false;
    }
    for (size_t i = end; i < size; ++i) {
        if (href[i] == '%') {
            if (size - i < 3) return false;
            int high = hex_digit((unsigned char)href[i + 1]), low = hex_digit((unsigned char)href[i + 2]);
            if (high < 0 || low < 0) return false;
            unsigned value = (unsigned)(high * 16 + low);
            if (value <= 32 || value == 127 || value == '\\') return false;
            i += 2;
        }
    }
    return true;
}

CeCslOutput *ce_csl_output_new(const CeCslOutputAllocator *allocator,
                             char *error, size_t capacity) {
    clear_error(error, capacity);
    CeCslOutputAllocator memory = {NULL, default_allocate, default_deallocate};
    if (allocator) memory = *allocator;
    if (!memory.allocate || !memory.deallocate) {
        fail(error, capacity, "Both output allocator callbacks are required");
        return NULL;
    }
    CeCslOutput *output = memory.allocate(memory.context, sizeof *output);
    if (!output) {
        fail(error, capacity, "Out of memory creating CSL output");
        return NULL;
    }
    memset(output, 0, sizeof *output);
    output->allocator = memory;
    return output;
}
static void free_run(const CeCslOutput *output, const CeCslOutputRun *run) {
    release(output, run->text);
    release(output, run->href);
}
void ce_csl_output_free(CeCslOutput *output) {
    if (!output) return;
    for (size_t i = 0; i < output->count; ++i) free_run(output, &output->runs[i]);
    release(output, output->runs);
    CeCslOutputAllocator memory = output->allocator;
    memory.deallocate(memory.context, output);
}
/* A replacement array copies run records, never their existing payloads. Do
 * not publish it until all new payloads are owned, so OOM is transactional. */
static CeCslOutputRun *run_storage(const CeCslOutput *output, size_t count, size_t *capacity) {
    *capacity = output->capacity;
    if (count <= output->capacity) return output->runs;
    size_t grown = output->capacity ? output->capacity : 8;
    while (grown < count) {
        if (grown > CE_CSL_OUTPUT_MAX_RUNS / 2) { grown = CE_CSL_OUTPUT_MAX_RUNS; break; }
        grown *= 2;
    }
    CeCslOutputRun *runs = output->allocator.allocate(output->allocator.context, grown * sizeof *runs);
    if (!runs) return NULL;
    if (output->count) memcpy(runs, output->runs, output->count * sizeof *runs);
    *capacity = grown;
    return runs;
}
static void commit_storage(CeCslOutput *output, CeCslOutputRun *runs, size_t capacity) {
    if (runs != output->runs) {
        release(output, output->runs);
        output->runs = runs;
        output->capacity = capacity;
    }
}
bool ce_csl_output_append(CeCslOutput *output, const char *text, size_t length,
                          unsigned format, const char *href, char *error, size_t capacity) {
    clear_error(error, capacity);
    const unsigned allowed = CE_CSL_BOLD | CE_CSL_ITALIC | CE_CSL_UNDERLINE |
                             CE_CSL_SMALL_CAPS | CE_CSL_SUPERSCRIPT | CE_CSL_SUBSCRIPT |
                             CE_CSL_OBLIQUE | CE_CSL_LIGHT;
    if (!output || (!text && length)) return fail(error, capacity, "Invalid CSL output/text argument");
    if ((format & ~allowed) || ((format & CE_CSL_SUPERSCRIPT) && (format & CE_CSL_SUBSCRIPT)) ||
        ((format & CE_CSL_ITALIC) && (format & CE_CSL_OBLIQUE)) ||
        ((format & CE_CSL_BOLD) && (format & CE_CSL_LIGHT)))
        return fail(error, capacity, "Invalid CSL text formatting");
    if (length > CE_CSL_OUTPUT_MAX_BYTES || length > CE_CSL_OUTPUT_MAX_BYTES - output->payload_bytes)
        return fail(error, capacity, "CSL output payload limit exceeded");
    if (!valid_utf8(text, length, false)) return fail(error, capacity, "Invalid UTF-8 or control character in CSL text");
    size_t link_bytes;
    if (!valid_link(href, &link_bytes)) return fail(error, capacity, "Unsupported or unsafe CSL link");
    if (!length) return true;
    if (link_bytes > CE_CSL_OUTPUT_MAX_BYTES - output->payload_bytes - length)
        return fail(error, capacity, "CSL output payload limit exceeded");
    if (output->count == CE_CSL_OUTPUT_MAX_RUNS) return fail(error, capacity, "CSL output run limit exceeded");
    CeCslOutputRun run = {copy_bytes(output, text, length), length, format, NULL};
    if (!run.text) return fail(error, capacity, "Out of memory copying CSL text");
    if (link_bytes) run.href = copy_bytes(output, href, link_bytes);
    if (link_bytes && !run.href) {
        free_run(output, &run);
        return fail(error, capacity, "Out of memory copying CSL link");
    }
    size_t reserved;
    CeCslOutputRun *runs = run_storage(output, output->count + 1, &reserved);
    if (!runs) {
        free_run(output, &run);
        return fail(error, capacity, "Out of memory growing CSL output");
    }
    runs[output->count] = run;
    commit_storage(output, runs, reserved);
    ++output->count;
    output->text_bytes += length;
    output->payload_bytes += length + link_bytes;
    return true;
}
bool ce_csl_output_concat(CeCslOutput *output, const CeCslOutput *source, char *error, size_t capacity) {
    clear_error(error, capacity);
    if (!output || !source) return fail(error, capacity, "Invalid CSL concatenation argument");
    if (source->payload_bytes > CE_CSL_OUTPUT_MAX_BYTES - output->payload_bytes ||
        source->count > CE_CSL_OUTPUT_MAX_RUNS - output->count)
        return fail(error, capacity, "CSL concatenation limit exceeded");
    if (!source->count) return true;
    size_t source_count = source->count, text_bytes = source->text_bytes, payload_bytes = source->payload_bytes;
    size_t reserved;
    CeCslOutputRun *runs = run_storage(output, output->count + source_count, &reserved);
    if (!runs) return fail(error, capacity, "Out of memory growing CSL output");
    size_t copied = 0;
    for (; copied < source_count; ++copied) {
        const CeCslOutputRun *original = &source->runs[copied];
        CeCslOutputRun run = {copy_bytes(output, original->text, original->byte_length),
                             original->byte_length, original->format, NULL};
        if (run.text && original->href) run.href = copy_bytes(output, original->href, strlen(original->href));
        if (!run.text || (original->href && !run.href)) {
            free_run(output, &run);
            for (size_t i = 0; i < copied; ++i) free_run(output, &runs[output->count + i]);
            if (runs != output->runs) release(output, runs);
            return fail(error, capacity, "Out of memory concatenating CSL runs");
        }
        runs[output->count + copied] = run;
    }
    commit_storage(output, runs, reserved);
    output->count += source_count;
    output->text_bytes += text_bytes;
    output->payload_bytes += payload_bytes;
    return true;
}
CeCslOutput *ce_csl_output_clone(const CeCslOutput *source, char *error, size_t capacity) {
    clear_error(error, capacity);
    if (!source) { fail(error, capacity, "Invalid CSL clone argument"); return NULL; }
    CeCslOutput *copy = ce_csl_output_new(&source->allocator, error, capacity);
    if (copy && !ce_csl_output_concat(copy, source, error, capacity)) {
        ce_csl_output_free(copy);
        return NULL;
    }
    return copy;
}
size_t ce_csl_output_run_count(const CeCslOutput *output) { return output ? output->count : 0; }
size_t ce_csl_output_text_bytes(const CeCslOutput *output) { return output ? output->text_bytes : 0; }
const CeCslOutputRun *ce_csl_output_run(const CeCslOutput *output, size_t index) {
    return output && index < output->count ? &output->runs[index] : NULL;
}
void ce_csl_output_string_free(CeCslOutputString *string) {
    if (!string) return;
    if (string->data && string->_deallocate) string->_deallocate(string->_context, string->data);
    memset(string, 0, sizeof *string);
}
static void set_string(const CeCslOutput *output, CeCslOutputString *result, char *data, size_t length) {
    ce_csl_output_string_free(result);
    result->data = data;
    result->length = length;
    result->_deallocate = output->allocator.deallocate;
    result->_context = output->allocator.context;
}
bool ce_csl_output_plain(const CeCslOutput *output, CeCslOutputString *result, char *error, size_t capacity) {
    clear_error(error, capacity);
    if (!output || !result) return fail(error, capacity, "Invalid CSL plain export argument");
    char *text = output->allocator.allocate(output->allocator.context, output->text_bytes + 1);
    if (!text) return fail(error, capacity, "Out of memory exporting CSL text");
    size_t used = 0;
    for (size_t i = 0; i < output->count; ++i) {
        memcpy(text + used, output->runs[i].text, output->runs[i].byte_length);
        used += output->runs[i].byte_length;
    }
    text[used] = 0;
    set_string(output, result, text, used);
    return true;
}
typedef struct HtmlWriter {
    char *data;
    size_t length;
    bool failed;
} HtmlWriter;
static void put_bytes(HtmlWriter *writer, const char *text, size_t size) {
    if (writer->failed) return;
    if (size > CE_CSL_OUTPUT_MAX_HTML_BYTES - writer->length) { writer->failed = true; return; }
    if (writer->data) memcpy(writer->data + writer->length, text, size);
    writer->length += size;
}
static void put(HtmlWriter *writer, const char *text) { put_bytes(writer, text, strlen(text)); }
static void escaped(HtmlWriter *writer, const char *text, size_t length) {
    size_t start = 0;
    for (size_t i = 0; i < length; ++i) {
        const char *entity = NULL;
        switch (text[i]) {
            case '&': entity = "&amp;"; break;
            case '<': entity = "&lt;"; break;
            case '>': entity = "&gt;"; break;
            case '"': entity = "&quot;"; break;
            case '\'': entity = "&#39;"; break;
            case '\r': entity = "&#13;"; break;
            default: break;
        }
        if (entity) {
            put_bytes(writer, text + start, i - start);
            put(writer, entity);
            start = i + 1;
        }
    }
    put_bytes(writer, text + start, length - start);
}
static void write_html(const CeCslOutput *output, HtmlWriter *writer) {
    put(writer, "<span style=\"white-space:pre-wrap\">");
    for (size_t i = 0; i < output->count && !writer->failed; ++i) {
        const CeCslOutputRun *run = &output->runs[i];
        if (run->href) {
            put(writer, "<a href=\"");
            escaped(writer, run->href, strlen(run->href));
            put(writer, "\" rel=\"noopener noreferrer\">");
        }
        if (run->format & CE_CSL_BOLD) put(writer, "<strong>");
        if (run->format & CE_CSL_LIGHT) put(writer, "<span style=\"font-weight:300\">");
        if (run->format & CE_CSL_ITALIC) put(writer, "<em>");
        if (run->format & CE_CSL_OBLIQUE) put(writer, "<span style=\"font-style:oblique\">");
        if (run->format & CE_CSL_UNDERLINE) put(writer, "<u>");
        if (run->format & CE_CSL_SMALL_CAPS) put(writer, "<span style=\"font-variant:small-caps\">");
        if (run->format & CE_CSL_SUPERSCRIPT) put(writer, "<sup>");
        if (run->format & CE_CSL_SUBSCRIPT) put(writer, "<sub>");
        escaped(writer, run->text, run->byte_length);
        if (run->format & CE_CSL_SUBSCRIPT) put(writer, "</sub>");
        if (run->format & CE_CSL_SUPERSCRIPT) put(writer, "</sup>");
        if (run->format & CE_CSL_SMALL_CAPS) put(writer, "</span>");
        if (run->format & CE_CSL_UNDERLINE) put(writer, "</u>");
        if (run->format & CE_CSL_OBLIQUE) put(writer, "</span>");
        if (run->format & CE_CSL_ITALIC) put(writer, "</em>");
        if (run->format & CE_CSL_LIGHT) put(writer, "</span>");
        if (run->format & CE_CSL_BOLD) put(writer, "</strong>");
        if (run->href) put(writer, "</a>");
    }
    put(writer, "</span>");
}
bool ce_csl_output_html(const CeCslOutput *output, CeCslOutputString *result, char *error, size_t capacity) {
    clear_error(error, capacity);
    if (!output || !result) return fail(error, capacity, "Invalid CSL HTML export argument");
    HtmlWriter measured = {0};
    write_html(output, &measured);
    if (measured.failed) return fail(error, capacity, "CSL HTML output limit exceeded");
    char *html = output->allocator.allocate(output->allocator.context, measured.length + 1);
    if (!html) return fail(error, capacity, "Out of memory exporting CSL HTML");
    HtmlWriter written = {html, 0, false};
    write_html(output, &written);
    html[written.length] = 0;
    set_string(output, result, html, written.length);
    return true;
}
