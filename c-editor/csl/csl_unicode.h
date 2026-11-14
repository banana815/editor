#ifndef CE_CSL_UNICODE_H
#define CE_CSL_UNICODE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define CE_CSL_UNICODE_VERSION "18.0.0"
#define CE_CSL_UNICODE_DEFAULT_MAX_BYTES ((size_t)8 * 1024 * 1024)
#define CE_CSL_UNICODE_DEFAULT_MAX_WORKSPACE ((size_t)128 * 1024 * 1024)

typedef enum CeCslUnicodeStatus {
    CE_CSL_UNICODE_OK = 0,
    CE_CSL_UNICODE_END,
    CE_CSL_UNICODE_INVALID_ARGUMENT,
    CE_CSL_UNICODE_INVALID_UTF8,
    CE_CSL_UNICODE_LIMIT,
    CE_CSL_UNICODE_OOM
} CeCslUnicodeStatus;

typedef enum CeCslUnicodeOperation {
    CE_CSL_UNICODE_NFC,
    CE_CSL_UNICODE_UPPER,
    CE_CSL_UNICODE_LOWER,
    CE_CSL_UNICODE_CASEFOLD,
    /* Full title mapping of each scalar; this does NOT segment/case words. */
    CE_CSL_UNICODE_TITLE_MAPPING
} CeCslUnicodeOperation;

typedef struct CeCslUnicodeOptions {
    /* Zero selects the documented default. Limits exclude the NUL sentinel.
     * Workspace bounds temporary storage; output has its separate byte limit. */
    size_t max_input_bytes, max_output_bytes, max_workspace_bytes;
    /* Supply both callbacks or neither. Defaults are malloc/free; allocate must
     * provide malloc-compatible alignment. The context must outlive the output.
     * No global allocator state is modified. */
    void *(*allocate)(void *context, size_t size);
    void (*deallocate)(void *context, void *pointer);
    void *allocator_context;
} CeCslUnicodeOptions;

typedef struct CeCslUnicodeText {
    char *data;
    size_t size;
    void (*deallocate)(void *context, void *pointer);
    void *allocator_context;
} CeCslUnicodeText;

typedef struct CeCslUnicodeSpan {
    size_t source_offset, source_length, output_offset, output_length;
} CeCslUnicodeSpan;
typedef struct CeCslUnicodeMappedText {
    CeCslUnicodeText text;
    CeCslUnicodeSpan *spans;
    size_t span_count;
} CeCslUnicodeMappedText;

/* Byte-counted Unicode permits embedded U+0000; output has one extra NUL.
 * NULL input is valid only with size 0. On failure, out and input are unchanged.
 * out must be zero-initialized/empty on success (never silently replaces an
 * owned result). language is NULL/root or a BCP47 language tag; tr/az/lt are
 * recognized case-insensitively, with '-' and '_' equivalent. Other languages
 * use Unicode default casing. Casefold uses C+F and optional tr/az T mappings;
 * it is not lowercase and does not promise NFC. Normalize explicitly if needed.
 * This module implements neither locale collation nor CSL title/sentence case. */
CeCslUnicodeStatus ce_csl_unicode_transform(const char *input, size_t size,
    CeCslUnicodeOperation operation, const char *language,
    const CeCslUnicodeOptions *options, CeCslUnicodeText *out);
void ce_csl_unicode_text_free(CeCslUnicodeText *text);
/* Casing-only scalar map, one record per input scalar including zero-length
 * deletions. This API rejects NFC: normalization can reorder and merge source
 * scalars and needs a different mapping contract. out must be empty; failure
 * preserves it. Both the codepoint workspace and returned span array count
 * toward max_workspace_bytes. The allocator context outlives text and spans. */
CeCslUnicodeStatus ce_csl_unicode_transform_mapped(const char *input, size_t size,
    CeCslUnicodeOperation operation, const char *language,
    const CeCslUnicodeOptions *options, CeCslUnicodeMappedText *out);
void ce_csl_unicode_mapped_free(CeCslUnicodeMappedText *out);
const char *ce_csl_unicode_status_string(CeCslUnicodeStatus status);

/* Validation rejects overlong forms, surrogates, truncated sequences and values
 * above U+10FFFF. Noncharacters and unassigned scalar values remain valid.
 * bad_offset receives the first malformed sequence's start, or size on success. */
CeCslUnicodeStatus ce_csl_unicode_validate(const char *input, size_t size,
                                         size_t *bad_offset);
/* next returns END at size; neither offset nor codepoint changes on error/END. */
CeCslUnicodeStatus ce_csl_unicode_next(const char *input, size_t size,
                                     size_t *offset, uint32_t *codepoint);
/* Writes no bytes on invalid scalar; at most 4 bytes otherwise. */
size_t ce_csl_unicode_encode(uint32_t codepoint, char output[4]);
bool ce_csl_unicode_is_scalar(uint32_t codepoint);
bool ce_csl_unicode_is_space(uint32_t codepoint);
bool ce_csl_unicode_is_letter(uint32_t codepoint);
bool ce_csl_unicode_is_mark(uint32_t codepoint);
bool ce_csl_unicode_is_number(uint32_t codepoint);
bool ce_csl_unicode_is_cased(uint32_t codepoint);
bool ce_csl_unicode_is_case_ignorable(uint32_t codepoint);
/* Category helper: Letter/Mark/Number/Connector_Punctuation. This is not UAX29
 * word segmentation; apostrophe/hyphen policy belongs to the caller. */
bool ce_csl_unicode_is_word_codepoint(uint32_t codepoint);

#endif
