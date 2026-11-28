#ifndef CE_IMAGE_PUBLISH_H
#define CE_IMAGE_PUBLISH_H
#include "image_store.h"
#include "document_publish.h"
char *ce_image_publish_url(void *store,const struct cJSON *node,char *error,size_t capacity);
bool ce_image_publish_bundle(const CeDocument *,CePublishFormat,CePublishAtomHtml,void *,CeImageStore *,unsigned char **,size_t *,char *,size_t);
#endif
