#ifndef CE_CSL_COLLATION_H
#define CE_CSL_COLLATION_H

#include "csl_sort.h"

#define CE_CSL_COLLATION_MAX_TEXT_BYTES ((size_t)64 * 1024)
#define CE_CSL_COLLATION_MAX_NONSTARTERS ((size_t)64)
#define CE_CSL_COLLATION_MAX_WORKSPACE_BYTES ((size_t)1024 * 1024)
#define CE_CSL_COLLATION_MAX_KEY_BYTES ((size_t)512 * 1024)
#define CE_CSL_COLLATION_PROFILE "Unicode-18.0.0/DUCET/secondary/non-ignorable/full-fold/NFD/v1"

/* This controls only the Unicode case-folding rules. It does not select CLDR
 * collation tailoring, Chinese pinyin, Swedish ordering, or a system locale. */
typedef enum CeCslCollationLanguage {
    CE_CSL_COLLATION_DEFAULT, CE_CSL_COLLATION_TR, CE_CSL_COLLATION_AZ
} CeCslCollationLanguage;

/* Bounded, case-insensitive Unicode DUCET key. The pipeline is strict UTF-8
 * validation, full CASEFOLD, linear NFD, then SECONDARY/NON_IGNORABLE UCA.
 * Accents retain their secondary weights. There is no language-specific
 * collation tailoring; TR/AZ affect folding only. Compare only keys produced
 * with the same profile and language, and regenerate keys after a profile or
 * dependency update. Keys are binary and can contain NUL.
 *
 * Input, folded text, and NFD text each have a 64 KiB limit. Canonical
 * decomposition of the input and folded text may contain at most 64 consecutive
 * nonstarters; decomposition is checked before normalization. Unicode workspace
 * is limited to 1 MiB and the returned key to 512 KiB. Limits cause explicit
 * failure, never truncation or fallback to a different ordering. NUL and C0
 * controls other than TAB/LF/CR are rejected; malformed UTF-8 is not repaired.
 *
 * Initialize out to {0} or a live CeCslSortKey. Success atomically replaces it,
 * even when input aliases the old key. Failure preserves the old key. Empty
 * text and nonempty all-ignorable text can both produce a zero-length key;
 * semantic missingness belongs to the evaluator, not this function.
 *
 * NULL allocator selects malloc/free; otherwise both callbacks are required.
 * The caller allocator covers Unicode buffers and the final key. Mojibake uses
 * its existing allocator: this function never reads or mutates its process
 * configuration. Its internal temporary allocations are NOT subject to our
 * workspace or returned-key caps. This is a per-key bound, not a whole-sort
 * memory/time budget; see COLLATION.md. */
bool ce_csl_collation_key(const char *text, size_t size,
    CeCslCollationLanguage language, const CeCslOutputAllocator *allocator,
    CeCslSortKey *out, char *error, size_t capacity);

#endif
