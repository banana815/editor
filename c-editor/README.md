# Note Lab — C editor

This directory is the active implementation of Note Lab for **Windows, macOS,
Linux, Android, iOS and the browser**, including rich-text notes and Zotero
workflows. The retired Tauri application is available in Git history. Legacy
note import remains supported; feature parity and final acceptance work are
tracked in the project status document.

For the pause checkpoint, verified results and remaining work, see
[项目状态与交接](PROJECT_STATUS.md). Local validation and remote CI results
are recorded separately; further feature development remains paused.

Application code and document/storage logic are C11. The iOS file picker uses
the platform's Objective-C API behind a C interface. SDL3 handles windows and
input; SDL_ttf, FreeType and HarfBuzz handle Unicode shaping. HarfBuzz is a C API
implemented in C++, and the platform SDKs supply their required native glue.
The new application does not depend on React, TypeScript, Node or Rust at runtime.

## Build and run on the development machine

Install CMake 3.24 or newer, a C/C++ toolchain and Python 3.8 or newer. Python
verifies and embeds the pinned citation styles during the build; the installed
application does not need Python.

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

The sidebar's **导入** button reads a selected old note and creates a new `.json`
copy. HTML/HTM preserves supported rich formatting and citation blocks; JSON
preserves unknown attributes; TXT is literal text, and MD uses content detection
for compatibility with the old HTML-in-MD files. The input limit is 128 MiB.
The original file is never overwritten. Existing names receive a numbered
suffix, and the current note must save successfully before switching documents.

Copy and paste carry a C-validated document slice: text marks, list ancestry,
citation source snapshots and unknown attributes. Plain text uses readable
citation labels; escaped HTML carries the private slice when a browser cannot
transfer its custom clipboard type. Clipboard representations are bounded to
16 MiB, 250,000 JSON values and 128 nested containers. These limits do not claim
one shared allocation budget. Complex replacements across incompatible list
ancestors fail explicitly. Pasted citations receive distinct identifiers and
pass the same whole-document processing as other edits. A cut deletes only
after clipboard acceptance and verification of its original note, revision and
selection; failed or stale operations preserve the document. The touch menu
requires multi-format `ClipboardItem`/`write` support for rich copying; otherwise
it fails without cutting. Browser-native copy/cut events remain supported, and
plain-only prompts can use `writeText`.

Formatting at an empty caret sets the marks for subsequent typing without
creating a document edit. Each committed text input remains one undo step,
including IME input. On touch screens, dragging scrolls without moving the
caret; tapping focuses input, and a long press selects text for the floating
copy/cut/paste/select-all menu. Mouse and pen input retain their normal behavior.

List items support one-level indent/outdent through toolbar actions and
Tab/Shift+Tab. Selections retain their text offsets and each operation is one
undo step. Clear formatting also resets selected headings, alignment and list
structure; at a caret it changes the block and subsequent typing, preserving
marks on existing unselected text. Unknown wrapper metadata that cannot survive
unwrapping causes an explicit failure.

The library panel keeps independent checkboxes across search and collection
filters. “勾选结果” selects the current filtered items, “清空勾选” clears all
checks, and “设为书目” replaces the explicit reading-list selection. The resulting
bibliography combines those sources with actual body citations. It does not
insert fake citation blocks. Changing the style, clearing the selection and
deleting a body citation all use the same C processing and undo transaction.
Opening the panel with selected prose copies that text into the quotation field.

“连接来源” explicitly downloads CSL JSON from an editable HTTP(S) URL;
“刷新来源” rereads the saved URL or a previously selected desktop SQLite file.
File imports on browsers and mobile devices ask the user to select the file
again. Sources are never contacted automatically on startup. Bytes are parsed
in C before replacing the library; cancellation, malformed content and failed
requests preserve the previous library. The source configuration and its
library snapshot are saved together in the local cache.

Transport uses WinHTTP, NSURLSession, Android HttpURLConnection, system libcurl
on Linux, and browser fetch. Application decisions and CSL parsing stay in C;
platform bridge code only transfers bytes. Responses are limited to 128 MiB
after decompression and normally 30 seconds. Redirects are rejected and TLS
verification remains enabled. Plain HTTP is limited to loopback, private/link
local IP addresses and localhost/.localhost/.local names. URLs must be ASCII
with non-ASCII path/query bytes percent-encoded. The Android bridge relies on
this application not installing a global Authenticator; it rejects an installed
CookieHandler instead of changing global handlers.

