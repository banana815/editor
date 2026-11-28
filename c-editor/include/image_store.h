#ifndef CE_IMAGE_STORE_H
#define CE_IMAGE_STORE_H
#include "note_store.h"
#include "image_codec.h"
#include <stdint.h>
typedef struct { NoteStore files; uint64_t revision; char error[256]; } CeImageStore;
bool ce_image_store_open(CeImageStore *,const char *notes_root);
void ce_image_store_close(CeImageStore *);
/* Validates and deduplicates content. Never overwrites an existing asset.
 * A later failed document insertion may leave an unused immutable asset. */
bool ce_image_store_add(CeImageStore *,const void *,size_t,char uri[83],CeImageInfo *);
bool ce_image_store_read(CeImageStore *,const char *uri,unsigned char **bytes,size_t *size);
/* Validate exact managed URI; borrowed filename result, no path components. */
const char *ce_image_asset_name(const char *uri);
/* malloc-owned JSON atom; width 0 fits available space, otherwise 1..2400 px. */
char *ce_image_node_json(const char *uri,const char *alt,const char *caption,unsigned width);
#endif
