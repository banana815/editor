#include "touch_gesture.h"
#include <assert.h>
#include <math.h>
#include <float.h>
#include <stdio.h>
static CeTouchResult event(CeTouchGesture *g,CeTouchEventKind kind,uint64_t id,uint64_t time,float x,float y,bool target) {
    CeTouchEvent e={kind,1,id,time,x,y,target}; return ce_touch_event(g,&e);
}
int main(void) {
    CeTouchGesture g={0}; CeTouchResult r;
    r=event(&g,CE_TOUCH_DOWN,1,10,20,100,true); assert(r.handled && !r.actions);
    r=event(&g,CE_TOUCH_UP,1,100,21,102,false); assert(r.actions==CE_TOUCH_TAP && g.phase==CE_TOUCH_IDLE);
    assert(event(&g,CE_TOUCH_UP,1,101,21,102,false).actions==CE_TOUCH_NONE);
    r=event(&g,CE_TOUCH_DOWN,1,200,20,100,true); assert(!r.actions);
    r=event(&g,CE_TOUCH_MOVE,1,210,20,96,false); assert(!r.actions);
    r=event(&g,CE_TOUCH_MOVE,1,220,20,80,false); assert(r.actions==CE_TOUCH_SCROLL && r.scroll_dy==20);
    r=event(&g,CE_TOUCH_MOVE,1,900,20,60,false); assert(r.actions==CE_TOUCH_SCROLL && r.scroll_dy==20);
    r=event(&g,CE_TOUCH_UP,1,910,20,55,false); assert(r.actions==CE_TOUCH_SCROLL && r.scroll_dy==5);
    r=event(&g,CE_TOUCH_DOWN,1,1000,20,100,true); assert(!r.actions);
    assert(!ce_touch_tick(&g,1499).actions);
    r=ce_touch_tick(&g,1500); assert(r.actions==CE_TOUCH_SELECT_BEGIN && r.anchor_y==100);
    assert(!ce_touch_tick(&g,1600).actions);
    r=event(&g,CE_TOUCH_MOVE,1,1700,50,140,false); assert(r.actions==CE_TOUCH_SELECT_UPDATE);
    r=event(&g,CE_TOUCH_UP,1,1800,50,140,false); assert(r.actions==CE_TOUCH_SELECT_END);
    event(&g,CE_TOUCH_DOWN,1,2000,20,100,true);
    r=event(&g,CE_TOUCH_UP,1,2500,20,100,false); assert(r.actions==(CE_TOUCH_SELECT_BEGIN|CE_TOUCH_SELECT_END));
    event(&g,CE_TOUCH_DOWN,1,3000,20,100,true);
    r=event(&g,CE_TOUCH_MOVE,1,3600,40,100,false); assert(r.actions==CE_TOUCH_SCROLL && !r.scroll_dy);
    r=event(&g,CE_TOUCH_CANCELED,1,3601,NAN,NAN,false); assert(r.actions==CE_TOUCH_CANCEL && g.phase==CE_TOUCH_IDLE);
    event(&g,CE_TOUCH_DOWN,1,4000,20,100,true);
    r=event(&g,CE_TOUCH_DOWN,2,4001,30,100,false); assert(r.actions==CE_TOUCH_CANCEL && g.phase==CE_TOUCH_BLOCKED);
    assert(!event(&g,CE_TOUCH_UP,1,4002,20,100,false).actions && g.phase==CE_TOUCH_BLOCKED);
    assert(!event(&g,CE_TOUCH_UP,2,4003,30,100,false).actions && g.phase==CE_TOUCH_IDLE);
    r=event(&g,CE_TOUCH_DOWN,1,5000,20,100,false); assert(!r.handled);
    r=event(&g,CE_TOUCH_MOVE,1,5100,20,50,true); assert(!r.handled);
    r=event(&g,CE_TOUCH_UP,1,5200,20,50,true); assert(!r.handled);
    event(&g,CE_TOUCH_DOWN,1,6000,20,100,true);
    r=event(&g,CE_TOUCH_DOWN,1,6001,20,100,true); assert(r.handled && !r.actions && g.contact_count==1);
    r=ce_touch_tick(&g,5999); assert(r.actions==CE_TOUCH_CANCEL);
    event(&g,CE_TOUCH_UP,1,6002,20,100,false); assert(g.phase==CE_TOUCH_IDLE);
    event(&g,CE_TOUCH_DOWN,1,7000,20,100,true);
    assert(ce_touch_cancel(&g).actions==CE_TOUCH_CANCEL && g.phase==CE_TOUCH_IDLE);
    assert(!event(&g,CE_TOUCH_UP,1,7001,20,100,false).actions);
    for(uint64_t i=0;i<CE_TOUCH_MAX_CONTACTS+1;i++) event(&g,CE_TOUCH_DOWN,i,8000+i,20,100,true);
    assert(g.overflow && g.phase==CE_TOUCH_BLOCKED);
    ce_touch_cancel(&g); assert(!g.overflow && !g.contact_count);
    event(&g,CE_TOUCH_DOWN,1,9000,FLT_MAX,100,true);
    r=event(&g,CE_TOUCH_MOVE,1,9001,-FLT_MAX,100,false); assert(r.actions==CE_TOUCH_CANCEL);
    ce_touch_cancel(&g);
    assert(!ce_touch_event(NULL,NULL).actions && !ce_touch_cancel(NULL).actions);
    puts("touch gesture: tap, scroll, long press, selection, cancellation, multiple contacts and limits passed");
    return 0;
}
