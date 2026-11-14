#include "platform_web_input.h"
#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#include <stdlib.h>
#include <string.h>

typedef struct WebInput { int kind; char *text; struct WebInput *next; } WebInput;
static WebInput *head, *tail;
static SDL_WindowID window_id;
static Uint32 input_event;
static PlatformWebClipboardRead clipboard_read;
static PlatformWebClipboardCut clipboard_cut;
static PlatformWebClipboardError clipboard_error;
static SDL_AppResult (*clipboard_handler)(void *, SDL_Event *);
static void *clipboard_state;

static void flush_input_before_clipboard(void) {
    /* Clipboard events require a synchronous answer. Process earlier clicks,
     * selection keys and text in SDL order before reading the selection. */
    SDL_Event event;
    if (!clipboard_handler) return;
    while (SDL_PeepEvents(&event, 1, SDL_GETEVENT, SDL_EVENT_FIRST, SDL_EVENT_LAST) > 0) {
        switch (event.type) {
        /* SDL's main-callback watcher already delivers these synchronously. */
        case SDL_EVENT_TERMINATING: case SDL_EVENT_LOW_MEMORY:
        case SDL_EVENT_WILL_ENTER_BACKGROUND: case SDL_EVENT_DID_ENTER_BACKGROUND:
        case SDL_EVENT_WILL_ENTER_FOREGROUND: case SDL_EVENT_DID_ENTER_FOREGROUND:
            break;
        default: clipboard_handler(clipboard_state, &event); break;
        }
    }
}
EMSCRIPTEN_KEEPALIVE char *platform_web_clipboard_read(void) {
    flush_input_before_clipboard();
    return clipboard_read ? clipboard_read(clipboard_state) : NULL;
}
EMSCRIPTEN_KEEPALIVE int platform_web_clipboard_cut(const char *expected) {
    return clipboard_cut && clipboard_cut(expected, clipboard_state);
}
EMSCRIPTEN_KEEPALIVE void platform_web_clipboard_free(void *text) { free(text); }
EMSCRIPTEN_KEEPALIVE void platform_web_clipboard_failed(void) {
    if (clipboard_error) clipboard_error(clipboard_state);
}

