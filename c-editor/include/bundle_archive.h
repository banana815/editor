#ifndef CE_BUNDLE_ARCHIVE_H
#define CE_BUNDLE_ARCHIVE_H
#include <stdbool.h>
#include <stddef.h>
#define CE_BUNDLE_MAX_BYTES ((size_t)128*1024*1024)
#define CE_BUNDLE_MAX_ENTRIES 10000u
typedef struct { const char *name; const unsigned char *data; size_t size; } CeBundleEntry;
typedef struct { CeBundleEntry *entries; size_t count; } CeBundle;
/* ZIP STORE only, UTF-8 names, bounded memory. Reader validates all headers,
 * CRCs, relative portable paths, aliases and file/directory collisions before
 * returning owned entries. No filesystem access. Empty files are supported.
 * Outputs are untouched on failure. Caller frees write buffer with free(). */
bool ce_bundle_valid_path(const char *path);
bool ce_bundle_write(const CeBundleEntry *entries,size_t count,unsigned char **out,size_t *size,char *error,size_t capacity);
bool ce_bundle_read(const void *bytes,size_t size,CeBundle *out,char *error,size_t capacity);
void ce_bundle_free(CeBundle *bundle);
#endif
