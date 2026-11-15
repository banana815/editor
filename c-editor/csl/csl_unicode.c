#include "csl_unicode.h"

#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <utf8proc.h>

typedef struct CodepointRange { uint32_t first, last; } CodepointRange;
typedef struct ScriptRange { uint32_t first, last; unsigned short script; } ScriptRange;
typedef struct SpecialCase {
    uint32_t codepoint, lower[3], title[3], upper[3];
    unsigned char lower_count, title_count, upper_count, language, context;
} SpecialCase;
typedef struct FoldCase {
    uint32_t codepoint, mapping[3];
    unsigned char count;
} FoldCase;
#include "data/unicode-tables.inc"

#define COUNT(a) (sizeof(a) / sizeof((a)[0]))
typedef struct Settings {
    size_t input_limit, output_limit, workspace_limit;
    void *(*allocate)(void *, size_t);
    void (*deallocate)(void *, void *);
    void *context;
} Settings;

static void *default_allocate(void *context, size_t size) {
    (void)context;
    return malloc(size);
}
static void default_deallocate(void *context, void *pointer) {
    (void)context;
    free(pointer);
}

bool ce_csl_unicode_is_scalar(uint32_t cp) {
    return cp <= 0x10ffff && !(cp >= 0xd800 && cp <= 0xdfff);
}

CeCslUnicodeStatus ce_csl_unicode_next(const char *input, size_t size,
                                     size_t *offset, uint32_t *codepoint) {
    utf8proc_int32_t cp;
    utf8proc_ssize_t n;
    size_t available;
    if ((!input && size) || !offset || !codepoint || *offset > size)
        return CE_CSL_UNICODE_INVALID_ARGUMENT;
    if (*offset == size) return CE_CSL_UNICODE_END;
    available = size - *offset;
    if (available > 4) available = 4;
    n = utf8proc_iterate((const utf8proc_uint8_t *)input + *offset,
                        (utf8proc_ssize_t)available, &cp);
    if (n <= 0 || !ce_csl_unicode_is_scalar((uint32_t)cp))
        return CE_CSL_UNICODE_INVALID_UTF8;
    *offset += (size_t)n;
    *codepoint = (uint32_t)cp;
    return CE_CSL_UNICODE_OK;
}

CeCslUnicodeStatus ce_csl_unicode_validate(const char *input, size_t size,
                                         size_t *bad_offset) {
    size_t offset = 0;
    uint32_t cp;
    CeCslUnicodeStatus status;
    if (!input && size) return CE_CSL_UNICODE_INVALID_ARGUMENT;
    while (offset < size) {
        status = ce_csl_unicode_next(input, size, &offset, &cp);
        if (status != CE_CSL_UNICODE_OK) {
            if (bad_offset) *bad_offset = offset;
            return status;
        }
    }
    if (bad_offset) *bad_offset = size;
    return CE_CSL_UNICODE_OK;
}

size_t ce_csl_unicode_encode(uint32_t cp, char output[4]) {
    if (!output || !ce_csl_unicode_is_scalar(cp)) return 0;
    return (size_t)utf8proc_encode_char((utf8proc_int32_t)cp,
                                      (utf8proc_uint8_t *)output);
}

static bool in_ranges(uint32_t cp, const CodepointRange *ranges, size_t count) {
    size_t lo = 0, hi = count;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (cp < ranges[mid].first) hi = mid;
        else if (cp > ranges[mid].last) lo = mid + 1;
        else return true;
    }
    return false;
}
bool ce_csl_unicode_is_space(uint32_t cp) {
    return in_ranges(cp, white_space_ranges, COUNT(white_space_ranges));
}
bool ce_csl_unicode_is_cased(uint32_t cp) {
    return in_ranges(cp, cased_ranges, COUNT(cased_ranges));
}
bool ce_csl_unicode_is_case_ignorable(uint32_t cp) {
    return in_ranges(cp, case_ignorable_ranges, COUNT(case_ignorable_ranges));
}
static bool is_soft_dotted(uint32_t cp) {
    return in_ranges(cp, soft_dotted_ranges, COUNT(soft_dotted_ranges));
}
static utf8proc_category_t category(uint32_t cp) {
    return ce_csl_unicode_is_scalar(cp) ?
        utf8proc_category((utf8proc_int32_t)cp) : UTF8PROC_CATEGORY_CN;
}
bool ce_csl_unicode_is_letter(uint32_t cp) {
    utf8proc_category_t c = category(cp);
    return c >= UTF8PROC_CATEGORY_LU && c <= UTF8PROC_CATEGORY_LO;
}
bool ce_csl_unicode_is_mark(uint32_t cp) {
    utf8proc_category_t c = category(cp);
    return c >= UTF8PROC_CATEGORY_MN && c <= UTF8PROC_CATEGORY_ME;
}
bool ce_csl_unicode_is_number(uint32_t cp) {
    utf8proc_category_t c = category(cp);
    return c >= UTF8PROC_CATEGORY_ND && c <= UTF8PROC_CATEGORY_NO;
}
bool ce_csl_unicode_is_word_codepoint(uint32_t cp) {
    return ce_csl_unicode_is_letter(cp) || ce_csl_unicode_is_mark(cp) ||
        ce_csl_unicode_is_number(cp) || category(cp) == UTF8PROC_CATEGORY_PC;
}
const char *ce_csl_unicode_script(uint32_t cp) {
    size_t low=0,high=COUNT(script_ranges);
    if(!ce_csl_unicode_is_scalar(cp))return "Unknown";
    while(low<high) {
        size_t mid=low+(high-low)/2;
        if(cp<script_ranges[mid].first)high=mid;
        else if(cp>script_ranges[mid].last)low=mid+1;
        else return script_names[script_ranges[mid].script];
    }
    return "Unknown";
}

