#ifndef CE_TOUCH_GESTURE_H
#define CE_TOUCH_GESTURE_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define CE_TOUCH_MAX_CONTACTS 32
#define CE_TOUCH_SLOP 8.0f
#define CE_TOUCH_LONG_PRESS_MS UINT64_C(500)

typedef enum CeTouchEventKind { CE_TOUCH_DOWN, CE_TOUCH_MOVE, CE_TOUCH_UP, CE_TOUCH_CANCELED } CeTouchEventKind;
typedef enum CeTouchPhase { CE_TOUCH_IDLE, CE_TOUCH_PENDING, CE_TOUCH_SCROLLING, CE_TOUCH_SELECTING, CE_TOUCH_BLOCKED } CeTouchPhase;
typedef enum CeTouchAction {
    CE_TOUCH_NONE=0, CE_TOUCH_TAP=1u, CE_TOUCH_SCROLL=2u,
    CE_TOUCH_SELECT_BEGIN=4u, CE_TOUCH_SELECT_UPDATE=8u,
    CE_TOUCH_SELECT_END=16u, CE_TOUCH_CANCEL=32u
} CeTouchAction;
typedef struct CeTouchEvent {
    CeTouchEventKind kind;
    uint64_t device, finger, time_ms;
    float x, y; /* Logical drawing coordinates, already converted exactly once. */
    bool target; /* On DOWN only: this contact starts in a gesture-enabled editor. */
} CeTouchEvent;
typedef struct CeTouchResult {
    unsigned actions;
    bool handled; /* False: route this single contact to the existing non-editor UI. */
    float x, y, anchor_x, anchor_y;
    float scroll_dy; /* Add to the scroll offset; positive moves the document up. */
} CeTouchResult;
typedef struct CeTouchContact { uint64_t device, finger; } CeTouchContact;
typedef struct CeTouchGesture {
    CeTouchPhase phase;
    CeTouchContact contacts[CE_TOUCH_MAX_CONTACTS];
    size_t contact_count;
    uint64_t device, finger, down_ms, last_ms;
    float down_x, down_y, last_x, last_y;
    bool clock_started, overflow;
} CeTouchGesture;

/* Zero initialization is valid. No allocation, SDL, text mutation or timers.
 * Feed every real contact (including those outside the editor); the first
 * contact owns its route until release. A second contact cancels the gesture
 * and blocks all contacts until all are lifted. At >32 contacts, remain blocked
 * until explicit cancel rather than risk treating an untracked finger as a tap.
 * Time is caller-supplied monotonic milliseconds. Nonfinite coordinates and
 * backwards time cancel the gesture. Pending never requests focus/selection.
 * Actions may combine BEGIN|UPDATE or BEGIN|END; apply in that order. Selection
 * and scrolling never request keyboard focus; only TAP may do so. */
void ce_touch_init(CeTouchGesture *gesture);
CeTouchResult ce_touch_event(CeTouchGesture *gesture, const CeTouchEvent *event);
CeTouchResult ce_touch_tick(CeTouchGesture *gesture, uint64_t time_ms);
CeTouchResult ce_touch_cancel(CeTouchGesture *gesture);
#endif