Browser CORS and local-network permissions still apply. Better BibTeX pull
exports reject browser access, so use file import there, or a CSL JSON endpoint
that permits the browser origin. On a phone, `127.0.0.1` refers to the phone.
The desktop SQLite refresh always uses the existing read-only WAL-aware loader;
mobile file-provider grants are not fabricated or retained as reusable paths.

## Desktop preview packages

```sh
cmake -S c-editor -B c-editor/build-package \
  -DCMAKE_BUILD_TYPE=Release -DEDITOR_DISTRIBUTION=ON
cmake --build c-editor/build-package --config Release --parallel 4
ctest --test-dir c-editor/build-package -C Release --output-on-failure
cpack --config c-editor/build-package/CPackConfig.cmake -C Release
python3 c-editor/tests/test_distribution.py c-editor/build-package/packages
```

This produces a ZIP with a macOS application bundle or Windows executable, or
a Linux tar.gz, including fonts and third-party notices. Extract the complete
archive before launching the application. The archive is specific to the OS
and architecture used for the build. Application libraries are linked statically;
Linux still uses the host's system libraries, libcurl (7.84 or later) and window services. The CI Linux
package targets Ubuntu 24.04 and compatible environments.

Packages are development previews. Apple signing/notarization, Windows signing
and automatic updates are not configured. The package test extracts into an
isolated directory and starts that copy with temporary notes and bundled fonts.
It does not install anything or change the user's notes. Dependency versions,
attribution and license texts are recorded in [THIRD_PARTY.md](THIRD_PARTY.md).

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
| C document core | Tested text/HTML/TipTap JSON import/export, rich marks, alignment, history, search, citation/endnote preservation, nested-list Enter/exit, indent/outdent, full clear-formatting, atom-adjacent insertion and paragraph splitting. Same-container block/list-item movement is supported; arbitrary cross-container moves remain restricted. |
| Note storage | Unicode filenames, exclusive creation, atomic saving, read/delete/list and failure propagation pass on macOS, Windows and Linux CI. |
| Native interface | Rich-text rendering, input, headings, marks, lists, alignment, selection, IME, reference cards and emoji shaping are tested. Autosave, themes, resizable sidebar, repeat search, file export and safe legacy import are wired. The library panel supports search, collection filtering, citation insertion/editing and undo. Logical-pixel and device-safe-area layout keep controls and IME within the usable mobile area. RTL/bidi text uses the plain layout; full rich bidi layout and Unicode line-breaking rules remain. |
| Browser | Actual Emscripten/Chrome integration tests cover IME, Unicode, IndexedDB save/reload and failures, navigation cancellation, real system clipboard, JSON download, file chooser import and the citation workflow. Safari/Firefox and mobile browser device validation remain. |
| Windows / Linux | Full desktop builds, C tests and dummy-video application startup pass in GitHub Actions. Interactive OS-specific input and install-package validation remain. |
| Android | Release/debug APKs, two native ABIs and font resources pass CI. The C application actually launches and renders on an Android emulator. Physical-device input, lifecycle and file-provider interaction still need validation. |
| iOS | Unsigned simulator/device bundles compile; the C app actually launches and renders on a GitHub-hosted iPhone simulator. New UIKit import/export bridges also link in complete local device/simulator builds. Actual file-picker/provider interactions and hardware validation remain. |
| Zotero and citations | CSL JSON and read-only modern/legacy SQLite import, collection queries and cross-library identities are supported. The C processor is connected to citation insertion/editing, numbering, bibliography generation, style changes and undo. Pinned APA 7, IEEE 2023 and GB/T 7714-2015 numeric profiles produce controlled formatting; unsupported constructs fail explicitly. Source snapshots and unknown metadata are preserved. See the processing limits below. |
| Distribution | Desktop preview archives include fonts and notices; the extracted macOS arm64 bundle passes startup independently of the build directory. Windows/Linux archive checks are in CI. Mobile artifacts remain unsigned, and signing, update behavior and accessibility remain to be completed and verified. |

The six-platform objective is not satisfied by compilation on one host. Final
acceptance requires platform builds and runtime checks, editing/formatting
parity, safe legacy migration, persistent browser saves, and usable Zotero
workflows. Desktop-only access to a running Zotero instance or its private
database must be exposed honestly; mobile/browser users need an import workflow.

Build details and reproducible platform checks:

- [Browser](platform/web/README.md)
- [Android](platform/android/README.md)
- [iOS](platform/ios/README.md)