static unsigned char ascii_lower(unsigned char c) {
    return c >= 'A' && c <= 'Z' ? (unsigned char)(c + 'a' - 'A') : c;
}
static unsigned language_id(const char *tag) {
    unsigned char a, b;
    unsigned id = 0;
    if (!tag || !tag[0] || !tag[1]) return 0;
    a = ascii_lower((unsigned char)tag[0]);
    b = ascii_lower((unsigned char)tag[1]);
    if (a == 't' && b == 'r') id = 1;
    if (a == 'a' && b == 'z') id = 2;
    if (a == 'l' && b == 't') id = 3;
    return id && (!tag[2] || tag[2] == '-' || tag[2] == '_') ? id : 0;
}
static unsigned combining_class(uint32_t cp) {
    return (unsigned)utf8proc_get_property((utf8proc_int32_t)cp)->combining_class;
}

/* Context predicates inspect the original string. Case_Ignorable is consumed
 * before Cased: U+0345 has both properties and must not stop that scan. */
static bool context_matches(unsigned context, const uint32_t *s, size_t n, size_t i) {
    size_t j;
    switch (context) {
    case 0: return true;
    case 1: /* Final_Sigma */
        j = i;
        while (j && ce_csl_unicode_is_case_ignorable(s[j - 1])) --j;
        if (!j || !ce_csl_unicode_is_cased(s[j - 1])) return false;
        j = i + 1;
        while (j < n && ce_csl_unicode_is_case_ignorable(s[j])) ++j;
        return j == n || !ce_csl_unicode_is_cased(s[j]);
    case 2: /* After_Soft_Dotted */
    case 6: /* After_I */
        j = i;
        while (j) {
            uint32_t cp = s[--j];
            unsigned ccc = combining_class(cp);
            if ((context == 2 && is_soft_dotted(cp)) ||
                (context == 6 && cp == 0x49)) return true;
            if (ccc == 0 || ccc == 230) return false;
        }
        return false;
    case 3: /* More_Above */
        for (j = i + 1; j < n; ++j) {
            unsigned ccc = combining_class(s[j]);
            if (ccc == 230) return true;
            if (ccc == 0) return false;
        }
        return false;
    case 4: /* Before_Dot */
    case 5: /* Not_Before_Dot */
        for (j = i + 1; j < n; ++j) {
            unsigned ccc = combining_class(s[j]);
            if (s[j] == 0x307) return context == 4;
            if (ccc == 0 || ccc == 230) break;
        }
        return context == 5;
    default: return false;
    }
}

