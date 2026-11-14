#ifndef CE_CSL_BIBLIOGRAPHY_H
#define CE_CSL_BIBLIOGRAPHY_H
#include "csl_finalize.h"
#include <stdint.h>

#define CE_CSL_BIBLIOGRAPHY_MAX_ENTRY_BYTES CE_CSL_OUTPUT_MAX_BYTES
#define CE_CSL_BIBLIOGRAPHY_MAX_ENTRIES CE_CSL_INPUT_MAX_ITEMS
#define CE_CSL_BIBLIOGRAPHY_MAX_ROWS ((size_t)131072)
#define CE_CSL_BIBLIOGRAPHY_MAX_NODES ((size_t)1048576)
#define CE_CSL_BIBLIOGRAPHY_MAX_TEXT_BYTES ((size_t)16 * 1024 * 1024)
#define CE_CSL_BIBLIOGRAPHY_MAX_HTML_BYTES ((size_t)64 * 1024 * 1024)
#define CE_CSL_BIBLIOGRAPHY_MAX_WORKSPACE ((size_t)128 * 1024 * 1024)

typedef struct CeCslBibliographyEntry CeCslBibliographyEntry;
typedef enum CeCslBibliographyAlignment {
    CE_CSL_BIBLIOGRAPHY_ALIGN_NONE,
    CE_CSL_BIBLIOGRAPHY_ALIGN_FLUSH,
    CE_CSL_BIBLIOGRAPHY_ALIGN_MARGIN
} CeCslBibliographyAlignment;
typedef enum CeCslBibliographyRowKind {
    CE_CSL_BIBLIOGRAPHY_FLOW,
    CE_CSL_BIBLIOGRAPHY_BLOCK,
    CE_CSL_BIBLIOGRAPHY_INDENT,
    CE_CSL_BIBLIOGRAPHY_TWO_COLUMNS
} CeCslBibliographyRowKind;
typedef struct CeCslBibliographyOptions {
    CeCslBibliographyAlignment alignment;
    bool hanging_indent;
    uint32_t line_spacing, entry_spacing;
} CeCslBibliographyOptions;
typedef struct CeCslBibliographyCell {
    const CeCslOutput *output;         /* Owned by entry, including text/styles/links. */
    size_t source_node_index;          /* Evaluation index, SIZE_MAX for combined ranges. */
    size_t final_offset, final_length; /* Original finalized UTF-8 coordinates. */
} CeCslBibliographyCell;
typedef struct CeCslBibliographyRow {
    CeCslBibliographyRowKind kind;
    CeCslBibliographyCell left, body; /* left.output is NULL outside TWO_COLUMNS. */
} CeCslBibliographyRow;
typedef struct CeCslBibliographySource {
    size_t source_node_index, person_index, offset, length;
    CeCslFinalSpanKind kind;
    CeCslEvalDisplay display;
} CeCslBibliographySource;
typedef enum CeCslBibliographyPlainMode {
    CE_CSL_BIBLIOGRAPHY_PLAIN_TABS,
    CE_CSL_BIBLIOGRAPHY_PLAIN_COMPACT
} CeCslBibliographyPlainMode;
typedef struct CeCslBibliographyHtmlOptions {
    /* Presentation metrics in 1/1000 em, independent of CSL's integer factors.
     * Zero selects defaults: indent=2000, gap=500, base line height=1200.
     * label_width=0 uses shared automatic columns for flush/explicit display.
     * MARGIN requires a caller-measured positive label width; never estimated
     * from bytes, Unicode scalar counts or the citation-number spelling. */
    uint32_t indent_milli_em, column_gap_milli_em, base_line_height_milli_em;
    uint32_t label_width_milli_em;
} CeCslBibliographyHtmlOptions;

/* Streaming entry adapter. finalized must be a standalone bibliography layout
 * evaluation from style (not a macro entry, citation, join or foreign style).
 * Copies row/cell outputs, options and numeric provenance; retains no pointers
 * to style, evaluation or finalization. They may be freed immediately on success.
 * Empty entries are valid: zero rows/text bytes, with source item index retained.
 * Custom allocator context must outlive the entry. Strings exported through
 * borrowed cell-output views must be released BEFORE freeing the entry. All
 * failures preserve an initialized NULL/live *out.
 *
 * First-field alignment follows semantic rendering nodes: choose is transparent,
 * group/macro remain one field, and an empty first field is not replaced by the
 * next nonempty field. Explicit display supports disjoint unnested blocks,
 * indentation and adjacent left-margin/right-inline pairs. Nested display,
 * conflicting alignment modes, uncontrolled display gaps and overlapping
 * cross-column punctuation ranges fail explicitly. This does not shape fonts,
 * wrap physical lines, sort entries or substitute repeated authors. */
bool ce_csl_bibliography_entry_build(const CeCslStyle *style, const CeCslFinalized *finalized,
                                     size_t source_item_index, const CeCslOutputAllocator *allocator,
                                     CeCslBibliographyEntry **out, char *error, size_t capacity);
void ce_csl_bibliography_entry_free(CeCslBibliographyEntry *entry);
size_t ce_csl_bibliography_entry_text_bytes(const CeCslBibliographyEntry *entry);
size_t ce_csl_bibliography_entry_source_item_index(const CeCslBibliographyEntry *entry);
const CeCslBibliographyOptions *ce_csl_bibliography_entry_options(const CeCslBibliographyEntry *entry);
size_t ce_csl_bibliography_entry_row_count(const CeCslBibliographyEntry *entry);
const CeCslBibliographyRow *ce_csl_bibliography_entry_row(const CeCslBibliographyEntry *entry, size_t row);
size_t ce_csl_bibliography_entry_source_count(const CeCslBibliographyEntry *entry);
const CeCslBibliographySource *ce_csl_bibliography_entry_source(const CeCslBibliographyEntry *entry,
                                                                size_t index);

/* Borrow an ordered array of independently owned entries, without transferring
 * ownership. Zero entries is valid. All entries must have compatible options.
 * Empty entries remain in the caller's array but emit no text/spacing.
 * Plain: TAB (or one ASCII space in COMPACT) between columns, LF between logical
 * rows, 1+entry_spacing LFs between entries, no trailing LF. Cell whitespace is
 * never trimmed. line_spacing/hanging indentation remain structural metadata;
 * plain export does not invent soft-wrap widths or physical indentation.
 * HTML: fixed structure/CSS and escaped controlled output runs only. Shared
 * columns align labels across entries; margin mode requires measured metrics.
 * Each export owns a single atomic string using the supplied allocator (NULL
 * selects malloc/free), released by ce_csl_output_string_free. Input entries
 * and a previous output string are excluded from operation workspace. Aggregate
 * limits apply across the whole array; per-entry payload remains 1 MiB. */
bool ce_csl_bibliography_plain(const CeCslBibliographyEntry *const *entries, size_t count,
                               CeCslBibliographyPlainMode mode, const CeCslOutputAllocator *allocator,
                               CeCslOutputString *out, char *error, size_t capacity);
bool ce_csl_bibliography_html(const CeCslBibliographyEntry *const *entries, size_t count,
                              const CeCslBibliographyHtmlOptions *options,
                              const CeCslOutputAllocator *allocator, CeCslOutputString *out, char *error,
                              size_t capacity);
#endif
