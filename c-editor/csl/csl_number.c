#include "csl_number.h"
#include "utf8proc.h"
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* CSL 1.0.2 scalar numbers and Appendix V page-range formats:
 * https://docs.citationstyles.org/en/stable/specification.html#number
 * https://docs.citationstyles.org/en/stable/specification.html#appendix-v-page-range-formats
 * This module does not infer how arbitrary strings are split into numbers. */
static CeCslNumberStatus fail(CeCslNumberStatus status, char *error, size_t capacity, const char *message) {
    if (error && capacity) snprintf(error, capacity, "%s", message);
    return status;
}
static void *default_allocate(void *context, size_t size) { (void)context; return malloc(size); }
static void default_deallocate(void *context, void *memory) { (void)context; free(memory); }
static bool memory_options(const CeCslOutputAllocator *allocator, CeCslOutputAllocator *memory) {
    *memory = (CeCslOutputAllocator){NULL, default_allocate, default_deallocate};
    if (allocator) *memory = *allocator;
    return memory->allocate && memory->deallocate;
}
static void replace_string(CeCslOutputString *out, char *data, size_t size, const CeCslOutputAllocator *memory) {
    ce_csl_output_string_free(out);
    out->data = data; out->length = size;
    out->_context = memory->context; out->_deallocate = memory->deallocate;
}
static bool valid_text(const char *text, size_t size) {
    for (size_t at = 0; at < size;) {
        utf8proc_int32_t cp;
        utf8proc_ssize_t n = utf8proc_iterate((const utf8proc_uint8_t *)text + at,
                                             (utf8proc_ssize_t)(size - at), &cp);
        if (n <= 0 || cp == 0 || (cp < 32 && cp != '\t' && cp != '\n' && cp != '\r')) return false;
        at += (size_t)n;
    }
    return true;
}
static size_t roman(uint64_t number, char *out) {
    static const struct { unsigned value; const char *text; } digits[] = {
        {1000, "m"}, {900, "cm"}, {500, "d"}, {400, "cd"}, {100, "c"}, {90, "xc"},
        {50, "l"}, {40, "xl"}, {10, "x"}, {9, "ix"}, {5, "v"}, {4, "iv"}, {1, "i"}
    };
    size_t size = 0;
    for (size_t i = 0; i < sizeof digits / sizeof *digits; ++i) {
        while (number >= digits[i].value) {
            size_t length = strlen(digits[i].text);
            memcpy(out + size, digits[i].text, length);
            size += length; number -= digits[i].value;
        }
    }
    out[size] = 0;
    return size;
}
CeCslNumberStatus ce_csl_number_format(uint64_t number, CeCslNumberForm form,
    const CeCslLocale *locale, CeCslLocaleGender gender,
    const CeCslOutputAllocator *allocator, CeCslOutputString *out,
    bool *missing_term, char *error, size_t capacity) {
    if (error && capacity) *error = 0;
    CeCslOutputAllocator memory;
    if (!out || !memory_options(allocator, &memory) ||
        form < CE_CSL_NUMBER_NUMERIC || form > CE_CSL_NUMBER_ROMAN ||
        gender < CE_CSL_LOCALE_NEUTER || gender > CE_CSL_LOCALE_FEMININE)
        return fail(CE_CSL_NUMBER_INVALID_ARGUMENT, error, capacity, "Invalid CSL number formatting argument");
    char digits[32];
    size_t digit_size;
    if (form == CE_CSL_NUMBER_ROMAN) {
        if (!number || number > 3999)
            return fail(CE_CSL_NUMBER_UNSUPPORTED, error, capacity, "Roman numerals currently support only 1..3999");
        digit_size = roman(number, digits);
    } else {
        int written = snprintf(digits, sizeof digits, "%" PRIu64, number);
        if (written < 0 || (size_t)written >= sizeof digits)
            return fail(CE_CSL_NUMBER_UNSUPPORTED, error, capacity, "Cannot represent the unsigned number");
        digit_size = (size_t)written;
    }
    const char *term = "";
    size_t term_size = 0;
    bool missing = false;
    if (form == CE_CSL_NUMBER_ORDINAL || form == CE_CSL_NUMBER_LONG_ORDINAL) {
        CeCslLocaleOrdinal ordinal = ce_csl_locale_ordinal(locale, number, form == CE_CSL_NUMBER_LONG_ORDINAL, gender);
        missing = !ordinal.term.found;
        if (ordinal.term.found) {
            term = ordinal.term.text; term_size = ordinal.term.length;
            if (ordinal.is_long) digit_size = 0;
        }
    }
    if (term_size > CE_CSL_OUTPUT_MAX_BYTES - digit_size)
        return fail(CE_CSL_NUMBER_LIMIT, error, capacity, "CSL number output limit exceeded");
    if (!term || !valid_text(term, term_size))
        return fail(CE_CSL_NUMBER_INVALID_UTF8, error, capacity, "Invalid UTF-8 or control character in ordinal term");
    size_t size = digit_size + term_size;
    char *data = memory.allocate(memory.context, size + 1);
    if (!data) return fail(CE_CSL_NUMBER_OOM, error, capacity, "Out of memory formatting CSL number");
    memcpy(data, digits, digit_size);
    if (term_size) memcpy(data + digit_size, term, term_size);
    data[size] = 0;
    replace_string(out, data, size, &memory);
    if (missing_term) *missing_term = missing;
    return CE_CSL_NUMBER_OK;
}
static bool decimal(const char *value, size_t size) {
    if (!value || !size) return false;
    for (size_t i = 0; i < size; ++i) if (value[i] < '0' || value[i] > '9') return false;
    return true;
}
static int decimal_compare(const char *a, size_t a_size, const char *b, size_t b_size) {
    while (a_size > 1 && *a == '0') { ++a; --a_size; }
    while (b_size > 1 && *b == '0') { ++b; --b_size; }
    if (a_size != b_size) return a_size < b_size ? -1 : 1;
    int order = memcmp(a, b, a_size);
    return order < 0 ? -1 : order > 0 ? 1 : 0;
}
static size_t expanded_end(const char *start, size_t start_size, const char *end, size_t end_size, char *expanded) {
    if (end_size >= start_size) {
        memcpy(expanded, end, end_size);
        expanded[end_size] = 0;
        return end_size;
    }
    size_t prefix = start_size - end_size, size = start_size;
    memcpy(expanded, start, prefix);
    memcpy(expanded + prefix, end, end_size);
    if (decimal_compare(expanded, size, start, start_size) < 0) {
        size_t at = prefix;
        while (at && expanded[at - 1] == '9') expanded[--at] = '0';
        if (at) ++expanded[at - 1];
        else {
            memmove(expanded + 1, expanded, size);
            expanded[0] = '1'; ++size;
        }
    }
    expanded[size] = 0;
    return size;
}
static size_t end_offset(const char *start, size_t start_size, const char *end, size_t end_size,
                          CeCslPageRangeFormat format) {
    if (format == CE_CSL_PAGE_EXPANDED || start_size != end_size ||
        (start_size > 1 && (*start == '0' || *end == '0')) ||
        !memcmp(start, end, start_size)) return 0;
    size_t common = 0;
    while (common < start_size && start[common] == end[common]) ++common;
    size_t keep = end_size - common;
    bool chicago = format == CE_CSL_PAGE_CHICAGO || format == CE_CSL_PAGE_CHICAGO_15 || format == CE_CSL_PAGE_CHICAGO_16;
    if (chicago) {
        if (start_size < 3) return 0;
        unsigned last_two = (unsigned)((start[start_size - 2] - '0') * 10 + start[start_size - 1] - '0');
        if (!last_two) return 0;
        if (format != CE_CSL_PAGE_CHICAGO_16 && start_size == 4 && keep == 3) return 0;
        if (last_two >= 10 && keep < 2) keep = 2;
    } else if (format == CE_CSL_PAGE_MINIMAL_TWO && keep < 2) keep = end_size < 2 ? end_size : 2;
    return end_size - keep;
}
CeCslNumberStatus ce_csl_page_range_pair(const char *start, size_t start_size,
    const char *end, size_t end_size, CeCslPageRangeFormat format,
    const char *delimiter, size_t delimiter_size,
    const CeCslOutputAllocator *allocator, CeCslOutputString *out,
    char *error, size_t capacity) {
    if (error && capacity) *error = 0;
    CeCslOutputAllocator memory;
    if (!out || !memory_options(allocator, &memory) || !delimiter ||
        format < CE_CSL_PAGE_EXPANDED || format > CE_CSL_PAGE_CHICAGO_16)
        return fail(CE_CSL_NUMBER_INVALID_ARGUMENT, error, capacity, "Invalid CSL page-range argument");
    if (start_size > CE_CSL_NUMBER_MAX_DECIMAL_DIGITS || end_size > CE_CSL_NUMBER_MAX_DECIMAL_DIGITS ||
        delimiter_size > CE_CSL_NUMBER_MAX_DELIMITER_BYTES)
        return fail(CE_CSL_NUMBER_LIMIT, error, capacity, "CSL page-range input limit exceeded");
    if (!decimal(start, start_size) || !decimal(end, end_size))
        return fail(CE_CSL_NUMBER_INVALID_ARGUMENT, error, capacity, "Page-range endpoints must be unsigned ASCII decimal digits");
    if (!valid_text(delimiter, delimiter_size))
        return fail(CE_CSL_NUMBER_INVALID_UTF8, error, capacity, "Invalid UTF-8 or control character in range delimiter");
    if (end_size >= start_size && decimal_compare(start, start_size, end, end_size) > 0)
        return fail(CE_CSL_NUMBER_UNSUPPORTED, error, capacity, "Descending full-width page range is unsupported");
    size_t scratch_size = (start_size > end_size ? start_size : end_size) + 2;
    char *expanded = memory.allocate(memory.context, scratch_size);
    if (!expanded) return fail(CE_CSL_NUMBER_OOM, error, capacity, "Out of memory expanding page range");
    size_t expanded_size = expanded_end(start, start_size, end, end_size, expanded);
    size_t offset = end_offset(start, start_size, expanded, expanded_size, format);
    size_t tail_size = expanded_size - offset;
    if (start_size > CE_CSL_OUTPUT_MAX_BYTES - delimiter_size ||
        tail_size > CE_CSL_OUTPUT_MAX_BYTES - delimiter_size - start_size) {
        memory.deallocate(memory.context, expanded);
        return fail(CE_CSL_NUMBER_LIMIT, error, capacity, "CSL page-range output limit exceeded");
    }
    size_t size = start_size + delimiter_size + tail_size;
    char *data = memory.allocate(memory.context, size + 1);
    if (!data) {
        memory.deallocate(memory.context, expanded);
        return fail(CE_CSL_NUMBER_OOM, error, capacity, "Out of memory formatting page range");
    }
    memcpy(data, start, start_size);
    memcpy(data + start_size, delimiter, delimiter_size);
    memcpy(data + start_size + delimiter_size, expanded + offset, tail_size);
    data[size] = 0;
    memory.deallocate(memory.context, expanded);
    replace_string(out, data, size, &memory);
    return CE_CSL_NUMBER_OK;
}
const char *ce_csl_number_status_string(CeCslNumberStatus status) {
    switch (status) {
        case CE_CSL_NUMBER_OK: return "Success";
        case CE_CSL_NUMBER_INVALID_ARGUMENT: return "Invalid argument";
        case CE_CSL_NUMBER_INVALID_UTF8: return "Invalid UTF-8 or control character";
        case CE_CSL_NUMBER_UNSUPPORTED: return "Unsupported numeric representation";
        case CE_CSL_NUMBER_LIMIT: return "Numeric formatting limit exceeded";
        case CE_CSL_NUMBER_OOM: return "Out of memory";
        default: return "Unknown numeric formatting status";
    }
}
