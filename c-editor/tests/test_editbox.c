#include "editbox.h"
#include <assert.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>

static const char *contents(EditBox *edit) { return edit->text->text ? edit->text->text : ""; }
static void expect(EditBox *edit, const char *value) { assert(!strcmp(contents(edit), value)); }
static void select_range(EditBox *edit, int start, int end) {
    edit->highlight1 = start;
    edit->highlight2 = end;
    edit->cursor = end;
}
static void preedit(EditBox *edit, const char *value) {
    SDL_Event event;
    SDL_zero(event);
    event.type = SDL_EVENT_TEXT_EDITING;
    event.edit.text = value;
    EditBox_HandleEvent(edit, &event);
}
static void commit(EditBox *edit, const char *value) {
    SDL_Event event;
    SDL_zero(event);
    event.type = SDL_EVENT_TEXT_INPUT;
    event.text.text = value;
    EditBox_HandleEvent(edit, &event);
}
static void key_mod(EditBox *edit, SDL_Keycode code, SDL_Keymod mod) {
    SDL_Event event;
    SDL_zero(event);
    event.type = SDL_EVENT_KEY_DOWN;
    event.key.key = code;
    event.key.mod = mod;
    EditBox_HandleEvent(edit, &event);
}
static void key(EditBox *edit, SDL_Keycode code) { key_mod(edit,code,SDL_KMOD_NONE); }

typedef struct { bool enabled; unsigned draws, selections; int step; } HookProbe;
static bool probe_prepare(void *p,EditBox *edit) { (void)edit; return ((HookProbe*)p)->enabled; }
static bool probe_caret(void *p,int offset,bool trailing,SDL_FRect *rect) {
    (void)p;
    bool second=offset>2 || (offset==2 && !trailing);
    *rect=(SDL_FRect){100+(float)(offset-(second?2:0))*10,second?50:0,1,40};
    return true;
}
static int probe_hit(void *p,float x,float y) {
    (void)p; int start=y>=40?2:0;
    return start+SDL_clamp((int)SDL_roundf((x-100)/10),0,2);
}
static int probe_step(void *p,int offset,int direction) {
    int stride=((HookProbe*)p)->step;
    return SDL_clamp(offset+direction*(stride?stride:1),0,4);
}
static void probe_selection(void *p,int start,int end,EditBoxRectCallback callback,void *context) {
    ((HookProbe*)p)->selections++;
    SDL_FRect rect={100,0,(float)(end-start)*10,40}; callback(&rect,context);
}
static bool probe_draw(void *p,float x,float y,int start,int end) {
    (void)x; (void)y; (void)start; (void)end; ((HookProbe*)p)->draws++; return true;
}
static const EditBoxLayout probe_hooks={probe_prepare,probe_caret,probe_hit,probe_step,probe_selection,probe_draw};
static void test_layout_hooks(EditBox *edit) {
    assert(EditBox_SetText(edit,"abcd"));
    HookProbe probe={true,0,0,0}; EditBox_SetLayout(edit,&probe_hooks,&probe);
    edit->cursor=0;
    key(edit,SDLK_END); assert(edit->cursor==2 && edit->cursor_trailing);
    SDL_FRect rect; assert(EditBox_CaretRect(edit,2,&rect)); assert(rect.x==120 && rect.y==0);
    EditBox_MoveCursorDown(edit); assert(edit->cursor==4);
    key(edit,SDLK_HOME); assert(edit->cursor==2 && !edit->cursor_trailing);
    EditBox_MoveCursorUp(edit); assert(edit->cursor==0);
    EditBox_MoveCursorRight(edit); assert(edit->cursor==1);
    EditBox_MoveCursorLeft(edit); assert(edit->cursor==0);
    key_mod(edit,SDLK_END,SDL_KMOD_CTRL); assert(edit->cursor==4);
    key_mod(edit,SDLK_HOME,SDL_KMOD_CTRL); assert(edit->cursor==0);
    key_mod(edit,SDLK_END,SDL_KMOD_GUI); assert(edit->cursor==4);
    key_mod(edit,SDLK_HOME,SDL_KMOD_GUI|SDL_KMOD_CTRL); assert(edit->cursor==0);
    probe.step=2;
    edit->cursor=2; key(edit,SDLK_BACKSPACE); expect(edit,"cd");
    assert(EditBox_SetText(edit,"abcd")); edit->cursor=0;
    key(edit,SDLK_DELETE); expect(edit,"cd");
    assert(EditBox_SetText(edit,"abcd")); select_range(edit,1,2);
    key(edit,SDLK_BACKSPACE); expect(edit,"acd"); /* Selection wins over 2-byte cluster hook. */
    assert(EditBox_SetText(edit,"abcd")); probe.step=0;
    SDL_Event event; SDL_zero(event);
    event.type=SDL_EVENT_MOUSE_BUTTON_DOWN; event.button.button=SDL_BUTTON_LEFT; event.button.x=101; event.button.y=10;
    assert(EditBox_HandleEvent(edit,&event)); assert(edit->highlight1==0);
    event.type=SDL_EVENT_MOUSE_MOTION; event.motion.x=119; event.motion.y=60;
    assert(EditBox_HandleEvent(edit,&event)); assert(edit->highlight2==4);
    event.type=SDL_EVENT_MOUSE_BUTTON_UP; assert(EditBox_HandleEvent(edit,&event));
    EditBox_Draw(edit); assert(probe.draws==1);
    select_range(edit,0,2); preedit(edit,"ni"); EditBox_Draw(edit);
    assert(probe.selections && probe.draws==2);
    assert(EditBox_CancelComposition(edit)); expect(edit,"abcd");
    probe.enabled=false; assert(EditBox_CaretRect(edit,0,&rect)); assert(rect.x<100);
    EditBox_Draw(edit); assert(probe.draws==2);
    EditBox_SetLayout(edit,NULL,NULL);
}

