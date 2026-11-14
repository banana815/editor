#include "csl_case.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <utf8proc.h>
#include "data/csl-stop-words.inc"

/* CSL 1.0.2 text-case rules, not the older reference processor's heuristics:
 * https://docs.citationstyles.org/en/stable/specification.html#text-case */
typedef struct Memory Memory;
typedef union Block {
    struct { CeCslOutputAllocator allocator; Memory *owner; size_t total; } header;
    long double alignment;
    void *pointer;
} Block;
struct Memory {
    CeCslOutputAllocator allocator;
    size_t used;
    bool limit;
};
typedef struct Word {
    size_t start, end;
    bool lowercase, stop, exception;
} Word;
typedef struct Builder {
    CeCslCaseMappedText result;
    size_t capacity;
    Memory *memory;
    CeCslUnicodeOptions options;
    const char *language;
} Builder;

static void *default_allocate(void *context, size_t size) { (void)context; return malloc(size); }
static void default_deallocate(void *context, void *pointer) { (void)context; free(pointer); }
static void *allocate(void *context, size_t size) {
    Memory *memory = context;
    if (size > CE_CSL_CASE_MAX_WORKSPACE - sizeof(Block) ||
        size + sizeof(Block) > CE_CSL_CASE_MAX_WORKSPACE - memory->used) {
        memory->limit = true; return NULL;
    }
    size_t total = size + sizeof(Block);
    Block *block = memory->allocator.allocate(memory->allocator.context, total);
    if (!block) return NULL;
    block->header.allocator = memory->allocator;
    block->header.owner = memory;
    block->header.total = total;
    memory->used += total;
    return block + 1;
}
static void release(void *context, void *pointer) {
    (void)context;
    if (!pointer) return;
    Block *block = (Block *)pointer - 1;
    if (block->header.owner) block->header.owner->used -= block->header.total;
    CeCslOutputAllocator original = block->header.allocator;
    original.deallocate(original.context, block);
}
/* Results outlive this stack's Memory. The allocation header retains the real
 * allocator, and exported callbacks do not borrow the tracking context. */
