#ifndef CE_CSL_SORT_H
#define CE_CSL_SORT_H

#include "csl_output.h"
#include <stdint.h>

#define CE_CSL_SORT_MAX_ROWS ((size_t)100000)
#define CE_CSL_SORT_MAX_COLUMNS ((size_t)64)
#define CE_CSL_SORT_MAX_KEY_BYTES ((size_t)512 * 1024)
#define CE_CSL_SORT_MAX_TOTAL_KEY_BYTES ((size_t)64 * 1024 * 1024)
#define CE_CSL_SORT_MAX_INTEGER_DIGITS ((size_t)4096)
#define CE_CSL_SORT_MAX_KEY_PARTS ((size_t)65536)

/* Binary key, not a C string. Initialize to {0}; free with key_free. Callback
 * fields are private. As with output objects, the allocator context is borrowed
 * and must outlive the key. Keys may contain NUL and are compared bytewise. */
typedef struct CeCslSortKey {
    unsigned char *data;
    size_t length;
    void (*_deallocate)(void *context, void *memory);
    void *_context;
} CeCslSortKey;

typedef struct CeCslSortKeyView {
    const unsigned char *data;
    size_t length;
    /* Semantic missingness is separate from a zero-length collation key.
     * Missing keys require length=0; their data pointer is ignored. */
    bool missing;
} CeCslSortKeyView;

typedef struct CeCslSortOrder {
    size_t *indices;
    size_t count;
    void (*_deallocate)(void *context, void *memory);
    void *_context;
} CeCslSortOrder;

/* Copies a binary key atomically, including when bytes aliases the old key.
 * NULL allocator means malloc/free; otherwise both callbacks are required. */
bool ce_csl_sort_key_copy(const unsigned char *bytes, size_t size,
    const CeCslOutputAllocator *allocator, CeCslSortKey *out,
    char *error, size_t capacity);
void ce_csl_sort_key_free(CeCslSortKey *key);
void ce_csl_sort_order_free(CeCslSortOrder *order);

/* Compose a lexicographic tuple of already compatible binary keys. Component
 * boundaries are encoded, so a shorter family/part compares before a longer
 * one before examining subsequent parts. Embedded NUL is escaped; encodings
 * cannot collide by concatenating different component boundaries. Complete
 * component ties proceed to the next component; a shorter tuple precedes its
 * extension. Empty components are allowed. Semantic missing components are
 * rejected: callers must explicitly choose their presence/empty-part policy.
 *
 * This preserves component byte order; it does not generate name/date keys,
 * choose language rules, or define mixed-type ordering. Packed output is at
 * most MAX_KEY_BYTES, including escape bytes and two bytes per component.
 * Count=0 produces a present empty binary key. Ownership/atomicity match copy;
 * parts may borrow from the old output key until a successful replacement. */
bool ce_csl_sort_key_sequence(const CeCslSortKeyView *parts, size_t count,
    const CeCslOutputAllocator *allocator, CeCslSortKey *out,
    char *error, size_t capacity);

/* Stable, lexicographic, row-major matrix sort. All keys must already have
 * been generated successfully. Comparison allocates nothing and does no locale
 * work. Each column must use one compatible key encoding: these utilities do
 * not define an ordering between integer/date/collation encodings.
 *
 * Missing values sort after present values in BOTH directions. Two missing
 * values tie for this column; later columns may distinguish them. Complete
 * ties retain input row order. descending may be NULL (all ascending). With
 * columns=0, keys/descending may be NULL and the result is input order.
 *
 * Borrowed keys are unchanged. Length limits count every matrix cell, including
 * shared buffers; a missing cell must have length=0. Returns owned original row
 * indices. Initialize out to {0} or a live order; failure preserves it. Two
 * temporary index arrays bound sorting workspace to 2*rows*sizeof(size_t).
 * Runtime is O(rows*log(rows)*columns*compared_key_bytes), not a time budget. */
bool ce_csl_sort_order(const CeCslSortKeyView *keys, size_t rows, size_t columns,
    const bool *descending, const CeCslOutputAllocator *allocator,
    CeCslSortOrder *out, char *error, size_t capacity);

/* Exact signed integer order, without floating point or host integer limits.
 * Grammar: optional '+'/'-', then 1..4096 ASCII digits. Leading zeros and
 * negative zero compare equal to their canonical value. No spaces/decimals,
 * ranges, prefixes, exponents or text fallback: the evaluator selects the
 * appropriate typed/text path before calling this utility. */
bool ce_csl_sort_key_integer(const char *decimal, size_t length,
    const CeCslOutputAllocator *allocator, CeCslSortKey *out,
    char *error, size_t capacity);

typedef struct CeCslSortDatePoint {
    int64_t year; /* -999999999..999999999, excluding zero. */
    unsigned month; /* 0 (absent), 1..12, or season 21..24 (ignored). */
    unsigned day; /* 0 (absent) or 1..31; requires a calendar month. */
} CeCslSortDatePoint;

/* Chronological key for a complete variable date. Missing trailing parts sort
 * before present parts. Seasons compare as year-only. A single date precedes
 * a range with the same start; range ends break remaining ties. A NULL end
 * means a single date. BC years sort numerically, not by rendered magnitude.
 * No calendar normalization or raw/literal parsing. Year zero/open endpoints
 * are rejected; callers handle semantic missingness separately. This helper
 * does not yet construct macro keys containing selected subsets of date parts.
 * All key builders replace out atomically, with the same ownership as copy. */
bool ce_csl_sort_key_date(const CeCslSortDatePoint *start,
    const CeCslSortDatePoint *end, const CeCslOutputAllocator *allocator,
    CeCslSortKey *out, char *error, size_t capacity);

#endif
