#ifndef NOTE_LAB_PLATFORM_WEB_INPUT_H
#define NOTE_LAB_PLATFORM_WEB_INPUT_H
#include <SDL3/SDL.h>
/* A native textarea supplies browser IME and mobile keyboard events. Text is
 * queued in the SDL event stream so text, navigation and confirmation keys
 * preserve their browser event order. */
void platform_web_input_init(void);
/* Selection text uses malloc/free. Cut is called synchronously only after the
 * browser clipboardData has accepted the exact UTF-8 text. */
typedef char *(*PlatformWebClipboardRead)(void *state);
typedef bool (*PlatformWebClipboardCut)(const char *expected, void *state);
typedef void (*PlatformWebClipboardError)(void *state);
void platform_web_input_clipboard(PlatformWebClipboardRead read,
    PlatformWebClipboardCut cut, PlatformWebClipboardError error,
    SDL_AppResult (*handler)(void *, SDL_Event *), void *state);
void platform_web_input_update(int target, SDL_Window *window, const SDL_FRect *area);
void platform_web_input_cancel(void);
bool platform_web_input_handle_event(SDL_Event *event, SDL_AppResult (*handler)(void *, SDL_Event *), void *state);
void platform_web_input_destroy(void);
#endif
