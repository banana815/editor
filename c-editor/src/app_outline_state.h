#ifndef APP_OUTLINE_STATE_H
#define APP_OUTLINE_STATE_H
#include "document_outline.h"
typedef struct {
    CeOutline index;
    unsigned char *folded;
    size_t *visible, count;
    const CeDocument *model;
    char *note;
    uint64_t revision, drag_revision;
    bool valid, pointer, moved, handle, after;
    size_t pressed, target;
    float down_x, down_y, last_y;
} OutlineView;
#endif
