# Agent Guide for Note Lab

Note Lab is a C11 rich-text editor for Windows, macOS, Linux, Android, iOS and WebAssembly. The active project lives in `c-editor/`. Read `c-editor/PROJECT_STATUS.md` for verified behavior, current work and remaining acceptance criteria.

## Build and test

Run from the repository root with CMake 3.24+, a C/C++ toolchain and Python 3.8+:

```sh
cmake -S c-editor -B c-editor/build -DCMAKE_BUILD_TYPE=Debug
cmake --build c-editor/build --config Debug --parallel 4
ctest --test-dir c-editor/build -C Debug --output-on-failure
```

Launch `./c-editor/build/notelab` on macOS/Linux or the executable in the selected configuration directory on Windows. Use temporary notes or `--notes /absolute/path/to/test-notes` for manual validation.

For C core memory checks:

```sh
cmake -S c-editor -B c-editor/build-core -DEDITOR_BUILD_APP=OFF -DEDITOR_SANITIZERS=ON -DCMAKE_BUILD_TYPE=Debug
cmake --build c-editor/build-core --parallel 4
ctest --test-dir c-editor/build-core --output-on-failure
```

Platform prerequisites and commands are documented in `c-editor/README.md` and `c-editor/platform/{android,ios,web}/README.md`. Linux needs system libcurl 7.84+ and window-system development packages. Dependencies, fonts and CSL styles are pinned by the CMake modules; preserve their versions and checksums unless the task calls for an update.

## Code map

| Path | Responsibility |
| --- | --- |
| `c-editor/src/app.c`, `app_*.inc` | Application state, UI, input and controllers |
| `c-editor/src/document.c`, `document_slice.c` | Rich documents, undo and clipboard slices |
| `c-editor/src/note_store.c` | Note persistence and file operations |
| `c-editor/src/zotero.c`, `library_panel.c` | Library parsing, filtering and selection |
| `c-editor/src/citation_*.c`, `bibliography_*.c` | Citation integration and bibliography state |
| `c-editor/csl/` | C citation processor, pinned profiles and fixtures |
| `c-editor/src/platform_*`, `c-editor/platform/` | OS access, mobile glue and browser bridge |
| `c-editor/include/` | Public C interfaces |
| `c-editor/tests/` | Native, browser, HTTP and packaged-startup tests |
| `c-editor/cmake/`, `c-editor/tools/` | Dependencies, packaging and generated data |

## Implementation contracts

- Keep application decisions, document/storage behavior, library parsing and citation processing in C. Objective-C, Java/JNI and browser JavaScript connect platform capabilities. Third-party dependencies may use other languages; keep their notices and attribution intact.
- Prepare and validate document/library changes before committing them. Failed allocation, parsing, persistence or download operations must preserve the existing note and library.
- Preserve unknown legacy metadata where representable. Keep legacy HTML/JSON imports and their regression fixtures even though the previous application has been retired.
- Regenerate citation display from authoritative source snapshots; imported render caches are not trusted output. Do not substitute approximate formatting for unsupported active CSL constructs.
- Network access requires an explicit source action. Preserve TLS, cancellation, size/deadline limits and the no-automatic-refresh contract in `c-editor/platform/LIBRARY_SOURCE.md`.
- Tests must use isolated notes, fixture servers and browser profiles. Do not read the user's actual notes, Zotero profile, credentials or clipboard for automated validation.
- Browser tests must wait for navigation, rendered state and persistence completion. Keep the slow-CPU library regression and its failure diagnostics enabled.

## Repository and validation

The two workflows are `.github/workflows/c-editor.yml` (application builds, tests, packaging and startup) and `.github/workflows/csl.yml` (citation processor checks). Run the checks relevant to a change and distinguish local results from actual platform CI. A successful build does not establish complete feature parity or physical-device behavior.

Keep generated builds and toolchains in ignored directories. Update the relevant README, platform contract or project status when behavior or build entry points change. `docs/design/` holds reference artwork; `c-editor/licenses/` and `THIRD_PARTY.md` record redistributable dependencies and unresolved licensing work.
