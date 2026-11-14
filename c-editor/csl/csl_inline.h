#ifndef CE_CSL_INLINE_H
#define CE_CSL_INLINE_H
#include "csl_case.h"
#include "csl_output.h"

#define CE_CSL_INLINE_MAX_BYTES CE_CSL_OUTPUT_MAX_BYTES
#define CE_CSL_INLINE_MAX_RUNS CE_CSL_OUTPUT_MAX_RUNS
#define CE_CSL_INLINE_MAX_DEPTH ((size_t)64)
#define CE_CSL_INLINE_MAX_FRAGMENTS ((size_t)65536)
#define CE_CSL_INLINE_MAX_WORKSPACE ((size_t)128 * 1024 * 1024)

typedef struct CeCslInline CeCslInline;
typedef struct CeCslInlineRun {
    size_t offset, length; /* UTF-8 bytes in the object's contiguous plain text. */
    unsigned format;       /* Final effective CeCslOutputFormat. */
    const char *href;      /* Copied, validated link from from_output; else NULL. */
    bool nocase;
} CeCslInlineRun;
typedef struct CeCslInlineDiagnostic {
    bool literal_markup;   /* Unknown/unsupported tag-like token was retained. */
    bool malformed_markup; /* Known tags mismatch/unclosed: whole field literal. */
} CeCslInlineDiagnostic;
typedef struct CeCslInlineMap {
    CeCslCaseSpan *spans; /* One per ORIGINAL plain-text scalar, never raw markup. */
    size_t count, source_size, output_size;
    void (*_deallocate)(void *context, void *memory);
    void *_context;
} CeCslInlineMap;

/* Owned UTF-8 rich fields, not an HTML parser or a full citation evaluator.
 * Recognizes lowercase i/b/sc/sup/sub and span with exactly class='nocase' or
 * style='font-variant: small-caps;' (single/double quotes and ASCII whitespace).
 * sc is an explicit compatibility alias. i/b/sc flip the CURRENT respective
 * property for every nesting level. i clears conflicting oblique; b clears
 * light. sup/sub set mutually exclusive vertical alignment. nocase is inherited.
 * Unknown tags, attributes, nodecor, entities and script-like text remain plain
 * text; entities are NOT decoded and no URLs, CSS or scripts are executed.
 * A mismatched/unclosed recognized tag makes the WHOLE raw field plain text,
 * with malformed_markup=true; original bytes remain intact at parse time.
 * Unrecognized span frames are balanced separately, so their closing tag cannot
 * accidentally close a recognized nocase/small-caps span.
 *
 * inherited_format must be the effective style at final leaf flattening: later
 * overwriting font bits would destroy inline flip-flops. Unknown/malformed text
 * still participates in later text-case as ordinary text. All whitespace is
 * retained. Invalid UTF-8, NUL and unsupported C0 controls are errors.
 *
 * Initialize *out to NULL or a live object, diagnostic to any value. Every
 * replace/mutation is atomic, including optional diagnostic/map outputs. Inputs
 * are borrowed only during calls; results own text, runs and links. NULL
 * allocator selects malloc/free. Custom allocator context outlives results.
 * Source/old caller-owned objects are excluded from workspace; all allocations
 * caused by an operation, including still-live nested case work and rebuilt
 * maps/text/runs, share MAX_WORKSPACE. Payload includes text and copied links.
 * No thread-global state. Views are invalidated by a successful mutation/free. */
bool ce_csl_inline_parse(const char *raw, size_t size, unsigned inherited_format,
                         const CeCslOutputAllocator *allocator, CeCslInline **out,
                         CeCslInlineDiagnostic *diagnostic, char *error, size_t capacity);
bool ce_csl_inline_from_output(const CeCslOutput *source, bool nocase, const CeCslOutputAllocator *allocator,
                               CeCslInline **out, char *error, size_t capacity);
bool ce_csl_inline_clone(const CeCslInline *source, const CeCslOutputAllocator *allocator, CeCslInline **out,
                         char *error, size_t capacity);
/* Copies a plain UTF-8 byte range, retaining effective formatting/link/nocase.
 * Both boundaries must align with scalars (combining sequences may be split).
 * Offset may equal text size only with length zero. Empty slices are valid.
 * Locates the first run by binary search, then visits only intersecting runs.
 * source may equal *out. No raw-tag coordinates or Unicode normalization. */
bool ce_csl_inline_slice(const CeCslInline *source, size_t offset, size_t length,
                         const CeCslOutputAllocator *allocator, CeCslInline **out, char *error,
                         size_t capacity);
/* Geometric storage; appends only new payloads, not a deep copy on every call.
 * Self-concat is supported. Adjacent identical format/link/nocase runs merge. */
bool ce_csl_inline_concat(CeCslInline *destination, const CeCslInline *source, char *error, size_t capacity);
void ce_csl_inline_free(CeCslInline *text);
const char *ce_csl_inline_text(const CeCslInline *text, size_t *size);
size_t ce_csl_inline_run_count(const CeCslInline *text);
const CeCslInlineRun *ce_csl_inline_run(const CeCslInline *text, size_t index);

/* Converts the whole plain text ONCE, then restores original bytes for nocase
 * scalars and rebuilds formatting/protection/map together. Protected text still
 * participates in Unicode context, word boundaries and whole-string uppercase
 * detection; it is not replaced with placeholders or treated as separate runs.
 * Map covers all original and final bytes monotonically, including expansion
 * and deletion. Only scalar-aligned ranges are supported, not NFC composition.
 * source may equal *out. Optional map must start at {0} or hold a live map. */
bool ce_csl_inline_case(const CeCslInline *source, CeCslTextCase mode, const char *item_language,
                        const char *style_default_locale, const CeCslOutputAllocator *allocator,
                        CeCslInline **out, CeCslInlineMap *map, char *error, size_t capacity);
/* Removes only U+002E '.', including within nocase runs (which protect CASE,
 * not punctuation). No entity decoding or changes to other period-like chars.
 * Preserves format/link/protection and merges newly adjacent identical runs.
 * Linear pass over text/runs. Optional map has one record per source scalar,
 * including zero-length deletions. Result and map are replaced atomically and
 * share the operation workspace budget; source may equal *out. */
bool ce_csl_inline_strip_periods(const CeCslInline *source, const CeCslOutputAllocator *allocator,
                                 CeCslInline **out, CeCslInlineMap *map, char *error, size_t capacity);
void ce_csl_inline_map_free(CeCslInlineMap *map);
bool ce_csl_inline_map_range(const CeCslInlineMap *map, size_t offset, size_t length, size_t *new_offset,
                             size_t *new_length);
/* Final downgrade only: appends controlled IR atomically, discarding nocase.
 * Keep the inline object for any future parent text-case transformations. */
bool ce_csl_inline_to_output(const CeCslInline *source, CeCslOutput *destination, char *error,
                             size_t capacity);
#endif
