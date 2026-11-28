#ifndef CE_IMAGE_CLIPBOARD_H
#define CE_IMAGE_CLIPBOARD_H
#include "image_store.h"
#include "document_slice.h"
/* Embedding adds reserved transient image bytes only to a private clipboard
 * slice. Import validates bytes/hash and removes them before document commit.
 * No network fetch, model mutation, or clipboard access; output owned by caller. */
bool ce_image_slice_transfer(CeImageStore *,const CeDocumentSlice *,bool embedding,CeDocumentSlice **out,char *,size_t);
/* Exact node-type scan (never text matching), with no filesystem access. */
bool ce_image_slice_has_images(const CeDocumentSlice *,bool *out,char *,size_t);
#endif
