#ifndef CE_PLATFORM_IMAGE_CLIPBOARD_H
#define CE_PLATFORM_IMAGE_CLIPBOARD_H
#include <stddef.h>
/* Explicit paste action only; no background clipboard reads. Returns 1 with
 * SDL_malloc-owned bytes, 0 for no image, -1 for error (SDL_GetError). */
int platform_image_clipboard(void **bytes,size_t *size);
#endif