static CeCslUnicodeStatus map_case(const uint32_t *s, size_t n, size_t i,
    CeCslUnicodeOperation operation, unsigned language, uint32_t mapped[4], size_t *count) {
    uint32_t cp = s[i];
    size_t lo = 0, hi = COUNT(special_cases), j;
    if (operation == CE_CSL_UNICODE_CASEFOLD) {
        utf8proc_int32_t folded[4];
        utf8proc_ssize_t length;
        if (language == 1 || language == 2) {
            for (j = 0; j < COUNT(turkic_folds); ++j) {
                if (turkic_folds[j].codepoint == cp) {
                    *count = turkic_folds[j].count;
                    memcpy(mapped, turkic_folds[j].mapping, *count * sizeof(*mapped));
                    return CE_CSL_UNICODE_OK;
                }
            }
        }
        length = utf8proc_decompose_char((utf8proc_int32_t)cp, folded, 4,
                                         UTF8PROC_CASEFOLD, NULL);
        if (length < 0 || length > 4) return CE_CSL_UNICODE_LIMIT;
        *count = (size_t)length;
        for (j = 0; j < *count; ++j) mapped[j] = (uint32_t)folded[j];
        return CE_CSL_UNICODE_OK;
    }
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (special_cases[mid].codepoint < cp) lo = mid + 1;
        else hi = mid;
    }
    for (j = lo; j < COUNT(special_cases) && special_cases[j].codepoint == cp; ++j) {
        const SpecialCase *m = &special_cases[j];
        if (m->language && m->language != language) continue;
        if (!context_matches(m->context, s, n, i)) continue;
        if (operation == CE_CSL_UNICODE_UPPER) {
            *count = m->upper_count;
            memcpy(mapped, m->upper, *count * sizeof(*mapped));
        } else if (operation == CE_CSL_UNICODE_TITLE_MAPPING) {
            *count = m->title_count;
            memcpy(mapped, m->title, *count * sizeof(*mapped));
        } else {
            *count = m->lower_count;
            memcpy(mapped, m->lower, *count * sizeof(*mapped));
        }
        return CE_CSL_UNICODE_OK;
    }
    mapped[0] = (uint32_t)(operation == CE_CSL_UNICODE_UPPER ? utf8proc_toupper((utf8proc_int32_t)cp) :
        operation == CE_CSL_UNICODE_TITLE_MAPPING ? utf8proc_totitle((utf8proc_int32_t)cp) : utf8proc_tolower((utf8proc_int32_t)cp));
    *count = 1;
    return CE_CSL_UNICODE_OK;
}

static CeCslUnicodeStatus encode_result(const utf8proc_int32_t *codepoints, size_t count,
                                      const Settings *settings, CeCslUnicodeText *out) {
    size_t i, bytes = 0, pos = 0;
    char encoded[4], *result;
    for (i = 0; i < count; ++i) {
        size_t length = ce_csl_unicode_encode((uint32_t)codepoints[i], encoded);
        if (length > settings->output_limit - bytes) return CE_CSL_UNICODE_LIMIT;
        bytes += length;
    }
    if (bytes == SIZE_MAX) return CE_CSL_UNICODE_LIMIT;
    result = settings->allocate(settings->context, bytes + 1);
    if (!result) return CE_CSL_UNICODE_OOM;
    for (i = 0; i < count; ++i)
        pos += ce_csl_unicode_encode((uint32_t)codepoints[i], result + pos);
    result[bytes] = '\0';
    *out = (CeCslUnicodeText){result, bytes, settings->deallocate, settings->context};
    return CE_CSL_UNICODE_OK;
}

/* Canonical ordering uses a stable counting sort for each non-starter run.
 * This avoids quadratic insertion sorting on adversarial combining sequences. */
static void canonical_order(utf8proc_int32_t *buffer, utf8proc_int32_t *scratch, size_t count) {
    size_t start = 0;
    while (start < count) {
        size_t end, i, bins[256], offsets[256], total = 0;
        unsigned previous = 0;
        bool disorder = false;
        if (!combining_class((uint32_t)buffer[start])) { ++start; continue; }
        memset(bins, 0, sizeof(bins));
        end = start;
        while (end < count) {
            unsigned ccc = combining_class((uint32_t)buffer[end]);
            if (!ccc) break;
            if (ccc < previous) disorder = true;
            previous = ccc;
            ++bins[ccc];
            ++end;
        }
        if (disorder) {
            for (i = 0; i < COUNT(bins); ++i) { offsets[i] = total; total += bins[i]; }
            for (i = start; i < end; ++i) {
                unsigned ccc = combining_class((uint32_t)buffer[i]);
                scratch[offsets[ccc]++] = buffer[i];
            }
            memcpy(buffer + start, scratch, (end - start) * sizeof(*buffer));
        }
        start = end;
    }
}