EM_JS(void, pending_js, (int pending), { Module.editorInputPending = !!pending; })
static void update_pending(void) {
    bool pending = false;
    for (WebInput *input = head; input; input = input->next)
        if (input->kind >= 0 && input->kind != 1) pending = true;
    pending_js(pending);
}
EMSCRIPTEN_KEEPALIVE void *platform_web_input_alloc(size_t size) { return malloc(size); }
EMSCRIPTEN_KEEPALIVE void platform_web_input_push(int kind, char *text) {
    WebInput *input = malloc(sizeof(*input));
    if (!input) { free(text); return; }
    *input = (WebInput){kind, text, NULL};
    SDL_Event event = {0};
    event.type = input_event; event.user.data1 = input;
    if (!input_event || !SDL_PushEvent(&event)) { free(text); free(input); return; }
    if (tail) tail->next = input; else head = input;
    tail = input;
    update_pending();
}
EM_JS(void, init_js, (void), {
    var input = document.getElementById('editor-input');
    if (!input) return;
    var composing = false, target = 0, suppress = null, enabled = false;
    var canvas = Module.canvas;
    function send(kind, text) {
        if (!enabled) return;
        var bytes = new TextEncoder().encode(text || "");
        var ptr = _platform_web_input_alloc(bytes.length + 1);
        if (!ptr) return;
        HEAPU8.set(bytes, ptr); HEAPU8[ptr + bytes.length] = 0;
        _platform_web_input_push(kind, ptr);
    }
    function clear() { composing = false; suppress = null; input.value = ""; }
    function ownsFocus() {
        return enabled && (document.activeElement === input || document.activeElement === canvas);
    }
    // Toolbar clicks can leave DOM focus on the canvas while SDL retains the
    // selection. Route clipboard shortcuts before SDL's global key listener.
    window.addEventListener('keydown', function(e) {
        if (!ownsFocus() || composing || e.isComposing) return;
        if ((e.ctrlKey || e.metaKey) && !e.altKey && ['c','x','v'].includes(e.key.toLowerCase())) {
            input.focus({preventScroll:true}); e.stopPropagation();
            if (e.key.toLowerCase() !== 'v') {
                var ptr = _platform_web_clipboard_read();
                if (!ptr) { e.preventDefault(); return; }
                try { input.value = UTF8ToString(ptr); input.select(); }
                finally { _platform_web_clipboard_free(ptr); }
            }
        }
    }, true);
    input.addEventListener('keypress', function(e) { e.stopPropagation(); });
    input.addEventListener('keydown', function(e) {
        suppress = null;
        // Let the browser dispatch copy/cut/paste. SDL's internal clipboard
        // cannot stand in for the system clipboard in Emscripten.
        if ((e.ctrlKey || e.metaKey) && !e.altKey && ['c','x','v'].includes(e.key.toLowerCase())) {
            e.stopPropagation(); return;
        }
        if (e.metaKey && ['s','n','z','y','b','i','u','f','a'].includes(e.key.toLowerCase())) e.preventDefault();
        if (composing || e.isComposing || e.keyCode === 229) e.stopPropagation();
        else if (e.key === 'Enter' || e.key === 'Delete') e.preventDefault();
    });
    function copyOrCut(e, cut) {
        if (!ownsFocus() || composing) return;
        e.preventDefault(); e.stopPropagation();
        var ptr = _platform_web_clipboard_read();
        if (!ptr) return;
        try {
            if (!e.clipboardData) throw new Error("Clipboard unavailable");
            var text = UTF8ToString(ptr);
            e.clipboardData.setData('text/plain', text);
            // A denied or read-only DataTransfer may ignore setData without
            // throwing. Never delete the selection unless its text was accepted.
            if (e.clipboardData.getData('text/plain') !== text) throw new Error("Clipboard write rejected");
            if (cut) _platform_web_clipboard_cut(ptr);
        } catch (error) { _platform_web_clipboard_failed(); }
        finally { _platform_web_clipboard_free(ptr); input.value = ""; }
    }
    document.addEventListener('copy', function(e) { copyOrCut(e, false); }, true);
    document.addEventListener('cut', function(e) { copyOrCut(e, true); }, true);
    document.addEventListener('paste', function(e) {
        if (!ownsFocus() || composing) return;
        e.preventDefault(); e.stopPropagation();
        try {
            if (!e.clipboardData) throw new Error("Clipboard unavailable");
            send(0, e.clipboardData.getData('text/plain'));
        } catch (error) { _platform_web_clipboard_failed(); }
        finally { input.value = ""; }
    }, true);
    input.addEventListener('compositionstart', function() { composing = true; suppress = null; });
    input.addEventListener('compositionupdate', function(e) { send(1, e.data); });
    input.addEventListener('compositionend', function(e) {
        composing = false;
        send(e.data ? 5 : 1, e.data);
        input.value = "";
        suppress = e.data;
        setTimeout(function() { suppress = null; }, 0);
    });
    input.addEventListener('beforeinput', function(e) {
        if (composing || e.isComposing) return;
        // Physical keys are prevented by SDL; these handle software keyboards.
        var kind = e.inputType === 'deleteContentBackward' ? 2 :
                   e.inputType === 'deleteContentForward' ? 3 :
                   e.inputType === 'insertLineBreak' || e.inputType === 'insertParagraph' ? 4 : -1;
        if (kind >= 0) { e.preventDefault(); send(kind, ""); input.value = ""; }
    });
    input.addEventListener('input', function(e) {
        if (composing || e.isComposing) return;
        if (suppress !== null && (e.inputType === 'insertFromComposition' || e.data === suppress)) {
            suppress = null; input.value = ""; return;
        }
        // Chromium can emit several input events for one multiline edit: the
        // first already exposes the complete value, then trailing events carry
        // only data fragments. After clearing, those fragments are not new text.
        var text = input.value;
        if (text) send(0, text);
        input.value = "";
    });
    input.addEventListener('blur', function() { if (composing) send(1, ""); clear(); });
    // Focus inside a trusted gesture so mobile browsers can show the keyboard.
    canvas.addEventListener('pointerup', function(e) {
        if (!enabled) return;
        var rect = canvas.getBoundingClientRect(), area = Module.editorInputArea;
        var x = (e.clientX - rect.left) * canvas.width / rect.width;
        var y = (e.clientY - rect.top) * canvas.height / rect.height;
        if (area && x >= area.x && x < area.x + area.w && y >= area.y && y < area.y + area.h) {
            input.focus({preventScroll:true});
        }
    });
    Module.editorInputCancel = function() {
        var wasFocused = document.activeElement === input;
        // Blurring terminates native composition; discard any resulting commit.
        var wasEnabled = enabled; enabled = false;
        if (wasFocused) input.blur();
        clear(); enabled = wasEnabled;
        if (wasFocused && enabled) input.focus({preventScroll:true});
    };
    Module.editorInputUpdate = function(next, x, y, w, h) {
        enabled = !!next;
        Module.editorInputArea = {x:x,y:y,w:w,h:h};
        var rect = canvas.getBoundingClientRect();
        input.style.left = (rect.left + x * rect.width / canvas.width) + 'px';
        input.style.top = (rect.top + y * rect.height / canvas.height) + 'px';
        if (next !== target) {
            if (document.activeElement === input) input.blur();
            clear(); target = next;
            if (enabled) input.focus({preventScroll:true});
        }
    };
})
EM_JS(void, update_js, (int target, float x, float y, float w, float h), {
    if (Module.editorInputUpdate) Module.editorInputUpdate(target, x, y, w, h);
})
EM_JS(void, cancel_js, (void), { if (Module.editorInputCancel) Module.editorInputCancel(); })
void platform_web_input_init(void) { input_event = SDL_RegisterEvents(1); init_js(); }
void platform_web_input_clipboard(PlatformWebClipboardRead read, PlatformWebClipboardCut cut,
    PlatformWebClipboardError error, SDL_AppResult (*handler)(void *, SDL_Event *), void *state) {
    clipboard_read = read; clipboard_cut = cut; clipboard_error = error;
    clipboard_handler = handler; clipboard_state = state;
}
void platform_web_input_update(int target, SDL_Window *window, const SDL_FRect *area) {
    window_id = SDL_GetWindowID(window);
    update_js(target, area->x, area->y, area->w, area->h);
}
void platform_web_input_cancel(void) {
    cancel_js();
    /* SDL still owns the queued event; keep its allocation until delivery. */
    // A view change may cancel an old IME composition, but ordinary committed
    // text after a navigation/confirmation key still belongs to the next view.
    // Keep those events in SDL order rather than dropping fast typing.
    for (WebInput *input = head; input; input = input->next)
        if (input->kind == 1 || input->kind == 5) input->kind = -1;
    update_pending();
}
bool platform_web_input_handle_event(SDL_Event *event,
        SDL_AppResult (*handler)(void *, SDL_Event *), void *state) {
    if (!input_event || event->type != input_event) return false;
    WebInput *input = event->user.data1;
    WebInput **link = &head;
    while (*link && *link != input) link = &(*link)->next;
    if (!*link) return true;
    *link = input->next;
    if (tail == input) { tail = head; while (tail && tail->next) tail = tail->next; }
    if (input->kind >= 0) {
        SDL_Event translated = {0};
        if (input->kind == 0 || input->kind == 5) {
            translated.type = SDL_EVENT_TEXT_INPUT; translated.text.windowID = window_id; translated.text.text = input->text;
        } else if (input->kind == 1) {
            translated.type = SDL_EVENT_TEXT_EDITING; translated.edit.windowID = window_id; translated.edit.text = input->text;
            translated.edit.start = (int)SDL_utf8strlen(input->text); translated.edit.length = 0;
        } else {
            translated.type = SDL_EVENT_KEY_DOWN; translated.key.windowID = window_id; translated.key.down = true;
            translated.key.key = input->kind == 2 ? SDLK_BACKSPACE : input->kind == 3 ? SDLK_DELETE : SDLK_RETURN;
        }
        handler(state, &translated);
    }
    free(input->text); free(input);
    update_pending();
    return true;
}
void platform_web_input_destroy(void) {
    platform_web_input_cancel();
    if (input_event) SDL_FlushEvent(input_event);
    while (head) { WebInput *old = head; head = head->next; free(old->text); free(old); }
    tail = NULL; input_event = 0;
    clipboard_read = NULL; clipboard_cut = NULL; clipboard_error = NULL;
    clipboard_handler = NULL; clipboard_state = NULL;
    update_pending();
}
#else
void platform_web_input_init(void) {}
void platform_web_input_clipboard(PlatformWebClipboardRead read, PlatformWebClipboardCut cut,
    PlatformWebClipboardError error, SDL_AppResult (*handler)(void *, SDL_Event *), void *state) {
    (void)read; (void)cut; (void)error; (void)handler; (void)state;
}
void platform_web_input_update(int target, SDL_Window *window, const SDL_FRect *area) { (void)target; (void)window; (void)area; }
void platform_web_input_cancel(void) {}
bool platform_web_input_handle_event(SDL_Event *event, SDL_AppResult (*handler)(void *, SDL_Event *), void *state) { (void)event; (void)handler; (void)state; return false; }
void platform_web_input_destroy(void) {}
#endif
