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
typedef struct PlatformWebClipboardData {
    const char *plain, *html, *typed_json;
} PlatformWebClipboardData;
/* DOM clipboard events are synchronous. A successful prepare lends all strings
 * until exactly one finish; failed writes finish(false), never a cut. Paste
 * strings are borrowed only during its callback. The controller owns guards
 * and all document edits. A plain-only result supports text prompts. */
typedef bool (*PlatformWebClipboardPrepare)(bool cut, PlatformWebClipboardData *out, void *state);
typedef void (*PlatformWebClipboardFinish)(bool accepted, void *state);
typedef bool (*PlatformWebClipboardPaste)(const PlatformWebClipboardData *data, void *state);
void platform_web_input_clipboard_rich(PlatformWebClipboardPrepare prepare,
    PlatformWebClipboardFinish finish, PlatformWebClipboardPaste paste, void *state);
void platform_web_input_update(int target, SDL_Window *window, const SDL_FRect *area);
void platform_web_input_cancel(void);
/* A C-confirmed tap may focus the DOM input at the end of the same trusted
 * pointerup event. Drag/long-press routes never request this token. */
void platform_web_input_request_focus(void);
typedef void (*PlatformWebClipboardCompletion)(bool success, const char *plain,
    const char *html, const char *typed_json, const char *error, void *userdata);
/* Call in a user gesture. False means not started and no callback; true means
 * exactly one completion, including cancellation during destroy. Read strings
 * are borrowed only during the callback. Write copies the supplied strings
 * before returning. This transport never edits the document. Native platforms
 * return false (the controller uses SDL there). No global clipboard read is
 * performed without an explicit request. Payloads are bounded to 16 MiB each. */
bool platform_web_clipboard_write(const char *plain, const char *html,
    const char *typed_json, PlatformWebClipboardCompletion completion, void *userdata);
bool platform_web_clipboard_read(PlatformWebClipboardCompletion completion, void *userdata);
bool platform_web_input_handle_event(SDL_Event *event, SDL_AppResult (*handler)(void *, SDL_Event *), void *state);
void platform_web_input_destroy(void);
#endif
