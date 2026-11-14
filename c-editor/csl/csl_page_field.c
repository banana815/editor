#include "csl_page_field.h"
#include "csl_inline.h"
#include "csl_unicode.h"
#include <stdio.h>
#include <string.h>

typedef struct Token { size_t start, end, digits, digits_end, separator, separator_size; } Token;
static bool fail(char *error, size_t capacity, const char *message) {
    if (error && capacity) snprintf(error, capacity, "CSL page field: %s", message);
    return false;
}
static uint32_t next(const char *text, size_t size, size_t at, size_t *after) {
    uint32_t cp = 0; *after = at;
    (void)ce_csl_unicode_next(text, size, after, &cp); return cp;
}
static void spaces(const char *text, size_t size, size_t *at) {
    while (*at < size) {
        size_t after; uint32_t cp = next(text, size, *at, &after);
        if (!ce_csl_unicode_is_space(cp)) break;
        *at = after;
    }
}
/* Called only after the shared scanner validates the WHOLE field. This records
 * source slices; it does not introduce a second numeric grammar or arithmetic. */
static Token token(const char *text, size_t size, size_t *at) {
    Token t = {0}; spaces(text, size, at); t.start = *at;
    while (*at < size && !(text[*at] >= '0' && text[*at] <= '9')) {
        size_t after; (void)next(text, size, *at, &after); *at = after;
    }
    t.digits = *at;
    while (*at < size && text[*at] >= '0' && text[*at] <= '9') ++*at;
    t.digits_end = *at;
    while (*at < size) {
        size_t after; uint32_t cp = next(text, size, *at, &after);
        if (!ce_csl_unicode_is_letter(cp) && !ce_csl_unicode_is_mark(cp)) break;
        *at = after;
    }
    t.end = *at; spaces(text, size, at);
    if (*at < size) {
        size_t after; (void)next(text, size, *at, &after);
        t.separator = *at; t.separator_size = after - *at; *at = after;
    }
    return t;
}
static bool range(const char *text, Token t) {
    return t.separator_size && (text[t.separator] == '-' || t.separator_size == 3);
}
static bool same(const char *text, size_t a, size_t an, size_t b, size_t bn) {
    return an == bn && !memcmp(text + a, text + b, an);
}
static bool append(CeCslOutput *out, const char *text, size_t size, char *error, size_t cap) {
    return ce_csl_output_append(out, text, size, 0, NULL, error, cap);
}
static bool hyphens(CeCslOutput *out, const char *text, size_t size, char *error, size_t cap) {
    size_t first = 0;
    for (size_t i = 0; i < size; ++i) if (text[i] == '-') {
        if (!append(out, text + first, i - first, error, cap) || !append(out, "–", 3, error, cap)) return false;
        first = i + 1;
    }
    return append(out, text ? text + first : NULL, size - first, error, cap);
}
bool ce_csl_page_field_render(const char *text, size_t size,
    const CeCslPageFieldOptions *options, const CeCslLocale *locale,
    const CeCslOutputAllocator *allocator, CeCslOutputString *output,
    CeCslPageFieldInfo *info, char *error, size_t capacity) {
    if (error && capacity) *error = 0;
    if (!options || !output || options->policy < CE_CSL_PAGE_FIELD_PAGE ||
        options->policy > CE_CSL_PAGE_FIELD_OTHER_LOCATOR ||
        (options->has_format && (options->format < CE_CSL_PAGE_EXPANDED || options->format > CE_CSL_PAGE_CHICAGO_16)))
        return fail(error, capacity, "invalid options/output");
    CeCslNumberFieldInfo scan;
    if (!ce_csl_number_field_scan(text, size, &scan, error, capacity)) return false;
    bool locator = options->policy != CE_CSL_PAGE_FIELD_PAGE;
    bool active = options->has_format && options->policy != CE_CSL_PAGE_FIELD_OTHER_LOCATOR;
    bool literal_tag = false;
    if ((active || locator) && size && memchr(text, '<', size)) {
        CeCslInline *parsed = NULL; CeCslInlineDiagnostic diagnostic = {0}; size_t plain_size = 0;
        if (!ce_csl_inline_parse(text, size, 0, allocator, &parsed, &diagnostic, error, capacity)) return false;
        ce_csl_inline_text(parsed, &plain_size);
        bool known = plain_size != size || diagnostic.malformed_markup;
        literal_tag = diagnostic.literal_markup;
        ce_csl_inline_free(parsed);
        if (known) return fail(error, capacity, "unsupported inline markup in transformed page/locator field");
    }
    CeCslOutput *temporary = ce_csl_output_new(allocator, error, capacity);
    if (!temporary) return false;
    CeCslPageFieldInfo result = {.numeric = scan.numeric}; bool ok = true;
    if (literal_tag || (!active && !locator) || (!scan.numeric && !locator))
        ok = append(temporary, text, size, error, capacity);
    else if (!active || !scan.numeric) ok = hyphens(temporary, text, size, error, capacity);
    else {
        CeCslLocaleTerm term = ce_csl_locale_term(locale, "page-range-delimiter", CE_CSL_LOCALE_LONG, false, CE_CSL_LOCALE_NEUTER);
        const char *delimiter = term.found ? term.text : "–"; size_t delimiter_size = term.found ? term.length : 3;
        size_t at = 0, copied = 0;
        while (ok && at < size) {
            Token left = token(text, size, &at);
            if (!range(text, left)) continue;
            Token right = token(text, size, &at);
            if (range(text, right)) { ok = fail(error, capacity, "unsupported chained page range"); break; }
            if (left.digits_end-left.digits > CE_CSL_NUMBER_MAX_DECIMAL_DIGITS ||
                right.digits_end-right.digits > CE_CSL_NUMBER_MAX_DECIMAL_DIGITS) {
                ok = fail(error, capacity, "page-range decimal limit exceeded"); break;
            }
            if (delimiter_size > CE_CSL_NUMBER_MAX_DELIMITER_BYTES) {
                ok = fail(error, capacity, "range delimiter limit exceeded"); break;
            }
            CeCslOutputString pair = {0}; const char *digits = text + right.digits;
            size_t digit_size = right.digits_end - right.digits;
            bool matching = same(text, left.start, left.digits-left.start, right.start, right.digits-right.start) &&
                same(text, left.digits_end, left.end-left.digits_end, right.digits_end, right.end-right.digits_end);
            if (matching) {
                ok = ce_csl_page_range_pair(text + left.digits, left.digits_end-left.digits,
                    digits, digit_size, options->format, delimiter, delimiter_size,
                    allocator, &pair, error, capacity) == CE_CSL_NUMBER_OK;
                if (ok) {
                    size_t tail = left.digits_end-left.digits + delimiter_size;
                    digits = pair.data + tail; digit_size = pair.length - tail;
                }
            }
            if (ok) ok = append(temporary, text + copied, left.separator-copied, error, capacity) &&
                append(temporary, delimiter, delimiter_size, error, capacity) &&
                append(temporary, text + left.separator + left.separator_size,
                    right.digits-left.separator-left.separator_size, error, capacity) &&
                append(temporary, digits, digit_size, error, capacity);
            ce_csl_output_string_free(&pair);
            if (ok) { copied = right.digits_end; ++result.range_count; result.missing_term |= !term.found; }
        }
        if (ok) ok = append(temporary, text + copied, size-copied, error, capacity);
    }
    if (ok) ok = ce_csl_output_plain(temporary, output, error, capacity);
    ce_csl_output_free(temporary);
    if (ok && info) *info = result;
    return ok;
}
