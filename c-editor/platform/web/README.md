# Browser build and verification

The browser application uses the same C document/storage code as the native
application. Emscripten produces `notelab.html`, `notelab.js`, `notelab.wasm` and
`notelab.data`; serve all four from the same directory. The data file contains
the licensed Noto CJK and monochrome Noto Emoji fonts. Node is needed only for the integration test.

```sh
# Activate an installed Emscripten SDK first; verified locally with 4.0.14.
source /path/to/emsdk/emsdk_env.sh
emcmake cmake -S c-editor -B c-editor/build-web \
  -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=OFF
cmake --build c-editor/build-web --target notelab --parallel 4
python3 -m http.server 8000 --directory c-editor/build-web
# Open http://localhost:8000/notelab.html
```

Use HTTP(S), not `file://`. Notes are stored in IndexedDB for the current origin;
changing the protocol, hostname or port creates a different storage location.
Clearing browser site data removes those notes. Export JSON for a portable copy.

The shell mounts `/notes` and completes an initial IndexedDB read before the C
application starts. A read error leaves an explicit error message and prevents
an empty workspace from starting. Later saves remain marked unsaved until the
IndexedDB callback succeeds. Pending or failed saves, including text queued before the next frame, trigger
the standard browser navigation warning where supported. Canceling the warning
keeps the C application running.

A native textarea supplies text, IME composition and mobile keyboard input to
the canvas editor. Its events share SDL's event queue with physical keys so
rapid typing, selection movement and filename confirmation retain their order.
Only committed text enters the document; IME preedit is temporary. Clicking or
tapping the editing area focuses the textarea within a browser gesture.

## Reproducible integration check

```sh
# Node 22+ with built-in WebSocket and Chromium/Chrome must be installed.
node c-editor/tests/test_browser.mjs c-editor/build-web /path/to/chromium
# Or set CHROME_BIN. macOS defaults to the standard Google Chrome location.
```

The check starts a temporary local server and an isolated browser profile,
creates its own test note, then removes the profile. It exercises the compiled
WebAssembly application and real IndexedDB, including Unicode filenames/text,
IME commit/cancel, physical and software input, saves while editing, injected
storage failures and retry, canceled navigation, reload recovery, a narrow
viewport, downloaded JSON contents and clipboard failures. The default check
uses real DOM ClipboardEvents with a private DataTransfer object and leaves the
operating-system clipboard untouched. On a disposable CI runner only, set
`EDITOR_TEST_SYSTEM_CLIPBOARD=1` to also exercise native keyboard copy/cut/paste
and read back the known fixture through the Clipboard API. That opt-in check
overwrites the clipboard with test text before reading it.
Screenshots are written to the build directory as `browser-validation.png`,
`browser-mobile-validation.png`, `browser-emoji-validation.png` and
`browser-emoji-bold-validation.png`. Emoji screenshots must be inspected for
actual glyph/ZWJ rendering; byte-preservation assertions alone cannot prove
font rendering. The integrated checks passed locally with
Emscripten 4.0.14 and Chrome 157 on macOS.

This is Chromium integration evidence. Safari, Firefox, Android/iOS keyboard UI,
accessibility, browser quota eviction and device behavior still need platform
validation. The C rewrite's remaining feature parity work is tracked in the
main `c-editor/README.md`.

## Bundled emoji coverage

`cmake/EmojiFont.cmake` pins the official Google Fonts Noto Emoji 3.002 binary
and verifies its SHA256 before copying it into the build. `assets/OFL-Emoji.txt`
is its SIL Open Font License. The font uses monochrome outlines; it does not
provide platform-specific colored emoji. Some flags use lettered flag artwork.

The renderer uses the emoji font as the primary font for complete emoji
clusters, with the CJK font as fallback. This is necessary because a CJK font
may itself contain zero-width joiners and otherwise split a sequence before the
emoji fallback can shape it. Derived bold/heading fonts need matching fallback
size and style as well. The actual browser screenshots verify 🙂, 👍🏽, 👩‍💻,
👨‍👩‍👧‍👦 and 🇨🇳 in normal and bold text, plus an emoji in a note filename.
This set does not establish coverage of every Unicode emoji or every input
method/device combination.


## Clipboard behavior

The web editor handles plain-text clipboard events through the browser. Copy
uses the current selection, including selections made immediately before the
shortcut. Cut first writes the exact selected text to the event's clipboard
and confirms it was accepted; an exception or silently ignored write leaves
the document intact. The temporary UTF-8 selection buffer is released on both
success and failure. Paste enters the same ordered SDL input stream as typing.
Native textarea events with multiple lines are consumed once, even when the
browser emits several input notifications for a single edit.
