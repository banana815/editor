#include "touch_gesture.h"
#include <math.h>
#include <string.h>

void ce_touch_init(CeTouchGesture *g) { if(g) memset(g,0,sizeof *g); }
static CeTouchResult result(const CeTouchGesture *g) {
    CeTouchResult r={0};
    if(g) { r.x=g->last_x; r.y=g->last_y; r.anchor_x=g->down_x; r.anchor_y=g->down_y; }
    return r;
}
static CeTouchResult block(CeTouchGesture *g) {
    CeTouchResult r=result(g); r.handled=true;
    if(g->phase!=CE_TOUCH_BLOCKED) r.actions=CE_TOUCH_CANCEL;
    g->phase=CE_TOUCH_BLOCKED; return r;
}
CeTouchResult ce_touch_cancel(CeTouchGesture *g) {
    CeTouchResult r=result(g);
    if(g) {
        r.handled=g->contact_count!=0 || g->phase!=CE_TOUCH_IDLE || g->overflow;
        if(r.handled) r.actions=CE_TOUCH_CANCEL;
        ce_touch_init(g);
    }
    return r;
}
static bool clock_step(CeTouchGesture *g,uint64_t now) {
    if(g->clock_started && now<g->last_ms) return false;
    g->clock_started=true; g->last_ms=now; return true;
}
static size_t contact(const CeTouchGesture *g,uint64_t device,uint64_t finger) {
    for(size_t i=0;i<g->contact_count;i++)
        if(g->contacts[i].device==device && g->contacts[i].finger==finger) return i;
    return SIZE_MAX;
}
static unsigned select_if_due(CeTouchGesture *g,uint64_t now) {
    if(g->phase==CE_TOUCH_PENDING && now>=g->down_ms && now-g->down_ms>=CE_TOUCH_LONG_PRESS_MS) {
        g->phase=CE_TOUCH_SELECTING; return CE_TOUCH_SELECT_BEGIN;
    }
    return CE_TOUCH_NONE;
}
CeTouchResult ce_touch_tick(CeTouchGesture *g,uint64_t now) {
    CeTouchResult r=result(g);
    if(!g) return r;
    if(!clock_step(g,now)) return block(g);
    r.handled=g->phase!=CE_TOUCH_IDLE;
    r.actions=select_if_due(g,now); return r;
}
CeTouchResult ce_touch_event(CeTouchGesture *g,const CeTouchEvent *e) {
    CeTouchResult r=result(g);
    if(!g || !e) return r;
    size_t at=contact(g,e->device,e->finger);
    if(e->kind<CE_TOUCH_DOWN || e->kind>CE_TOUCH_CANCELED) return block(g);
    /* Terminal events still drain contacts even after an invalid coordinate or
     * clock sample. A canceled gesture must not strand otherwise valid input. */
    bool terminal=e->kind==CE_TOUCH_UP || e->kind==CE_TOUCH_CANCELED;
    bool valid=isfinite(e->x) && isfinite(e->y) && clock_step(g,e->time_ms);
    if(e->kind==CE_TOUCH_DOWN) {
        if(at!=SIZE_MAX) { r.handled=true; return r; }
        if(g->contact_count==CE_TOUCH_MAX_CONTACTS) { g->overflow=true; return block(g); }
        g->contacts[g->contact_count++]=(CeTouchContact){e->device,e->finger};
        if(!valid || g->overflow || g->contact_count>1 || g->phase==CE_TOUCH_BLOCKED) return block(g);
        g->device=e->device; g->finger=e->finger;
        g->down_x=g->last_x=e->x; g->down_y=g->last_y=e->y; g->down_ms=e->time_ms;
        g->phase=e->target?CE_TOUCH_PENDING:CE_TOUCH_IDLE;
        r=result(g); r.handled=e->target; return r;
    }
    if(at==SIZE_MAX) { r.handled=true; return r; }
    if(!valid || e->kind==CE_TOUCH_CANCELED) r=block(g);
    else if(g->phase==CE_TOUCH_BLOCKED || g->overflow) r.handled=true;
    else if(g->phase!=CE_TOUCH_IDLE) {
        r.handled=true; r.x=e->x; r.y=e->y;
        float dx=e->x-g->down_x,dy=e->y-g->down_y;
        if(!isfinite(dx) || !isfinite(dy) || !isfinite(g->last_y-e->y)) r=block(g);
        else if(g->phase==CE_TOUCH_PENDING && (fabsf(dx)>=CE_TOUCH_SLOP || fabsf(dy)>=CE_TOUCH_SLOP)) {
            g->phase=CE_TOUCH_SCROLLING;
            r.actions=CE_TOUCH_SCROLL; r.scroll_dy=g->down_y-e->y;
        } else if(g->phase==CE_TOUCH_SCROLLING) {
            r.actions=CE_TOUCH_SCROLL; r.scroll_dy=g->last_y-e->y;
        } else {
            r.actions=select_if_due(g,e->time_ms);
            if(g->phase==CE_TOUCH_SELECTING)
                r.actions|=terminal?CE_TOUCH_SELECT_END:CE_TOUCH_SELECT_UPDATE;
            else if(terminal) r.actions=CE_TOUCH_TAP;
        }
        g->last_x=e->x; g->last_y=e->y;
    }
    if(terminal) {
        g->contacts[at]=g->contacts[--g->contact_count];
        if(!g->contact_count && !g->overflow) g->phase=CE_TOUCH_IDLE;
    }
    return r;
}
