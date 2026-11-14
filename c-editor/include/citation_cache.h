#ifndef CE_CITATION_CACHE_H
#define CE_CITATION_CACHE_H

#include "csl_bibliography.h"

#define CE_CITATION_CACHE_VERSION 1
#define CE_CITATION_PROCESSOR_PROFILE "notelab-csl/1"
#define CE_CITATION_CACHE_MAX_BYTES ((size_t)16 * 1024 * 1024)
#define CE_CITATION_CACHE_MAX_RUNS ((size_t)262144)
#define CE_CITATION_CACHE_MAX_ROWS ((size_t)131072)
#define CE_CITATION_CACHE_MAX_ENTRIES ((size_t)50000)
#define CE_CITATION_CACHE_MAX_WORKSPACE ((size_t)256 * 1024 * 1024)

typedef enum CeCitationCacheKind {
    CE_CITATION_CACHE_CITATION,
    CE_CITATION_CACHE_BIBLIOGRAPHY
} CeCitationCacheKind;
typedef struct CeCitationCacheHeader {
    const char *processor_profile;
    const char *profile;
    const char *style_id;
    const char *style_sha256;
    const char *style_commit;
    const char *locale;
    const char *locale_commit;
    const char *source_digest;
} CeCitationCacheHeader;
typedef struct CeCitationCacheRow {
    CeCslBibliographyRowKind kind;
    const CeCslOutput *left; /* Non-NULL only for TWO_COLUMNS. */
    const CeCslOutput *body;
} CeCitationCacheRow;
typedef struct CeCitationCacheEntry {
    const char *source_item_id;
    uint64_t number;
    bool visible;
    size_t row_count;
    const CeCitationCacheRow *rows;
} CeCitationCacheEntry;
typedef struct CeCitationCacheBibliographySource {
    const char *source_item_id;
    uint64_t number;
    bool visible;
    const CeCslBibliographyEntry *entry;
} CeCitationCacheBibliographySource;
typedef struct CeCitationCache CeCitationCache;

/* All results own copied metadata, strings, rows and validated output runs.
 * Inputs are borrowed only during the call. Initialize *out to NULL/live;
 * failures preserve the old cache and every borrowed view. Allocator context
 * must survive until free. The strict JSON parser/cJSON use their existing
 * independent allocators, bounded by the JSON byte/node/depth limits. The
 * workspace constant bounds this cache's owned allocator, excluding that
 * parser/tree and previous results. Encoded strings own independent malloc
 * storage. Strings serialized directly from borrowed output views must be
 * released before their cache, because those views use its allocator context.
 *
 * Decode verifies v1's controlled representation, UTF-8, known format flags,
 * mutually exclusive style flags and validated absolute HTTP(S) links. It does
 * NOT authenticate cache provenance or establish freshness: the application
 * must bind header/source_digest to the current complete citation revision,
 * and rebuild imported documents before presenting standards-based output.
 * Unknown schema/processor versions fail explicitly without rewriting input.
 * Empty arrays are valid. No HTML/CSS, pixel/font data or evaluation pointers
 * are read or retained. */
bool ce_citation_cache_decode(const char *json, size_t size, const CeCslOutputAllocator *allocator,
                              CeCitationCache **out, char *error, size_t capacity);
bool ce_citation_cache_encode(const CeCitationCache *cache, CeCslOutputString *out, char *error,
                              size_t capacity);
bool ce_citation_cache_from_citation(const CeCitationCacheHeader *header, const char *citation_id,
                                     const CeCslOutput *output, const CeCslOutputAllocator *allocator,
                                     CeCitationCache **out, char *error, size_t capacity);
bool ce_citation_cache_from_bibliography(const CeCitationCacheHeader *header,
                                         const CeCslBibliographyOptions *options,
                                         const CeCitationCacheBibliographySource *entries, size_t count,
                                         const CeCslOutputAllocator *allocator, CeCitationCache **out,
                                         char *error, size_t capacity);
void ce_citation_cache_free(CeCitationCache *cache);

/* Borrowed immutable views, valid until free/successful replacement. Invalid
 * object/kind/index queries return NULL/zero (kind(NULL) is CITATION). */
CeCitationCacheKind ce_citation_cache_kind(const CeCitationCache *cache);
const CeCitationCacheHeader *ce_citation_cache_header(const CeCitationCache *cache);
const char *ce_citation_cache_citation_id(const CeCitationCache *cache);
const CeCslOutput *ce_citation_cache_citation_output(const CeCitationCache *cache);
const CeCslBibliographyOptions *ce_citation_cache_bibliography_options(const CeCitationCache *cache);
size_t ce_citation_cache_entry_count(const CeCitationCache *cache);
const CeCitationCacheEntry *ce_citation_cache_entry(const CeCitationCache *cache, size_t index);

#endif