static void test_keyboard_boundaries(EditBox *edit) {
    const char *clusters[]={"e\xcc\x81","中","🙂","👍🏽","👩‍💻","👨‍👩‍👧‍👦","🇨🇳"};
    for(size_t i=0;i<SDL_arraysize(clusters);i++) {
        char text[128]; SDL_snprintf(text,sizeof text,"L%sR",clusters[i]);
        assert(EditBox_SetText(edit,text)); edit->cursor=1+(int)SDL_strlen(clusters[i]);
        key(edit,SDLK_BACKSPACE); expect(edit,"LR"); assert(edit->cursor==1);
        assert(EditBox_SetText(edit,text)); edit->cursor=1;
        key(edit,SDLK_DELETE); expect(edit,"LR"); assert(edit->cursor==1);
    }
    assert(EditBox_SetText(edit,"Le\xcc\x81R")); select_range(edit,2,4);
    key(edit,SDLK_DELETE); expect(edit,"LeR"); /* Explicit partial selection remains authoritative. */
    assert(EditBox_SetText(edit,"aaaa\nbbbb\ncccc")); edit->cursor=12;
    key(edit,SDLK_HOME); assert(edit->cursor==10);
    key(edit,SDLK_UP); assert(edit->cursor==5);
    commit(edit,"Y"); key(edit,SDLK_END); assert(edit->cursor==10);
    commit(edit,"Z"); expect(edit,"aaaa\nYbbbbZ\ncccc");
    key_mod(edit,SDLK_HOME,SDL_KMOD_CTRL); assert(edit->cursor==0);
    key_mod(edit,SDLK_END,SDL_KMOD_GUI); assert(edit->cursor==16);
    const SDL_Keymod word_mods[]={SDL_KMOD_CTRL,SDL_KMOD_ALT};
    for(size_t i=0;i<SDL_arraysize(word_mods);i++) {
        SDL_Keymod mod=word_mods[i];
        assert(EditBox_SetText(edit,"one two\nthree four")); edit->cursor=18;
        key_mod(edit,SDLK_BACKSPACE,mod); expect(edit,"one two\nthree "); assert(edit->cursor==14);
        key_mod(edit,SDLK_LEFT,mod); assert(edit->cursor==8);
        key_mod(edit,SDLK_BACKSPACE,mod); expect(edit,"one two\nthree "); /* Never cross LF. */
        key_mod(edit,SDLK_RIGHT,mod); assert(edit->cursor==13);
        assert(EditBox_SetText(edit,"one two\nthree four")); edit->cursor=4;
        key_mod(edit,SDLK_DELETE,mod); expect(edit,"one \nthree four");
        key_mod(edit,SDLK_DELETE,mod); expect(edit,"one \nthree four");
        assert(EditBox_SetText(edit,"one two\nthree four")); edit->cursor=3;
        key_mod(edit,SDLK_DELETE,mod); expect(edit,"one\nthree four");
        assert(EditBox_SetText(edit,"one 🙂 two\nkeep")); edit->cursor=8;
        key_mod(edit,SDLK_BACKSPACE,mod); expect(edit,"one  two\nkeep");
        assert(EditBox_SetText(edit,"one 🙂 two\nkeep")); edit->cursor=4;
        key_mod(edit,SDLK_DELETE,mod); expect(edit,"one  two\nkeep");
        assert(EditBox_SetText(edit,"one two\nthree four")); select_range(edit,4,9);
        key_mod(edit,SDLK_BACKSPACE,mod); expect(edit,"one hree four");
    }
    const SDL_Keymod line_mods[]={SDL_KMOD_GUI,SDL_KMOD_GUI|SDL_KMOD_CTRL};
    for(size_t i=0;i<SDL_arraysize(line_mods);i++) {
        SDL_Keymod mod=line_mods[i];
        assert(EditBox_SetText(edit,"one two\nthree four")); edit->cursor=13;
        key_mod(edit,SDLK_BACKSPACE,mod); expect(edit,"one two\n four"); assert(edit->cursor==8);
        assert(EditBox_SetText(edit,"one two\nthree four")); edit->cursor=2;
        key_mod(edit,SDLK_DELETE,mod); expect(edit,"on\nthree four");
        assert(EditBox_SetText(edit,"one two\nthree four")); edit->cursor=10;
        key_mod(edit,SDLK_LEFT,mod); assert(edit->cursor==8);
        key_mod(edit,SDLK_RIGHT,mod); assert(edit->cursor==18);
        select_range(edit,0,3); key_mod(edit,SDLK_DELETE,mod); expect(edit," two\nthree four");
    }
}