static CeCslUnicodeStatus normalize(const char *input, size_t size, bool compose,
                                    const Settings *settings, CeCslUnicodeText *out) {
    utf8proc_int32_t *buffer, part[4];
    utf8proc_ssize_t normalized;
    size_t offset = 0, count = 0, written = 0;
    uint32_t cp;
    CeCslUnicodeStatus result;
    while (offset < size) {
        utf8proc_ssize_t n;
        (void)ce_csl_unicode_next(input, size, &offset, &cp);
        n = utf8proc_decompose_char((utf8proc_int32_t)cp, part, 4,
                                    UTF8PROC_DECOMPOSE | UTF8PROC_STABLE, NULL);
        if (n < 0 || n > 4 || (size_t)n > SIZE_MAX - count) return CE_CSL_UNICODE_LIMIT;
        count += (size_t)n;
    }
    if (!count) return encode_result(NULL, 0, settings, out);
    if (count > settings->workspace_limit / (2 * sizeof(*buffer)) || count > (size_t)PTRDIFF_MAX)
        return CE_CSL_UNICODE_LIMIT;
    buffer = settings->allocate(settings->context, count * 2 * sizeof(*buffer));
    if (!buffer) return CE_CSL_UNICODE_OOM;
    offset = 0;
    while (offset < size) {
        utf8proc_ssize_t n;
        (void)ce_csl_unicode_next(input, size, &offset, &cp);
        n = utf8proc_decompose_char((utf8proc_int32_t)cp, part, 4,
                                    UTF8PROC_DECOMPOSE | UTF8PROC_STABLE, NULL);
        memcpy(buffer + written, part, (size_t)n * sizeof(*buffer));
        written += (size_t)n;
    }
    canonical_order(buffer, buffer + count, count);
    normalized = compose ? utf8proc_normalize_utf32(buffer, (utf8proc_ssize_t)count,
                                        UTF8PROC_COMPOSE | UTF8PROC_STABLE) : (utf8proc_ssize_t)count;
    result = normalized < 0 ? CE_CSL_UNICODE_LIMIT :
        encode_result(buffer, (size_t)normalized, settings, out);
    settings->deallocate(settings->context, buffer);
    return result;
}

static CeCslUnicodeStatus change_case(const char *input, size_t size,
    CeCslUnicodeOperation operation, unsigned language, const Settings *settings,
    CeCslUnicodeText *out, CeCslUnicodeSpan **map_out, size_t *map_count) {
    size_t offset = 0, count = 0, i, j, bytes = 0, written = 0;
    uint32_t cp, *codepoints, mapped[4];
    char encoded[4], *output;
    CeCslUnicodeSpan *spans = NULL;
    CeCslUnicodeStatus status = CE_CSL_UNICODE_OK;
    while (offset < size) {
        (void)ce_csl_unicode_next(input, size, &offset, &cp);
        ++count;
    }
    if (!count) return encode_result(NULL, 0, settings, out);
    size_t bytes_per_scalar = sizeof(*codepoints) + (map_out ? sizeof(*spans) : 0);
    if (count > settings->workspace_limit / bytes_per_scalar) return CE_CSL_UNICODE_LIMIT;
    codepoints = settings->allocate(settings->context, count * sizeof(*codepoints));
    if (!codepoints) return CE_CSL_UNICODE_OOM;
    offset = 0;
    for (i = 0; i < count; ++i)
        (void)ce_csl_unicode_next(input, size, &offset, &codepoints[i]);
    if (map_out) {
        spans = settings->allocate(settings->context, count * sizeof(*spans));
        if (!spans) { status = CE_CSL_UNICODE_OOM; goto cleanup; }
    }
    for (i = 0; i < count; ++i) {
        size_t mapped_count;
        status = map_case(codepoints, count, i, operation, language, mapped, &mapped_count);
        if (status != CE_CSL_UNICODE_OK) goto cleanup;
        for (j = 0; j < mapped_count; ++j) {
            size_t n = ce_csl_unicode_encode(mapped[j], encoded);
            if (n > settings->output_limit - bytes) { status = CE_CSL_UNICODE_LIMIT; goto cleanup; }
            bytes += n;
        }
    }
    if (bytes == SIZE_MAX) { status = CE_CSL_UNICODE_LIMIT; goto cleanup; }
    output = settings->allocate(settings->context, bytes + 1);
    if (!output) { status = CE_CSL_UNICODE_OOM; goto cleanup; }
    offset = 0;
    for (i = 0; i < count; ++i) {
        size_t mapped_count;
        size_t before = written;
        (void)map_case(codepoints, count, i, operation, language, mapped, &mapped_count);
        for (j = 0; j < mapped_count; ++j)
            written += ce_csl_unicode_encode(mapped[j], output + written);
        if (spans) {
            size_t source_size = ce_csl_unicode_encode(codepoints[i], encoded);
            spans[i] = (CeCslUnicodeSpan){offset, source_size, before, written - before};
            offset += source_size;
        }
    }
    output[bytes] = '\0';
    *out = (CeCslUnicodeText){output, bytes, settings->deallocate, settings->context};
    if (map_out) { *map_out = spans; *map_count = count; spans = NULL; }
cleanup:
    if (spans) settings->deallocate(settings->context, spans);
    settings->deallocate(settings->context, codepoints);
    return status;
}

