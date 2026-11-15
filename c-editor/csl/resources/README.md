# Pinned CSL styles and locales

These resources come from the [Citation Style Language (CSL) project](https://citationstyles.org/), with the authors, contributors and translators credited in each XML file. Their original bytes and complete metadata are preserved. No processing runtime or processor-test fixture is included in this directory.

| Resource | Pinned profile | Upstream metadata `updated` |
| --- | --- | --- |
| `apa.csl` | APA Style 7th edition, including the upstream profile's Bluebook handling | `2026-02-07T00:00:00+00:00` |
| `ieee.csl` | IEEE Reference Guide version 11.29.2023 | `2024-03-27T11:41:27+00:00` |
| `china-national-standard-gb-t-7714-2015-numeric.csl` | China National Standard GB/T 7714-2015, numeric, Chinese | `2024-01-22T22:07:03+08:00` |
| `locales-en-US.xml` | English, United States | `2026-01-10T00:00:00+00:00` |
| `locales-zh-CN.xml` | Chinese, China | `2025-10-16T11:24:00+08:00` |

All three styles use CSL `version="1.0"`. That XML attribute identifies the CSL schema family; the profile titles and fixed commits identify the bibliographic style versions. The GB/T file specifically targets the **2015 numeric Chinese profile**. Other editions and author-date profiles require their own resources and validation. These files alone do not establish complete APA, IEEE, GB/T or CSL conformance of the C processor. The independent modules and their tests currently cover a subset of a complete citation engine.

The styles are pinned to [`citation-style-language/styles` commit `3c3995baaa0c4fe1e5c6328027ff6d985c07bfde`](https://github.com/citation-style-language/styles/tree/3c3995baaa0c4fe1e5c6328027ff6d985c07bfde). The locales are pinned to [`citation-style-language/locales` commit `14940e56f7c783e4173cafbd92e0f5856f2caa14`](https://github.com/citation-style-language/locales/tree/14940e56f7c783e4173cafbd92e0f5856f2caa14). [SOURCES.json](SOURCES.json) records each original URL, commit, Git blob ID, SHA-256, byte length and attribution. The GB/T file retains its original upstream filename.

## Redistribution and attribution

The five style/locale data files are licensed under **Creative Commons Attribution-ShareAlike 3.0 Unported (CC BY-SA 3.0)**. Their embedded `rights` metadata and the pinned [styles licensing statement](https://github.com/citation-style-language/styles/blob/3c3995baaa0c4fe1e5c6328027ff6d985c07bfde/README.md#licensing) and [locales licensing statement](https://github.com/citation-style-language/locales/blob/14940e56f7c783e4173cafbd92e0f5856f2caa14/README.md#licensing) identify that license and require attribution to the CSL project and preservation of contributor metadata.

[LICENSE.txt](LICENSE.txt) contains the complete license text downloaded from [Creative Commons' official legal-code endpoint](https://creativecommons.org/licenses/by-sa/3.0/legalcode.txt), with its exact hash in the manifest. Keep that file, this attribution notice, the source manifest and the original metadata together when redistributing the resources. Credit the **Citation Style Language (CSL) project — https://citationstyles.org/** in the software's third-party notices when these resources are included. Future modifications must be identified and must follow the license's applicable attribution and ShareAlike conditions.

This resource notice applies to the five data files. It does not grant a license to, or change the licensing of, the application's source code or the independent C processor. The upstream XML metadata remains the authoritative individual author/contributor/translator listing. There are no resource modifications in this snapshot.

`../.gitattributes` disables line-ending conversion for `resources/**`, preserving the manifest hashes on Windows as well as other hosts. To verify bytes, use SHA-256 and byte lengths from `SOURCES.json`; do not reserialize, reindent, rewrite XML declarations or normalize line endings before comparing.

## Existing test arguments

The following paths are appended to the existing executable command. Native CTest can pass absolute paths under `${CMAKE_CURRENT_SOURCE_DIR}/resources`; no environment, network access or user library is needed.

| Test executable | Optional arguments, in order |
| --- | --- |
| `test_csl_xml` | All three `.csl` files and both locale `.xml` files, in any order |
| `test_csl_style` | `apa.csl`, `ieee.csl`, `china-national-standard-gb-t-7714-2015-numeric.csl` |
| `test_csl_locale` | All three styles and both locales, in any order; the test builds one locale catalog and resolves each style |
| `test_csl_number` | `locales-en-US.xml`, `locales-zh-CN.xml`; each is tested independently |
| `test_csl_date` | **First** `locales-en-US.xml`, **second** `locales-zh-CN.xml` |

For example, the date registration can use:

```cmake
add_test(NAME csl_date COMMAND test_csl_date
  "${CMAKE_CURRENT_SOURCE_DIR}/resources/locales-en-US.xml"
  "${CMAKE_CURRENT_SOURCE_DIR}/resources/locales-zh-CN.xml")
```

Update the existing registrations rather than adding duplicate test names. For the shared test loop, choose the argument list per module before `add_test`. Each executable still runs its own unit tests before loading the supplied resources. The date executable has a third optional argument for an external research fixture bundle; that bundle is not shipped here and should not be passed by the repository's ordinary CTest registration.

For Emscripten tests, either:

1. Embed this directory into each relevant test target at a fixed virtual path, for example `--embed-file <absolute-resources-directory>@/csl-resources`, and pass `/csl-resources/<filename>` as the test arguments. Run the resulting JavaScript launcher with Node through CMake's cross-compiling emulator. This keeps test file access inside Emscripten's virtual filesystem.
2. Enable `-sNODERAWFS=1` **only on the Node test targets** and pass native absolute resource paths. This is a test harness option, not a browser/application setting.

Resource embedding and resource test registrations belong inside `if(BUILD_TESTING)`. Android/iOS compile-only jobs configured with `BUILD_TESTING=OFF` need no fixture filesystem or embedding. Application packaging of these five resources is a separate integration step and must also carry the attribution/license files above.
