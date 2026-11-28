#ifndef CE_APP_IMAGES_STATE_H
#define CE_APP_IMAGES_STATE_H
#include "image_store.h"
#include "platform_import.h"
typedef struct {
    CeImageStore store;
    PlatformImport *job;
    char *pending_note, *pending_root;
    char pending_hash[65], property_hash[65];
    char *property_note, *property_root;
    uint64_t pending_revision;
    size_t pending_caret;
    size_t property_block;
    uint64_t property_revision;
    bool pending, ready;
    struct { char uri[83]; SDL_Surface *surface; uint64_t used; } cache[16];
    size_t cache_bytes;
    uint64_t cache_tick;
} CeAppImages;
#endif
