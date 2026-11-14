# Note Lab — C rewrite

The target is the same note editor on **Windows, macOS, Linux, Android, iOS and
the browser**, with the existing rich-text and Zotero workflows. This directory
is an ongoing rewrite, not a completed replacement. `../test-app` remains intact.

Application code and document/storage logic are C11. SDL3 handles windows and
input; SDL_ttf, FreeType and HarfBuzz handle Unicode shaping. HarfBuzz is a C API
implemented in C++, and the platform SDKs supply their required native glue.
The new application does not depend on React, TypeScript, Node or Rust at runtime.

## Build and run on the development machine

```sh
cmake -S c-editor -B c-editor/build -DCMAKE_BUILD_TYPE=Debug
cmake --build c-editor/build --parallel 4
ctest --test-dir c-editor/build --output-on-failure
./c-editor/build/notelab
```

The first configuration downloads pinned source archives, Noto Sans CJK SC and
a monochrome Noto Emoji fallback font. Versions and checksums are in
`cmake/Dependencies.cmake`, `cmake/EmojiFont.cmake` and `cmake/SQLite.cmake`.
An installed SDL3 can be used on the host. For fully source-built dependencies,
pass `-DEDITOR_USE_SYSTEM_SDL=OFF`. For an offline local font override, pass
`-DEDITOR_FONT_PATH=/absolute/path/to/font.ttf`; redistribution rights for an
override font are the caller's responsibility.

The default notes directory is the platform's application storage directory.
Use `--notes /absolute/path/to/a/copied-notes-folder` to test an existing set of
notes. Use a copy during migration. The old `.md` files often contain HTML,
despite their extension; they must not be treated as plain Markdown.

## Tests

```sh
cmake -S c-editor -B c-editor/build-core \
  -DEDITOR_BUILD_APP=OFF -DEDITOR_SANITIZERS=ON -DCMAKE_BUILD_TYPE=Debug
cmake --build c-editor/build-core --parallel 4
ctest --test-dir c-editor/build-core --output-on-failure
```

Tests use isolated temporary directories and dummy SDL video. They do not open
the user's real notes or Zotero library. Assertions remain enabled in Release
test builds. The core tests cover Unicode editing, formatting and legacy
roundtrips, undo/savepoints, malformed-input rollback, randomized replacements,
atomic-write failure, symlink rejection and filename portability. The input
tests cover stale selections, IME cancellation/commit and clipboard edge cases.

## Current implementation and remaining acceptance work

| Area | Current evidence / remaining work |
| --- | --- |
| C document core | Tested text/HTML/TipTap JSON import/export, rich marks, alignment, history, search, citation/endnote preservation, nested-list Enter/exit, atom-adjacent insertion and paragraph splitting. Same-container block/list-item movement is supported; arbitrary cross-container moves remain restricted. |
| Note storage | Tested Unicode filenames, exclusive creation, atomic saving, read/delete/list, failure propagation. Windows code exists but needs execution on Windows. |
| Native interface | macOS rich-text rendering and input tests pass: headings, marks, colors, nested lists, alignment, selection, caret geometry, IME, reference cards and emoji shaping. Autosave, themes, resizable sidebar, repeat search and file export are wired. Bibliography panels remain in progress. RTL/bidi text uses the plain layout to preserve safe editing; full rich bidi layout and Unicode line-breaking rules remain. |
| Browser | Actual Emscripten 4.0.14 build and Chrome integration tests pass: IME preedit/commit/cancel, Unicode input, IndexedDB save/reload, failure/retry, editing during saves, canceling navigation and JSON file downloads. Safari/Firefox and mobile browser device validation remain. |
| Windows / Linux | Portable source path exists. Full build, input and packaging checks are required. |
| Android | Gradle/NDK wrapper, matching SDL Java bridge and font packaging are provided. The development host has no Android SDK; APK compilation and emulator/device validation remain. |
| iOS | Actual arm64 simulator and device bundles compile with Xcode 27.2, with iPhone/iPad configuration and the verified CJK font/license. They are unsigned. Simulator launch was blocked by the host's CoreSimulator service/SDK mismatch; runtime and device interaction remain unverified. |
| Zotero | Tested C CSL import and read-only modern/legacy SQLite loading, group-library links, actual PDF-attachment links, deleted-item/collection filtering and descendant collection queries. Citation insertion/editing, library UI, full CSL formatting and bibliography workflows remain in progress. Approximate formatters are explicitly named as such. |
| Distribution | Platform install packages, update behavior and accessibility remain to be completed and verified. |

The six-platform objective is not satisfied by compilation on one host. Final
acceptance requires platform builds and runtime checks, editing/formatting
parity, safe legacy migration, persistent browser saves, and usable Zotero
workflows. Desktop-only access to a running Zotero instance or its private
database must be exposed honestly; mobile/browser users need an import workflow.

Build details and reproducible platform checks:

- [Browser](platform/web/README.md)
- [Android](platform/android/README.md)
- [iOS](platform/ios/README.md)

The C editor workflow includes desktop, sanitizer, browser, Android and iOS
jobs. Merely adding those jobs does not establish that a GitHub run has passed.

The SQLite importer is also tested as actual WebAssembly code, including
modern/legacy database fixtures and failure rollback:

```sh
# After activating Emscripten; '/' here is the WASM in-memory filesystem root.
emcmake cmake -S c-editor -B c-editor/build-wasm-tests \
  -DEDITOR_BUILD_APP=OFF -DBUILD_TESTING=ON
cmake --build c-editor/build-wasm-tests --target test_zotero --parallel 4
node c-editor/build-wasm-tests/test_zotero.js /
```

## Platform references

- [SDL CMake guide](https://wiki.libsdl.org/SDL3/README-cmake)
- [SDL Android integration](https://wiki.libsdl.org/SDL3/README-android)
- [SDL iOS integration](https://wiki.libsdl.org/SDL3/README-ios)
- [SDL browser integration](https://wiki.libsdl.org/SDL3/README-emscripten)
- [Emscripten persistent filesystems](https://emscripten.org/docs/api_reference/Filesystem-API.html)

These documents describe toolkit capabilities, not proof that this application's
six-platform acceptance checks have passed.