static CeCslUnicodeStatus get_settings(const CeCslUnicodeOptions *options, Settings *settings) {
    *settings = (Settings){CE_CSL_UNICODE_DEFAULT_MAX_BYTES, CE_CSL_UNICODE_DEFAULT_MAX_BYTES,
        CE_CSL_UNICODE_DEFAULT_MAX_WORKSPACE, default_allocate, default_deallocate, NULL};
    if (options) {
        if (!!options->allocate != !!options->deallocate) return CE_CSL_UNICODE_INVALID_ARGUMENT;
        if (options->max_input_bytes) settings->input_limit = options->max_input_bytes;
        if (options->max_output_bytes) settings->output_limit = options->max_output_bytes;
        if (options->max_workspace_bytes) settings->workspace_limit = options->max_workspace_bytes;
        if (options->allocate) {
            settings->allocate = options->allocate;
            settings->deallocate = options->deallocate;
            settings->context = options->allocator_context;
        }
    }
    return CE_CSL_UNICODE_OK;
}
CeCslUnicodeStatus ce_csl_unicode_transform(const char *input, size_t size,
    CeCslUnicodeOperation operation, const char *language,
    const CeCslUnicodeOptions *options, CeCslUnicodeText *out) {
    Settings settings;
    CeCslUnicodeStatus status;
    if ((!input && size) || !out || out->data || operation < CE_CSL_UNICODE_NFC ||
        operation > CE_CSL_UNICODE_NFD) return CE_CSL_UNICODE_INVALID_ARGUMENT;
    status = get_settings(options, &settings);
    if (status != CE_CSL_UNICODE_OK) return status;
    if (size > settings.input_limit) return CE_CSL_UNICODE_LIMIT;
    status = ce_csl_unicode_validate(input, size, NULL);
    if (status != CE_CSL_UNICODE_OK) return status;
    return operation == CE_CSL_UNICODE_NFC || operation == CE_CSL_UNICODE_NFD ?
        normalize(input, size, operation == CE_CSL_UNICODE_NFC, &settings, out) :
        change_case(input, size, operation, language_id(language), &settings, out, NULL, NULL);
}

CeCslUnicodeStatus ce_csl_unicode_transform_mapped(const char *input, size_t size,
    CeCslUnicodeOperation operation, const char *language,
    const CeCslUnicodeOptions *options, CeCslUnicodeMappedText *out) {
    Settings settings;
    CeCslUnicodeStatus status;
    if ((!input && size) || !out || out->text.data || out->spans || out->span_count ||
        operation <= CE_CSL_UNICODE_NFC || operation > CE_CSL_UNICODE_TITLE_MAPPING)
        return CE_CSL_UNICODE_INVALID_ARGUMENT;
    status = get_settings(options, &settings);
    if (status != CE_CSL_UNICODE_OK) return status;
    if (size > settings.input_limit) return CE_CSL_UNICODE_LIMIT;
    status = ce_csl_unicode_validate(input, size, NULL);
    if (status != CE_CSL_UNICODE_OK) return status;
    return change_case(input, size, operation, language_id(language), &settings, &out->text, &out->spans, &out->span_count);
}
void ce_csl_unicode_mapped_free(CeCslUnicodeMappedText *out) {
    if (!out) return;
    if (out->spans) {
        if (out->text.deallocate) out->text.deallocate(out->text.allocator_context, out->spans);
        else free(out->spans);
    }
    ce_csl_unicode_text_free(&out->text);
    memset(out, 0, sizeof *out);
}

void ce_csl_unicode_text_free(CeCslUnicodeText *text) {
    if (!text) return;
    if (text->data) {
        if (text->deallocate) text->deallocate(text->allocator_context, text->data);
        else free(text->data);
    }
    memset(text, 0, sizeof(*text));
}

const char *ce_csl_unicode_status_string(CeCslUnicodeStatus status) {
    switch (status) {
    case CE_CSL_UNICODE_OK: return "success";
    case CE_CSL_UNICODE_END: return "end of input";
    case CE_CSL_UNICODE_INVALID_ARGUMENT: return "invalid Unicode argument";
    case CE_CSL_UNICODE_INVALID_UTF8: return "malformed UTF-8";
    case CE_CSL_UNICODE_LIMIT: return "Unicode size limit exceeded";
    case CE_CSL_UNICODE_OOM: return "out of memory";
    default: return "unknown Unicode status";
    }
}
