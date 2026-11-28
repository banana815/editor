#ifndef APP_PREVIEW_STATE_H
#define APP_PREVIEW_STATE_H
#include "rich_layout.h"
#include "citation_session.h"
typedef struct {
    bool open,mobile,valid,pointer,moved;
    CeRichLayout *layout;
    CeDocument *document;
    CeCitationPrepared *snapshot;
    SDL_Texture *texture;
    SDL_FRect area,viewport,buttons[4];
    float width,scroll,scale,last_y,down_x,down_y,font_size;
    int texture_width,texture_height;
    uint64_t revision,citation_revision,images_revision;
    char *notes_root;
    const CeDocument *model;
    size_t images,links,code;
    char error[512];
} BlogPreview;
#endif
