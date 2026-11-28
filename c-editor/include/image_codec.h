#ifndef CE_IMAGE_CODEC_H
#define CE_IMAGE_CODEC_H
#include <stdbool.h>
#include <stddef.h>
#define CE_IMAGE_MAX_BYTES ((size_t)16*1024*1024)
#define CE_IMAGE_MAX_PIXELS ((size_t)16*1024*1024)
typedef struct { unsigned width,height; const char *extension; } CeImageInfo;
/* PNG/JPEG/BMP only. Main-thread serialized, bounded decode allocations.
 * pixels is malloc-owned RGBA8; NULL skips returned pixels but still validates
 * the entire compressed stream. Output remains untouched on failure. */
bool ce_image_decode(const void *bytes,size_t size,CeImageInfo *info,
                     unsigned char **pixels,char *error,size_t capacity);
#endif
