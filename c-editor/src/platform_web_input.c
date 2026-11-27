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
static PlatformWebClipboardPrepare rich_prepare;
static PlatformWebClipboardFinish rich_finish;
static PlatformWebClipboardPaste rich_paste;
static void *rich_state;
static PlatformWebClipboardData rich_data;
static bool rich_prepared;
static bool focus_requested;
typedef struct ClipboardRequest {
    unsigned id;
    PlatformWebClipboardCompletion completion;
    void *userdata;
    struct ClipboardRequest *next;
} ClipboardRequest;
static ClipboardRequest *clipboard_requests;
static unsigned clipboard_next_id=1;
static bool clipboard_closing;

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
EMSCRIPTEN_KEEPALIVE char *platform_web_clipboard_selection_text(void) {
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
void platform_web_input_request_focus(void) { focus_requested=true; }
EMSCRIPTEN_KEEPALIVE void platform_web_input_pointer_begin(void) { focus_requested=false; }
EMSCRIPTEN_KEEPALIVE int platform_web_input_pointer_finish(void) {
    flush_input_before_clipboard();
    bool result=focus_requested; focus_requested=false; return result;
}
EMSCRIPTEN_KEEPALIVE void platform_web_clipboard_complete(unsigned id,int success,
    char *plain,char *html,char *typed,char *error) {
    ClipboardRequest **link=&clipboard_requests;
    while(*link && (*link)->id!=id) link=&(*link)->next;
    if(*link) {
        ClipboardRequest *request=*link; *link=request->next;
        request->completion(success!=0,plain,html,typed,error,request->userdata);
        free(request);
    }
    free(plain); free(html); free(typed); free(error);
}

EMSCRIPTEN_KEEPALIVE int platform_web_clipboard_prepare(int cut) {
    flush_input_before_clipboard();
    if (!rich_prepare) return -1;
    if (rich_prepared) return 0;
    rich_data=(PlatformWebClipboardData){0};
    rich_prepared=rich_prepare(cut!=0,&rich_data,rich_state);
    return rich_prepared ? 1 : 0;
}
EMSCRIPTEN_KEEPALIVE const char *platform_web_clipboard_format(int format) {
    if (!rich_prepared) return NULL;
    return format==0 ? rich_data.plain : format==1 ? rich_data.html : format==2 ? rich_data.typed_json : NULL;
}
EMSCRIPTEN_KEEPALIVE void platform_web_clipboard_finish(int accepted) {
    if (!rich_prepared) return;
    rich_prepared=false; rich_data=(PlatformWebClipboardData){0};
    if (rich_finish) rich_finish(accepted!=0,rich_state);
}
EMSCRIPTEN_KEEPALIVE int platform_web_clipboard_paste_data(char *plain,char *html,char *typed) {
    flush_input_before_clipboard();
    PlatformWebClipboardData data={plain,html,typed};
    int result=rich_paste ? (rich_paste(&data,rich_state) ? 1 : 0) : -1;
    free(plain); free(html); free(typed); return result;
}
EMSCRIPTEN_KEEPALIVE int platform_web_clipboard_rich_paste_available(void) { return rich_paste!=NULL; }

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
    var clipboardJobs = new Map();
    var clipboardLimit = 16 * 1024 * 1024;
    var customClipboardType = 'web application/x-notelab-document-slice+json';
    Module.editorClipboardCancel = function() { clipboardJobs.clear(); };
    function clipboardFinish(id, success, plain, html, typed, error) {
        if (!clipboardJobs.has(id)) return;
        clipboardJobs.delete(id);
        var pointers=[];
        try {
            for (var text of [plain,html,typed,error]) {
                if (text == null) { pointers.push(0); continue; }
                if (text.length>clipboardLimit) throw new Error('Clipboard payload exceeds 16 MiB');
                var bytes=new TextEncoder().encode(text);
                if (bytes.length>clipboardLimit || bytes.includes(0)) throw new Error('Clipboard payload is too large or contains NUL');
                var ptr=_platform_web_input_alloc(bytes.length+1);
                if (!ptr) throw new Error('Out of memory reading clipboard');
                HEAPU8.set(bytes,ptr); HEAPU8[ptr+bytes.length]=0; pointers.push(ptr);
            }
        } catch (e) {
            for (var pointer of pointers) if(pointer) _platform_web_clipboard_free(pointer);
            pointers=[0,0,0,0]; success=false;
        }
        _platform_web_clipboard_complete(id,success?1:0,pointers[0],pointers[1],pointers[2],pointers[3]);
    }
    Module.editorClipboardStart = function(id, writing, plain, html, typed) {
        var api=navigator.clipboard;
        if (!api || (writing ? !api.write && !api.writeText : !api.read && !api.readText)) return 0;
        // Rich copying must retain its HTML fallback; plain-only prompts can
        // still use older writeText implementations.
        if (writing && (html != null || typed != null) &&
            (!api.write || typeof ClipboardItem === 'undefined')) return 0;
        clipboardJobs.set(id,true);
        try {
            var operation;
            if (writing) {
                if (api.write && typeof ClipboardItem !== 'undefined') {
                    var data={'text/plain':new Blob([plain||""],{type:'text/plain'})};
                    if (html != null) data['text/html']=new Blob([html],{type:'text/html'});
                    var customSupported=false;
                    try { customSupported=typed != null && typeof ClipboardItem.supports==='function' && ClipboardItem.supports(customClipboardType)===true; }
                    catch (e) { /* Optional custom formats cannot prevent standard text copying. */ }
                    if (customSupported) data[customClipboardType]=new Blob([typed],{type:'application/x-notelab-document-slice+json'});
                    var item;
                    try { item=new ClipboardItem(data); }
                    catch (e) { delete data[customClipboardType]; item=new ClipboardItem(data); }
                    function writeStandard(error) {
                        if (!clipboardJobs.has(id) || !(customClipboardType in data) || !error || error.name!=='NotSupportedError') throw error;
                        delete data[customClipboardType];
                        return api.write([new ClipboardItem(data)]);
                    }
                    try { operation=api.write([item]); }
                    catch (error) { operation=writeStandard(error); }
                    operation=Promise.resolve(operation).catch(writeStandard);
                } else operation=api.writeText(plain||"");
                Promise.resolve(operation).then(function(){clipboardFinish(id,true,null,null,null,null);},
                    function(error){clipboardFinish(id,false,null,null,null,String(error));});
            } else if (api.read) {
                operation=api.read();
                Promise.resolve(operation).then(async function(items) {
                    var values=[null,null,null],types=['text/plain','text/html',customClipboardType];
                    if (items.length) for (var i=0;i<types.length;i++) {
                        if (!items[0].types.includes(types[i])) continue;
                        var blob=await items[0].getType(types[i]);
                        if (blob.size>clipboardLimit) throw new Error('Clipboard payload exceeds 16 MiB');
                        values[i]=new TextDecoder('utf-8',{fatal:true,ignoreBOM:true}).decode(await blob.arrayBuffer());
                    }
                    if (values.every(function(value){return value===null;})) throw new Error('Clipboard has no supported text format');
                    clipboardFinish(id,true,values[0],values[1],values[2],null);
                }).catch(function(error){clipboardFinish(id,false,null,null,null,String(error));});
            } else Promise.resolve(api.readText()).then(function(text){clipboardFinish(id,true,text,null,null,null);},
                function(error){clipboardFinish(id,false,null,null,null,String(error));});
        } catch (error) {
            /* Keep true-started requests asynchronous even for immediate API
             * exceptions so the controller can establish its guard ownership. */
            Promise.resolve().then(function(){clipboardFinish(id,false,null,null,null,String(error));});
        }
        return 1;
    };
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
        if (!ownsFocus() || composing || e.isComposing || e.keyCode === 229) return;
        // SDL owns writing commands, including when toolbar clicks leave focus
        // on the canvas. Cancel browser actions without stopping SDL delivery.
        var command = e.ctrlKey || e.metaKey;
        var heading = e.altKey && (/^[0-6]$/.test(e.key) || /^Digit[0-6]$/.test(e.code || ''));
        var workspace = !e.altKey && e.shiftKey && ['f','o','t','p','m'].includes(e.key.toLowerCase());
        var replace = !e.altKey && e.key.toLowerCase() === 'h';
        var link = !e.altKey && e.key.toLowerCase() === 'k';
        if (command && (heading || workspace || replace || link)) e.preventDefault();
        if ((e.ctrlKey || e.metaKey) && !e.altKey && ['c','x','v'].includes(e.key.toLowerCase())) {
            input.focus({preventScroll:true}); e.stopPropagation();
            if (e.key.toLowerCase() !== 'v') {
                var ptr = _platform_web_clipboard_selection_text();
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
        var prepared=_platform_web_clipboard_prepare(cut?1:0);
        if (prepared>=0) {
            if (!prepared) return;
            var accepted=false;
            try {
                if (!e.clipboardData) throw new Error('Clipboard unavailable');
                if (!_platform_web_clipboard_format(0)) throw new Error('Clipboard has no plain text representation');
                var types=['text/plain','text/html','application/x-notelab-document-slice+json'];
                for (var i=0;i<types.length;i++) {
                    var value=_platform_web_clipboard_format(i);
                    if (!value) continue;
                    var text=UTF8ToString(value);
                    if (i===2) {
                        try { e.clipboardData.setData(types[i],text); } catch (optional) {}
                    } else {
                        e.clipboardData.setData(types[i],text);
                        if (e.clipboardData.getData(types[i])!==text) throw new Error('Clipboard write rejected');
                    }
                }
                accepted=true;
            } catch (error) { _platform_web_clipboard_failed(); }
            finally { _platform_web_clipboard_finish(accepted?1:0); input.value=""; }
            return;
        }
        var ptr = _platform_web_clipboard_selection_text();
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
            if (_platform_web_clipboard_rich_paste_available()) {
                var pointers=[],types=['text/plain','text/html','application/x-notelab-document-slice+json'];
                try {
                    for (var type of types) {
                        if (!Array.from(e.clipboardData.types).includes(type)) { pointers.push(0); continue; }
                        var text=e.clipboardData.getData(type);
                        if (text.length>clipboardLimit) throw new Error('Clipboard payload exceeds 16 MiB');
                        var bytes=new TextEncoder().encode(text);
                        if (bytes.length>clipboardLimit || bytes.includes(0)) throw new Error('Invalid clipboard payload');
                        var ptr=_platform_web_input_alloc(bytes.length+1);
                        if (!ptr) throw new Error('Out of memory reading clipboard');
                        pointers.push(ptr); HEAPU8.set(bytes,ptr); HEAPU8[ptr+bytes.length]=0;
                    }
                } catch (error) {
                    for (var ptr of pointers) if(ptr) _platform_web_clipboard_free(ptr);
                    throw error;
                }
                _platform_web_clipboard_paste_data(pointers[0],pointers[1],pointers[2]);
            } else send(0, e.clipboardData.getData('text/plain'));
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
    canvas.addEventListener('pointerdown', function(e) {
        if (e.pointerType==='touch') _platform_web_input_pointer_begin();
    },true);
    // SDL registered its pointer listener before this bridge. Drain the queued
    // events now, while the browser still recognizes the trusted gesture.
    canvas.addEventListener('pointerup', function(e) {
        var requested=_platform_web_input_pointer_finish();
        if (!enabled) return;
        if (e.pointerType==='touch') {
            if (requested) input.focus({preventScroll:true});
            return;
        }
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
EM_JS(int, clipboard_start_js, (unsigned id,int writing,const char *plain,const char *html,const char *typed), {
    return Module.editorClipboardStart ? Module.editorClipboardStart(id,!!writing,
        plain?UTF8ToString(plain):null,html?UTF8ToString(html):null,typed?UTF8ToString(typed):null):0;
})
EM_JS(void, clipboard_cancel_js, (void), { if(Module.editorClipboardCancel) Module.editorClipboardCancel(); })
static bool clipboard_start(bool writing,const char *plain,const char *html,const char *typed,
    PlatformWebClipboardCompletion completion,void *userdata) {
    if(!completion || clipboard_closing || !clipboard_next_id) return false;
    const char *values[]={plain,html,typed};
    for(size_t i=0;i<3;i++) if(values[i] && strlen(values[i])>16u*1024u*1024u) return false;
    size_t count=0; for(ClipboardRequest *r=clipboard_requests;r;r=r->next) if(++count>=8) return false;
    ClipboardRequest *request=malloc(sizeof *request); if(!request) return false;
    *request=(ClipboardRequest){clipboard_next_id++,completion,userdata,clipboard_requests};
    clipboard_requests=request;
    if(!clipboard_start_js(request->id,writing,plain,html,typed)) {
        clipboard_requests=request->next; free(request); return false;
    }
    return true;
}
bool platform_web_clipboard_write(const char *plain,const char *html,const char *typed,
    PlatformWebClipboardCompletion completion,void *userdata) {
    return plain && clipboard_start(true,plain,html,typed,completion,userdata);
}
bool platform_web_clipboard_read(PlatformWebClipboardCompletion completion,void *userdata) {
    return clipboard_start(false,NULL,NULL,NULL,completion,userdata);
}
void platform_web_input_init(void) { clipboard_closing=false; focus_requested=false; input_event = SDL_RegisterEvents(1); init_js(); }
void platform_web_input_clipboard(PlatformWebClipboardRead read, PlatformWebClipboardCut cut,
    PlatformWebClipboardError error, SDL_AppResult (*handler)(void *, SDL_Event *), void *state) {
    clipboard_read = read; clipboard_cut = cut; clipboard_error = error;
    clipboard_handler = handler; clipboard_state = state;
}
void platform_web_input_clipboard_rich(PlatformWebClipboardPrepare prepare,
    PlatformWebClipboardFinish finish,PlatformWebClipboardPaste paste,void *state) {
    platform_web_clipboard_finish(0);
    rich_prepare=prepare;rich_finish=finish;rich_paste=paste;rich_state=state;
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
    clipboard_closing=true; clipboard_cancel_js();
    platform_web_clipboard_finish(0);
    rich_prepare=NULL;rich_finish=NULL;rich_paste=NULL;rich_state=NULL;
    while(clipboard_requests) {
        ClipboardRequest *request=clipboard_requests; clipboard_requests=request->next;
        request->completion(false,NULL,NULL,NULL,"Clipboard request canceled",request->userdata); free(request);
    }
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
void platform_web_input_clipboard_rich(PlatformWebClipboardPrepare prepare,
    PlatformWebClipboardFinish finish,PlatformWebClipboardPaste paste,void *state) {
    (void)prepare;(void)finish;(void)paste;(void)state;
}
void platform_web_input_update(int target, SDL_Window *window, const SDL_FRect *area) { (void)target; (void)window; (void)area; }
void platform_web_input_cancel(void) {}
void platform_web_input_request_focus(void) {}
bool platform_web_clipboard_write(const char *plain,const char *html,const char *typed,
    PlatformWebClipboardCompletion completion,void *userdata) {
    (void)plain;(void)html;(void)typed;(void)completion;(void)userdata;return false;
}
bool platform_web_clipboard_read(PlatformWebClipboardCompletion completion,void *userdata) {
    (void)completion;(void)userdata;return false;
}
bool platform_web_input_handle_event(SDL_Event *event, SDL_AppResult (*handler)(void *, SDL_Event *), void *state) { (void)event; (void)handler; (void)state; return false; }
void platform_web_input_destroy(void) {}
#endif