static void detach(void *pointer) {
    if (!pointer) return;
    Block *block = (Block *)pointer - 1;
    if (block->header.owner) block->header.owner->used -= block->header.total;
    block->header.owner = NULL;
}
static bool fail(char *error, size_t capacity, const char *message) {
    if (error && capacity) snprintf(error, capacity, "%s", message);
    return false;
}
static bool memory_fail(Memory *memory, char *error, size_t capacity) {
    return fail(error, capacity, memory->limit ? "CSL text-case workspace limit exceeded" : "Out of memory converting CSL text case");
}
static bool unicode_fail(CeCslUnicodeStatus status, Memory *memory, char *error, size_t capacity) {
    if (status == CE_CSL_UNICODE_OOM) return memory_fail(memory, error, capacity);
    return fail(error, capacity, ce_csl_unicode_status_string(status));
}
static unsigned lower_ascii(unsigned ch) { return ch >= 'A' && ch <= 'Z' ? ch + ('a' - 'A') : ch; }
static bool english_language(const char *language) {
    return language && lower_ascii((unsigned char)language[0]) == 'e' && language[1] &&
        lower_ascii((unsigned char)language[1]) == 'n' &&
        (!language[2] || language[2] == '-' || language[2] == '_');
}
static bool english_item(const char *item, const char *style) {
    if (item && *item) return english_language(item);
    return !style || !*style || english_language(style);
}
static bool apostrophe(uint32_t cp) { return cp == '\'' || cp == 0x2019; }
static bool next(const char *text, size_t size, size_t *at, uint32_t *cp) {
    return ce_csl_unicode_next(text, size, at, cp) == CE_CSL_UNICODE_OK;
}
static size_t scalar_count(const char *text, size_t size) {
    size_t at = 0, count = 0; uint32_t cp;
    while (next(text, size, &at, &cp)) ++count;
    return count;
}
static bool uppercase_string(const char *text, size_t size) {
    size_t at = 0; uint32_t cp; bool cased = false;
    while (next(text, size, &at, &cp)) {
        if (ce_csl_unicode_is_cased(cp)) {
            cased = true;
            if (!utf8proc_isupper((utf8proc_int32_t)cp)) return false;
        }
    }
    return cased;
}
static bool next_word(const char *text, size_t size, size_t *at, Word *word) {
    uint32_t cp;
    size_t begin;
    for (;;) {
        begin = *at;
        if (!next(text, size, at, &cp)) return false;
        if (ce_csl_unicode_is_word_codepoint(cp)) break;
    }
    bool cased = ce_csl_unicode_is_cased(cp);
    bool lower = !cased || utf8proc_islower((utf8proc_int32_t)cp);
    size_t first_end = *at;
    bool d_prefix = cp == 'd' || cp == 'D';
    while (*at < size) {
        size_t end = *at;
        uint32_t current;
        (void)next(text, size, &end, &current);
        if (ce_csl_unicode_is_word_codepoint(current)) {
            if (ce_csl_unicode_is_cased(current)) {
                cased = true;
                if (!utf8proc_islower((utf8proc_int32_t)current)) lower = false;
            }
            *at = end;
        } else if (apostrophe(current)) {
            size_t look = end; uint32_t following;
            if (!next(text, size, &look, &following) || !ce_csl_unicode_is_word_codepoint(following)) break;
            bool split = d_prefix && *at == first_end;
            *at = end;
            if (split) break;
        } else break;
    }
    *word = (Word){begin, *at, cased && lower, false, false};
    return true;
}
static bool stop_match(const char *text, size_t size, size_t start, const char *pattern, size_t *end) {
    size_t input = start, p = 0, pattern_size = strlen(pattern);
    uint32_t last = 0;
    while (p < pattern_size) {
        uint32_t wanted, actual;
        (void)next(pattern, pattern_size, &p, &wanted);
        last = wanted;
        if (wanted == ' ') {
            bool found = false;
            while (input < size) {
                size_t look = input;
                (void)next(text, size, &look, &actual);
                if (!ce_csl_unicode_is_space(actual)) break;
                input = look; found = true;
            }
            if (!found) return false;
        } else {
            if (!next(text, size, &input, &actual)) return false;
            if (wanted == '\'' && actual == 0x2019) actual = '\'';
            if ((uint32_t)utf8proc_tolower((utf8proc_int32_t)actual) != wanted) return false;
        }
    }
    if (input < size && last != '\'') {
        size_t look = input; uint32_t after;
        (void)next(text, size, &look, &after);
        if (ce_csl_unicode_is_word_codepoint(after) || apostrophe(after)) return false;
    }
    *end = input;
    return true;
}
static bool words(const char *text, size_t size, Memory *memory, bool title, Word **result, size_t *count) {
    size_t at = 0; Word current;
    *count = 0; *result = NULL;
    while (next_word(text, size, &at, &current)) ++*count;
    if (!*count) return true;
    if (*count > CE_CSL_CASE_MAX_WORKSPACE / sizeof **result) { memory->limit = true; return false; }
    Word *array = allocate(memory, *count * sizeof *array);
    if (!array) return false;
    at = 0;
    for (size_t i = 0; i < *count; ++i) {
        (void)next_word(text, size, &at, &array[i]);
        array[i].exception = i == 0 || i + 1 == *count;
        size_t gap = i ? array[i - 1].end : 0;
        for (size_t j = gap; j < array[i].start; ++j) if (text[j] == ':') array[i].exception = true;
    }
    if (title) for (size_t i = 0; i < *count; ++i) {
        for (size_t s = 0; s < sizeof stop_words / sizeof *stop_words; ++s) {
            size_t end;
            if (stop_match(text, size, array[i].start, stop_words[s], &end))
                for (size_t j = i; j < *count && array[j].start < end; ++j) array[j].stop = true;
        }
    }
    *result = array;
    return true;
}
static bool reserve(Builder *builder, size_t extra) {
    size_t size = builder->result.text.length;
    if (extra > CE_CSL_OUTPUT_MAX_BYTES - size) { builder->memory->limit = true; return false; }
    size_t required = size + extra + 1;
    if (required <= builder->capacity) return true;
    size_t capacity = builder->capacity ? builder->capacity : 128;
    while (capacity < required) {
        if (capacity > (CE_CSL_OUTPUT_MAX_BYTES + 1) / 2) { capacity = CE_CSL_OUTPUT_MAX_BYTES + 1; break; }
        capacity *= 2;
    }
    char *data = allocate(builder->memory, capacity);
    if (!data) return false;
    if (size) memcpy(data, builder->result.text.data, size);
    release(NULL, builder->result.text.data);
    builder->result.text.data = data;
    builder->result.text._deallocate = release;
    builder->result.text._context = NULL;
    builder->capacity = capacity;
    return true;
}
static bool copy_segment(Builder *builder, const char *text, size_t source_offset, size_t size) {
    if (!reserve(builder, size)) return false;
    size_t base = builder->result.text.length, at = 0;
    if (size) memcpy(builder->result.text.data + base, text, size);
    uint32_t cp;
    while (at < size) {
        size_t before = at;
        (void)next(text, size, &at, &cp);
        builder->result.spans[builder->result.span_count++] = (CeCslCaseSpan){source_offset + before, at - before, base + before, at - before};
    }
    builder->result.text.length += size;
    return true;
}
static bool mapped_segment(Builder *builder, const char *text, size_t source_offset, size_t size,
                           CeCslUnicodeOperation operation, char *error, size_t capacity) {
    CeCslUnicodeMappedText mapped = {0};
    CeCslUnicodeStatus status = ce_csl_unicode_transform_mapped(text, size, operation, builder->language, &builder->options, &mapped);
    if (status != CE_CSL_UNICODE_OK) return unicode_fail(status, builder->memory, error, capacity);
    if (!reserve(builder, mapped.text.size)) { ce_csl_unicode_mapped_free(&mapped); return memory_fail(builder->memory, error, capacity); }
    size_t base = builder->result.text.length;
    if (mapped.text.size) memcpy(builder->result.text.data + base, mapped.text.data, mapped.text.size);
    for (size_t i = 0; i < mapped.span_count; ++i) {
        CeCslCaseSpan span = mapped.spans[i];
        span.source_offset += source_offset; span.output_offset += base;
        builder->result.spans[builder->result.span_count++] = span;
    }
    builder->result.text.length += mapped.text.size;
    ce_csl_unicode_mapped_free(&mapped);
    return true;
}
static CeCslCaseMappedText take_unicode(CeCslUnicodeMappedText *input) {
    CeCslCaseMappedText result = {0};
    result.text = (CeCslOutputString){input->text.data, input->text.size, input->text.deallocate, input->text.allocator_context};
    result.spans = input->spans; result.span_count = input->span_count;
    result._deallocate = input->text.deallocate; result._context = input->text.allocator_context;
    memset(input, 0, sizeof *input);
    return result;
}
void ce_csl_case_mapped_free(CeCslCaseMappedText *out) {
    if (!out) return;
    if (out->spans && out->_deallocate) out->_deallocate(out->_context, out->spans);
    ce_csl_output_string_free(&out->text);
    memset(out, 0, sizeof *out);
}
static bool compose(const CeCslCaseMappedText *first, CeCslCaseMappedText *second, Memory *memory) {
    CeCslCaseSpan *spans = first->span_count ? allocate(memory, first->span_count * sizeof *spans) : NULL;
    if (first->span_count && !spans) return false;
    size_t at = 0;
    for (size_t i = 0; i < first->span_count; ++i) {
        const CeCslCaseSpan *original = &first->spans[i];
        size_t middle_end = original->output_offset + original->output_length;
        size_t begin = at < second->span_count ? second->spans[at].output_offset : second->text.length;
        size_t end = begin;
        while (at < second->span_count && second->spans[at].source_offset < middle_end) {
            end = second->spans[at].output_offset + second->spans[at].output_length;
            ++at;
        }
        spans[i] = (CeCslCaseSpan){original->source_offset, original->source_length, begin, end - begin};
    }
    release(NULL, second->spans);
    second->spans = spans; second->span_count = first->span_count;
    return true;
}
bool ce_csl_case_transform_mapped(const char *text, size_t size, CeCslTextCase mode,
    const char *item_language, const char *style_default_locale,
    const CeCslOutputAllocator *allocator, CeCslCaseMappedText *out, char *error, size_t capacity) {
    if (error && capacity) *error = 0;
    Memory memory = {{NULL, default_allocate, default_deallocate}, 0, false};
    if (allocator) memory.allocator = *allocator;
    if (!out || (!text && size) || !memory.allocator.allocate || !memory.allocator.deallocate ||
        mode < CE_CSL_CASE_NONE || mode > CE_CSL_CASE_TITLE)
        return fail(error, capacity, "Invalid CSL text-case argument");
    if (size > CE_CSL_OUTPUT_MAX_BYTES) return fail(error, capacity, "CSL text-case input limit exceeded");
    if (ce_csl_unicode_validate(text, size, NULL) != CE_CSL_UNICODE_OK) return fail(error, capacity, "Malformed UTF-8 in CSL text-case input");
    size_t position = 0; uint32_t cp;
    while (next(text, size, &position, &cp))
        if (!cp || (cp < 32 && cp != '\t' && cp != '\r' && cp != '\n')) return fail(error, capacity, "Unsupported control character in CSL text-case input");
    const char *language = item_language && *item_language ? item_language : style_default_locale;
    CeCslUnicodeOptions options = {CE_CSL_OUTPUT_MAX_BYTES, CE_CSL_OUTPUT_MAX_BYTES, CE_CSL_CASE_MAX_WORKSPACE,
        allocate, release, &memory};
    CeCslCaseMappedText initial = {0}, result = {0};
    Word *word_array = NULL;
    bool success = false;
    if (mode == CE_CSL_CASE_TITLE && !english_item(item_language, style_default_locale)) mode = CE_CSL_CASE_NONE;
    if (mode == CE_CSL_CASE_LOWER || mode == CE_CSL_CASE_UPPER) {
        CeCslUnicodeMappedText mapped = {0};
        CeCslUnicodeStatus status = ce_csl_unicode_transform_mapped(text, size,
            mode == CE_CSL_CASE_LOWER ? CE_CSL_UNICODE_LOWER : CE_CSL_UNICODE_UPPER,
            language, &options, &mapped);
        if (status != CE_CSL_UNICODE_OK) { unicode_fail(status, &memory, error, capacity); goto done; }
        result = take_unicode(&mapped);
    } else {
        const char *working = text ? text : "";
        size_t working_size = size;
        bool lowered = (mode == CE_CSL_CASE_TITLE || mode == CE_CSL_CASE_SENTENCE) && uppercase_string(text, size);
        if (lowered) {
            CeCslUnicodeMappedText mapped = {0};
            CeCslUnicodeStatus status = ce_csl_unicode_transform_mapped(text, size, CE_CSL_UNICODE_LOWER, language, &options, &mapped);
            if (status != CE_CSL_UNICODE_OK) { unicode_fail(status, &memory, error, capacity); goto done; }
            initial = take_unicode(&mapped);
            working = initial.text.data; working_size = initial.text.length;
        }
        size_t count = scalar_count(working, working_size), word_count = 0;
        Builder builder = {0};
        builder.memory = &memory; builder.options = options; builder.language = language;
        builder.result._deallocate = release;
        builder.result.spans = count ? allocate(&memory, count * sizeof *builder.result.spans) : NULL;
        if (count && !builder.result.spans) { memory_fail(&memory, error, capacity); goto done; }
        if (mode != CE_CSL_CASE_NONE && !words(working, working_size, &memory, mode == CE_CSL_CASE_TITLE, &word_array, &word_count)) {
            ce_csl_case_mapped_free(&builder.result); memory_fail(&memory, error, capacity); goto done;
        }
        size_t at = 0;
        bool built = true;
        for (size_t i = 0; i < word_count && built; ++i) {
            const Word *word = &word_array[i];
            built = copy_segment(&builder, working + at, at, word->start - at);
            if (!built) break;
            bool lower = mode == CE_CSL_CASE_TITLE && word->stop && !word->exception;
            bool capitalize = word->lowercase && (mode == CE_CSL_CASE_CAPITALIZE_ALL || mode == CE_CSL_CASE_TITLE || i == 0);
            if (lower) built = mapped_segment(&builder, working + word->start, word->start, word->end - word->start, CE_CSL_UNICODE_LOWER, error, capacity);
            else if (capitalize) {
                size_t head = word->start;
                (void)next(working, working_size, &head, &cp);
                while (head < word->end) {
                    size_t look = head;
                    (void)next(working, working_size, &look, &cp);
                    if (!ce_csl_unicode_is_mark(cp)) break;
                    head = look;
                }
                built = mapped_segment(&builder, working + word->start, word->start, head - word->start, CE_CSL_UNICODE_TITLE_MAPPING, error, capacity);
                if (built) built = copy_segment(&builder, working + head, head, word->end - head);
            } else built = copy_segment(&builder, working + word->start, word->start, word->end - word->start);
            at = word->end;
        }
        if (built) built = copy_segment(&builder, working + at, at, working_size - at);
        if (built && !builder.result.text.data) built = reserve(&builder, 0);
        release(NULL, word_array); word_array = NULL;
        if (!built) {
            ce_csl_case_mapped_free(&builder.result);
            if (!error || !capacity || !*error) memory_fail(&memory, error, capacity);
            goto done;
        }
        builder.result.text.data[builder.result.text.length] = 0;
        result = builder.result;
        if (lowered && !compose(&initial, &result, &memory)) { memory_fail(&memory, error, capacity); goto done; }
    }
    detach(result.text.data); detach(result.spans);
    result.text._context = NULL; result._context = NULL;
    ce_csl_case_mapped_free(out);
    *out = result; memset(&result, 0, sizeof result);
    success = true;
done:
    release(NULL, word_array);
    ce_csl_case_mapped_free(&initial); ce_csl_case_mapped_free(&result);
    return success;
}
bool ce_csl_case_transform(const char *text, size_t size, CeCslTextCase mode,
    const char *item_language, const char *style_default_locale,
    const CeCslOutputAllocator *allocator, CeCslOutputString *out, char *error, size_t capacity) {
    if (!out) return fail(error, capacity, "Invalid CSL text-case destination");
    CeCslCaseMappedText mapped = {0};
    if (!ce_csl_case_transform_mapped(text, size, mode, item_language, style_default_locale, allocator, &mapped, error, capacity)) return false;
    ce_csl_output_string_free(out);
    *out = mapped.text; memset(&mapped.text, 0, sizeof mapped.text);
    ce_csl_case_mapped_free(&mapped);
    return true;
}
