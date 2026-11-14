#ifndef CE_CSL_CASE_H
#define CE_CSL_CASE_H

#include "csl_output.h"
#include "csl_unicode.h"

#define CE_CSL_CASE_MAX_WORKSPACE ((size_t)128 * 1024 * 1024)

typedef enum CeCslTextCase {
    CE_CSL_CASE_NONE, CE_CSL_CASE_LOWER, CE_CSL_CASE_UPPER,
    CE_CSL_CASE_CAPITALIZE_FIRST, CE_CSL_CASE_CAPITALIZE_ALL,
    CE_CSL_CASE_SENTENCE, CE_CSL_CASE_TITLE
} CeCslTextCase;

typedef CeCslUnicodeSpan CeCslCaseSpan;
typedef struct CeCslCaseMappedText {
    CeCslOutputString text;
    CeCslCaseSpan *spans;
    size_t span_count;
    void (*_deallocate)(void *context, void *memory);
    void *_context;
} CeCslCaseMappedText;

/* Plain UTF-8 text, without markup. Protected/nocase spans are an upper layer's
 * responsibility. Do not independently transform font runs: title/sentence and
 * Unicode context need the whole text. Empty input is valid; embedded NUL and
 * unsupported C0 controls are rejected. Input/output are bounded to
 * CE_CSL_OUTPUT_MAX_BYTES; no host locale or network lookup is performed.
 *
 * Title casing applies only to English-language items, using item_language
 * when nonempty and style_default_locale otherwise (an absent default is en).
 * Stop words come from the pinned CSL schema. This layer provides CSL casing
 * rules with a word tokenizer for Unicode letters/marks/numbers/connector
 * punctuation and internal apostrophes. Hyphens/dashes separate word parts;
 * the schema's d' prefix is a separate part. Multiword stop phrases can span
 * Unicode whitespace; curly apostrophes match their ASCII stop-word counterpart.
 * Stop-word first/last/colon exceptions apply to individual word parts. This is
 * not full UAX29 segmentation or
 * a parser for rich-text markup, URLs, protected names, or arbitrary scripts.
 * Unicode full case mappings preserve normalization unless a case mapping
 * itself changes the scalar sequence. No blanket NFC normalization occurs.
 * Temporary buffers AND scalar maps share MAX_WORKSPACE, including retained
 * intermediate maps during composition. English gate recognizes primary en
 * with a '-' or '_' boundary, case-insensitively, not words such as 'english'.
 *
 * Output must start at {0} or hold a live result. Success atomically replaces
 * it; every failure preserves it. Allocator/ownership follows csl_output.h.
 * The corresponding free function must release each returned object. */
bool ce_csl_case_transform(const char *text, size_t size, CeCslTextCase mode,
    const char *item_language, const char *style_default_locale,
    const CeCslOutputAllocator *allocator, CeCslOutputString *out,
    char *error, size_t capacity);

/* One span per original Unicode scalar, including output_length=0 for deleted
 * scalars. Offsets/lengths count UTF-8 bytes. Spans are ordered, nonoverlapping
 * and cover all input and output; expansions belong to their originating
 * scalar. This lets a renderer retain source formatting after a whole-string
 * transform. Callers must not split source font/style runs inside a scalar.
 * The map and text are independently owned; allocator context must outlive both.
 * Results require zero-initialization and are atomically replaced on success. */
bool ce_csl_case_transform_mapped(const char *text, size_t size, CeCslTextCase mode,
    const char *item_language, const char *style_default_locale,
    const CeCslOutputAllocator *allocator, CeCslCaseMappedText *out,
    char *error, size_t capacity);
void ce_csl_case_mapped_free(CeCslCaseMappedText *out);

#endif