The checkpoint at commit `8edc978de1dfedb09937dd569417600992444f29` passed all
eight desktop, sanitizer, browser, Android and iOS jobs in
[application run 43686573923](https://github.com/banana815/editor/actions/runs/43686573923),
plus all nine [citation processor jobs](https://github.com/banana815/editor/actions/runs/43686573919).
Android and iOS results include a success marker from the C application and a
complete rendered screenshot. This is evidence for that commit's startup;
later feature changes need their own checks, and startup alone does not verify
complete mobile usability or feature parity.

The **文献** button opens the library panel. Import CSL JSON on any platform,
or select a SQLite database. Desktop SQLite reads use the original path and a
read-only transaction so committed WAL data remains visible. Browser/mobile
single-file SQLite imports use a bounded, read-only memory snapshot and reject
WAL-mode main files because their separate journal may contain newer data;
export CSL JSON or a standalone SQLite backup in that case. Failed imports
retain the current library and note. Select a document, set page/excerpt, then
insert; use the current-citation view to edit it. Insertion and citation edits
update the bibliography in the same undo transaction. Deleting or moving a
citation recalculates numbering and disambiguation. The style button cycles
**APA 7**, **IEEE 2023** and **GB/T 7714-2015** and persists that choice in the
note, including undo/redo. **更新参考文献** explicitly rebuilds the revision.

The processing implementation and profile resources are C and embedded data;
no JavaScript processor or interpreter runs citation logic. Native and browser
builds use the same pipeline. Formatting retains italic/bold/underline,
superscript/subscript, bibliography columns, hanging indentation and spacing.
Rendered cards remain atomic editor selections. User quotations are displayed
separately and do not become CSL locators or note numbers.

An embedded `cslJson` snapshot is authoritative. A legacy citation without one
may use a unique exact match from the imported library; author/year display
strings are never turned into invented structured metadata. Conflicting
snapshots for one source cause a visible error and leave the document unchanged.
Ordinary prose in an unprocessable legacy note remains editable. The startup
sequence restores the library before processing the first note.

Legacy reference lists can contain independently selected items with no body
citation. Such unmatched entries are retained and displayed as “旧存档” using
their stored text and a bounded, controlled HTML subset. Unsupported stored
formatting falls back to readable text with a diagnostic. This is archival
display, not newly computed citation output; processing still reports when a
list needs migration. Opening a legacy note never deletes its unmatched items.

A section marked `bibliographyMode: "citations"` declares ordinary entries
derived from actual body citations. New explicit reading lists instead store
`bibliographyMode: "selected"` with complete source attributes in
`selectedItemsJson`. The C processor regenerates the union of selected and cited
sources, preserving unknown source attributes. Removing a body citation leaves
an explicitly selected source in the bibliography. An empty selected set removes
only the explicit selection, retaining body-cited sources. Unknown metadata or
conflicting snapshots that cannot be merged cause an atomic failure. Unmanaged
legacy entries are not silently converted into complete CSL sources.

`cslRenderJson` stores versioned controlled runs and bibliography rows, bound to
the complete source order, style/locale pins and a SHA-256 consistency digest.
The digest is not authentication: opening a document always regenerates output
from source data. Current prose is included in each save/export snapshot.
HTML interchange preserves those strings and root profile settings as escaped
attributes; it is editable interchange, not a promise of a print-ready HTML
publication. JSON preserves additional metadata that HTML cannot represent.

The [C processor documentation](csl/README.md) and
[document support boundaries](csl/DOCUMENT.md) describe the supported subset.
These fixed profiles and representative tests do not establish full CSL or
APA/IEEE/GB conformance. Unsupported active constructs produce explicit errors,
with no approximate formatter fallback. Default Unicode collation has no CLDR
language tailoring. Rich display currently reports unsupported small caps,
light/oblique styles and bidirectional citation text rather than dropping those
format flags. SQLite metadata interpretation limits below also remain.

The imported library is cached only in the application's own
`.notelab-library/library.json` beneath its notes directory. Restart restores
items, collections and CSL metadata. No external Zotero directory is scanned
automatically. A damaged cache remains untouched, with a visible warning;
failed saves retain the imported library in memory and offer a retry. Browser
success is reported only after the persistent-storage flush completes.

SQLite field mappings follow Zotero data schema version 45 at commit
`b86c79b56479cadac3288e1f122cf34ef04e8809`; `tools/generate_zotero_mapping.py`
regenerates the C lookup data from that schema. Raw fields/creator metadata are
retained in the generated CSL object's `custom.zotero` property. Advanced date
interpretation, name particles and CSL overrides in Zotero's Extra field still
need compatibility work.

Zotero's multipart date prefixes preserve year-only and year/month precision;
explicit ISO date intervals in the original date text retain both endpoints.
The importer keeps the original SQLite field alongside the structured date.

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