int main(int argc, char **argv) {
    SDL_Window *window;
    SDL_Renderer *renderer;
    TTF_Font *font;
    TTF_TextEngine *engine;
    EditBox *edit;
    SDL_FRect rect = {0, 0, 320, 200};
    if (argc != 2) { fprintf(stderr, "Usage: test_editbox FONT_FILE\n"); return 2; }
    /* Never touch the real desktop or the user's clipboard. The dummy driver's
     * clipboard is an in-process string owned by this test. */
    assert(SDL_SetEnvironmentVariable(SDL_GetEnvironment(), "SDL_VIDEODRIVER", "dummy", true));
    assert(SDL_Init(SDL_INIT_VIDEO));
    assert(!strcmp(SDL_GetCurrentVideoDriver(), "dummy"));
    assert(TTF_Init());
    window = SDL_CreateWindow("EditBox tests", 320, 200, SDL_WINDOW_HIDDEN);
    assert(window);
    renderer = SDL_CreateRenderer(window, "software");
    assert(renderer);
    font = TTF_OpenFont(argv[1], 16);
    if (!font) fprintf(stderr, "Cannot open test font: %s\n", SDL_GetError());
    assert(font);
    engine = TTF_CreateRendererTextEngine(renderer);
    assert(engine);
    edit = EditBox_Create(window, renderer, engine, font, &rect);
    assert(edit);
    EditBox_SetFocus(edit, true);

    assert(EditBox_SetText(edit, "original text"));
    edit->cursor = 8;
    assert(SDL_SetClipboardText("clipboard sentinel"));
    EditBox_Copy(edit);
    {
        char *clipboard=SDL_GetClipboardText();
        assert(clipboard && !strcmp(clipboard,"clipboard sentinel"));
        SDL_free(clipboard);
    }
    EditBox_Cut(edit);
    expect(edit, "original text");
    assert(edit->cursor == 8);

    select_range(edit, 3, 12);
    /* Simulate an old caller replacing text without resetting its selection. */
    assert(TTF_SetTextString(edit->text, "x", 0));
    EditBox_Copy(edit);
    {
        char *clipboard = SDL_GetClipboardText();
        assert(clipboard && !strcmp(clipboard, "clipboard sentinel"));
        SDL_free(clipboard);
    }
    assert(edit->cursor == 1 && edit->highlight1 == -1 && edit->highlight2 == -1);
    EditBox_Cut(edit);
    expect(edit, "x");
    edit->cursor = INT_MAX;
    EditBox_Backspace(edit);
    expect(edit, "");
    assert(edit->cursor == 0);

    assert(EditBox_SetText(edit, "中A"));
    edit->cursor = 2; /* Inside a three-byte UTF-8 character. */
    EditBox_Insert(edit, "X");
    expect(edit, "X中A");
    assert(edit->cursor == 1);
    edit->cursor = 3;
    EditBox_Delete(edit);
    expect(edit, "XA");
    assert(edit->cursor == 1);
    select_range(edit, 0, 2);
    EditBox_Insert(edit, "\xc0\xaf"); /* Invalid UTF-8 must not delete selection. */
    expect(edit, "XA");

    assert(EditBox_SetText(edit, "原文ABC"));
    select_range(edit, 0, 6);
    preedit(edit, "ni");
    expect(edit, "niABC");
    assert(edit->composition_snapshot && edit->composition_length == 2);
    preedit(edit, "nih");
    expect(edit, "nihABC");
    /* Return is for the IME to handle; it cannot insert a newline/commit pinyin. */
    key(edit, SDLK_RETURN);
    expect(edit, "nihABC");
    preedit(edit, "");
    expect(edit, "原文ABC");
    assert(!edit->composition_snapshot && !edit->composition_length);
    assert(edit->cursor == 6 && edit->highlight1 == 0 && edit->highlight2 == 6);
    preedit(edit, "ni");
    commit(edit, "你");
    expect(edit, "你ABC");
    assert(edit->cursor == 3 && !edit->composition_snapshot && !edit->composition_length);
    preedit(edit, ""); /* A trailing editing-ended event must not undo the commit. */
    expect(edit, "你ABC");
    commit(edit, "好");
    expect(edit, "你好ABC");

    select_range(edit, 0, 6);
    preedit(edit, "test");
    EditBox_SetFocus(edit, false);
    expect(edit, "你好ABC");
    assert(!edit->has_focus && !edit->composition_snapshot);
    commit(edit, "ignored");
    preedit(edit, "ignored");
    expect(edit, "你好ABC");
    EditBox_SetFocus(edit, true);
    preedit(edit, "pending");
    assert(EditBox_SetText(edit, "another document"));
    assert(EditBox_CancelComposition(edit));
    expect(edit, "another document");

    select_range(edit, 0, 7);
    preedit(edit, "pinyin");
    key(edit, SDLK_ESCAPE);
    expect(edit, "another document");
    assert(!edit->composition_snapshot && edit->has_focus);

    /* Valid cut and paste also exercise dummy clipboard ownership. */
    select_range(edit, 0, 7);
    EditBox_Cut(edit);
    expect(edit, " document");
    assert(edit->cursor == 0);
    EditBox_Paste(edit);
    expect(edit, "another document");
    test_layout_hooks(edit);
    test_keyboard_boundaries(edit);
    assert(EditBox_SetText(edit,"another document"));
    select_range(edit, 0, 7);
    preedit(edit, "unfinished"); /* Destroy frees active composition snapshot. */
    EditBox_Destroy(edit);
    TTF_DestroyRendererTextEngine(engine);
    TTF_CloseFont(font);
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    TTF_Quit();
    SDL_Quit();
    puts("editbox: all tests passed");
    return 0;
}
