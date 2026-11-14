# Third-party software and font notices

This inventory covers the C editor, its bundled fonts, the Android Java bridge,
and the platform runtimes used by the current build. It does not assign a license
to NoteLab itself. Licenses apply to their respective components. Preserve the
files in [`licenses/`](licenses/) with redistributed builds; the license texts,
copyright notices, and upstream exceptions control over this inventory.

[`licenses/SOURCES.json`](licenses/SOURCES.json) records the source of each notice,
the SHA-256 of the inspected source file, and the SHA-256 of the packaged notice.
The sources were read from the dependency checkouts fetched by CMake and the
Emscripten 4.0.14 SDK. This is a cross-platform inventory: notices for conditional
SDL backends and runtime source files are retained even when a particular binary
does not use them. Test frameworks and unused CSL processors are not dependencies
of this application.

## Application dependencies

| Component actually built or bundled | Version / source revision | License and packaged notices |
| --- | --- | --- |
| cJSON, JSON parsing and serialization | [1.7.19](https://github.com/DaveGamble/cJSON/tree/v1.7.19) | MIT; Dave Gamble and cJSON contributors, 2009–2017. `licenses/cJSON.txt`. |
| SDL, rendering, input, windows and Android Java bridge | [`f00dc919ca35b9756316e86c4777392de442f9b2`](https://github.com/libsdl-org/SDL/tree/f00dc919ca35b9756316e86c4777392de442f9b2) | zlib; Sam Lantinga, 1997–2026, plus the embedded notices below. `licenses/SDL.txt`. The Java files are compiled from this same checkout, not a separate Android UI framework. |
| SDL_ttf, text layout and rendering | [3.2.2 / `a1ce3670aec736ecbf0936c43f2f0cc53aa61e5b`](https://github.com/libsdl-org/SDL_ttf/tree/a1ce3670aec736ecbf0936c43f2f0cc53aa61e5b) | zlib; Sam Lantinga, 1997–2025. `licenses/SDL_ttf.txt`. |
| SDL_ttf editbox example adapted in `vendor/editbox.c` and `.h` | SDL_ttf 3.2.2 | The permissive example-source notice is retained in both files and copied into `licenses/SDL_ttf-editbox.txt`. This example has its own short notice. Local adaptations are marked in the source and described in `vendor/README.md`. |
| FreeType, glyph rasterization | [2.13.2 / `9973564cfa63763a3e4ac67c09147899539b1e07`](https://github.com/freetype/freetype/tree/9973564cfa63763a3e4ac67c09147899539b1e07) | The **FreeType License (FTL)** is selected from the upstream FTL/GPL alternatives. `licenses/FreeType.txt`, `FreeType-license-choice.txt`, and `FreeType-additional.txt`. |
| HarfBuzz, text shaping | [8.5.0 / `564bf9818a18709776856533829c0c04950773d6`](https://github.com/harfbuzz/harfbuzz/tree/564bf9818a18709776856533829c0c04950773d6) | Old MIT, plus MIT contributions. `licenses/HarfBuzz.txt`, `HarfBuzz-Microsoft-USE.txt`, and `HarfBuzz-additional.txt`. Full upstream copyright-holder lists are preserved there. |
| SQLite, read-only Zotero database access | [3.54.1 amalgamation](https://sqlite.org/2026/sqlite-amalgamation-3540100.zip), Fossil check-in `555e07ef33f69a0957d24d8e555b702464a1` | Public-domain dedication copied from the actual amalgamation into `licenses/SQLite.txt`. Only the SQLite core is linked, not `shell.c`. |
| Noto Sans CJK SC Regular, unmodified default text font | [2.004, noto-cjk revision `523d033d6cb47f4a80c58a35753646f5c3608a78`](https://github.com/notofonts/noto-cjk/tree/523d033d6cb47f4a80c58a35753646f5c3608a78) | SIL OFL 1.1. The font's name table identifies © 2014–2021 Adobe (http://www.adobe.com/). `licenses/NotoSansCJK-OFL.txt` preserves that attribution together with the upstream license; `assets/OFL.txt` also remains with the font assets. |
| Noto Emoji, unmodified monochrome variable font | [3.002, Google Fonts revision `29b4ec6df2522a8aee279414b78a07910b065874`](https://github.com/google/fonts/tree/29b4ec6df2522a8aee279414b78a07910b065874/ofl/notoemoji) | SIL OFL 1.1; Copyright 2013 Google LLC. `licenses/NotoEmoji-OFL.txt` and `assets/OFL-Emoji.txt`. |

Portions of this software are copyright © 1996–2023 The FreeType Project
(https://www.freetype.org). All rights reserved. FreeType is developed by David
Turner, Robert Wilhelm, Werner Lemberg, and contributors. This acknowledgement
is part of the documentation distributed with the binary under the FTL.

FreeType includes separately licensed BDF/PCF drivers and hash code (Francesco
Zappa Nardelli and Computing Research Labs, New Mexico State University),
HarfBuzz-derived glue (Red Hat and Google), and **bundled zlib 1.3** (Jean-loup
Gailly and Mark Adler, 1995–2023). `FT_DISABLE_ZLIB=ON` disables the external zlib
dependency; the generated configuration still enables the internal gzip/zlib
implementation. Those notices are all included in `FreeType-additional.txt`.
The current configuration does not link external libpng, Brotli, bzip2,
PlutoSVG, ICU, Graphite2, GLib, or Cairo.

HarfBuzz's Microsoft Universal Shaping Engine data notice and Zilong Tan's
2012 MIT hash implementation notice are included separately. HarfBuzz's own
top-level notice contains its other copyright holders.

## Embedded SDL code

The SDL source contains contributions with their own notices in addition to
SDL's top-level zlib license:

- HIDAPI: the **BSD-style alternative** is selected from upstream's offered
  choices. `SDL-hidapi-BSD.txt` contains its terms, and
  `SDL-hidapi-license-choice.txt` preserves the choice statement. Additional
  source copyright notices, including libusb/hidapi contributors and Valve's
  Android/iOS backends, are in `SDL-additional.txt`.
- yuv2rgb: BSD 3-Clause, Adrien Descamps (2016), in `SDL-yuv2rgb.txt`.
- X11 keysym/EDID/XSettings code and Unix XDG directory lookup: their MIT-style
  notices are in `SDL-additional.txt`.
- Sun fdlibm, SDL_gfx rotation, Gareth McCaughan's qsort, controller contributions,
  Khronos/DirectX declarations, stb_image, miniz, public-domain helpers and debug
  font data: the original source notice comments are in `SDL-additional.txt`.
  For stb_image the MIT alternative is selected. Khronos declarations marked
  Apache-2.0 use the full text in `Gradle-Apache-2.0.txt`; MIT terms are also
  reproduced in the corresponding embedded source notices.
- Wayland protocol-generated code: the copyright and permission text of each
  bundled protocol is preserved in `SDL-Wayland-protocols.txt`.

This inventory retains the applicable source notices for the supported platform
backends. It does not imply that external system libraries dynamically used by
SDL are included in every package. When redistributing any such libraries,
include the notices for the actual copied binaries as well.

## Platform runtime and build tools

**Browser.** The current build uses Emscripten **4.0.14**. Its generated runtime
and path helper notices (including the Node.js-derived path code) are in
`licenses/Emscripten.txt`; the MIT alternative is selected for Emscripten's own
code. The bundled musl copyright notice is in `Emscripten-musl.txt`, and embedded
math/BSD/MIT notices and the dlmalloc public-domain notice are in
`Emscripten-runtime-additional.txt`. The SDK's exact libc++, libc++abi, libunwind,
compiler-rt, and LLVM libc license files are retained as `LLVM-*.txt`, including
Apache-2.0 WITH LLVM-exception and the legacy terms present upstream. Runtime
members linked from these libraries depend on the compiler options; retaining
their notices does not mean that the complete SDK or development tools are
distributed. Recheck these files when upgrading Emscripten.

**Android.** The APK includes SDL3, the application and static text/database
dependencies, and **`libc++_shared.so`** from NDK **28.2.13676358 (r28c)** for the
configured `arm64-v8a` and `x86_64` ABIs. The package must also include the actual
NDK's root `NOTICE` and `NOTICE.toolchain`, copied at build time under
`licenses/android-ndk/`. These aggregate upstream notices are taken from the
installed SDK used to build the APK, rather than guessed from a different LLVM
release. Android system Java APIs and system shared libraries are supplied by
the OS. The Android Gradle Plugin 8.7.3 and Gradle 8.12 are build tools, not APK
libraries. This source tree includes the Gradle wrapper; its Apache 2.0 text and
script copyright notice are preserved in `Gradle-Apache-2.0.txt` and
`Gradle-wrapper-notice.txt` and in the original Android project files.

**Desktop and iOS.** Apple frameworks and OS-provided C/C++ libraries are linked
from the SDK/system and are not copied into the application by this source tree.
Any Windows redistributable runtime or Linux shared libraries actually included
in a release need the notices/redistribution terms of those exact binaries.
Development builds may use an installed SDL; that installation's version and
notices must replace the pinned-SDL assumption if its binary is redistributed.
Changing the compiler, SDK, build options or bundled dependencies requires
checking the resulting package again.

## Zotero schema mapping provenance

`src/zotero_mapping.h` contains interface field/type/creator relationships
generated by `tools/generate_zotero_mapping.py` from the `csl` part of
[Zotero schema version 45 at revision
`b86c79b56479cadac3288e1f122cf34ef04e8809`](https://github.com/zotero/zotero-schema/blob/b86c79b56479cadac3288e1f122cf34ef04e8809/schema.json).
No Zotero executable, CSL rendering engine, styles, or locale bundle is linked.

The [pinned schema repository README](https://github.com/zotero/zotero-schema/blob/b86c79b56479cadac3288e1f122cf34ef04e8809/README.md)
describes retrieving and caching the schema from applications, and the
[official API documentation](https://www.zotero.org/support/dev/web_api/v3/types_and_fields)
describes its use by API clients. Neither provides a separate explicit
redistribution license for this generated mapping. The pinned repository tree
contains no license file. The [general Zotero licensing page](https://www.zotero.org/support/licensing)
states an AGPLv3 default for Zotero project source code, but does not specifically
resolve the licensing of these interface-data relationships. This inventory
records the source and attribution and **does not label the generated mapping
MIT, public domain, or conclusively AGPL**. The mapping's redistribution basis
remains to be established or the source of these relationships replaced before
treating the licensing inventory as complete.

## Distribution contents and remaining checks

Ship this document and the entire `licenses/` directory in each desktop archive,
iOS app resource set, Android assets and browser distribution. Preserve the two
font license files alongside the fonts. Browser releases should also offer the
human-readable notices as ordinary downloadable files, so users need not inspect
the WebAssembly virtual filesystem. Platform packaging may add notices such as
Android's `licenses/android-ndk/NOTICE*`; those generated files are additional to
the checked-in `SOURCES.json` list.

Before release, inspect the actual archive/APK/app/browser output for those
files, verify that all copied shared libraries are covered, and resolve the
mapping provenance item above. A successful compile does not establish these
distribution contents. If `EDITOR_FONT_PATH` replaces the default Noto font,
include that font's actual license/attribution and update the inventory; the
Noto license does not cover an arbitrary replacement font. These notices do not
choose the license for NoteLab's original source or grant use of third-party
trademarks.
