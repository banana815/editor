# Bounded C collation keys

`ce_csl_collation_key` produces owned binary keys for `ce_csl_sort_order`.
The implementation and all runtime dependencies are C. It uses the Unicode
18.0.0 Default Unicode Collation Element Table (DUCET), secondary strength,
and non-ignorable punctuation. Full Unicode case folding precedes linear NFD
normalization and key generation. Thus `A`/`a`, `É`/`é`, and `SS`/`ß` compare
equal, while accents still affect ordering (`cafe < café < caff`).

This is **not CLDR locale collation**. It does not implement Chinese pinyin,
Swedish ordering, phonebook rules, or the host's locale. The language enum
selects only the core case-folding rules: default, Turkish (`tr`), or
Azerbaijani (`az`). All other languages use the default option. Only compare
keys generated with the same profile and language. Keys are temporary derived
data; regenerate them when the profile, Unicode data, or pinned library changes.

Keys are byte arrays, not NUL-terminated text. Empty input and nonempty
all-ignorable input can both produce a zero-length key. Neither means the
variable is missing: the evaluator supplies `CeCslSortKeyView.missing` separately.
Numbers, dates, and text also require distinct compatible key encodings; this
module does not choose a cross-type ordering or strip rich-text markup.

## Limits and failure behavior

Each input, folded buffer, and NFD buffer is limited to 64 KiB. Before either
transform, the input is decoded and canonically decomposed without allocation
to reject sequences exceeding 64 consecutive nonstarters. This counts expanded
characters, not merely marks in the source. Unicode transform workspace is
limited to 1 MiB. The final key is limited to 512 KiB.

Malformed UTF-8, embedded NUL, unsupported C0 controls, invalid options, excess
size/complexity, and allocation failure return an error. TAB, LF, and CR are
allowed. No text is silently truncated, repaired, or assigned a different
fallback ordering. A failure preserves the existing output key, including when
the input aliases it. A success replaces it atomically. Release keys with
`ce_csl_sort_key_free` and keep a supplied allocator's context alive until then.

The caller allocator covers the two Unicode transformations and the final key
copy. The adapter never configures Mojibake's process-wide allocator. Mojibake
uses its existing allocator for its temporary arrays and intermediate key. The
workspace and final-key limits therefore **do not impose a hard cap on those
internal allocations**; the final-key limit is checked after generation. A
whole sort additionally needs aggregate input/key budgets, cancellation or work
scheduling, and the row/column limits of the sort module. These per-key bounds
are not a device-independent latency guarantee.

## Why both NFD and a sequence limit are required

The pinned Mojibake normalizer flushes after 32 scalars. On longer sequences,
that can reorder marks only within individual blocks. For example, `á`
followed by 31 U+032F marks produces a different unprotected key from its
correct NFD form. Merely preparing NFC does not avoid this defect. Our full
linear NFD prepass makes Mojibake take its already-normalized quick-check path.
The regression test checks the resulting bytes against DUCET 18 weights.

Mojibake's discontiguous-contraction search can also take superlinear time on
long nonstarter runs, even on NFD input. The preflight sequence limit prevents
that unbounded workload. An isolated Release measurement on the development
machine processed a roughly 64 KiB adversarial string with runs of 64 marks in
about 31 ms natively and 44 ms in WebAssembly. Raising that limit to 256 took
about 340 ms and 527 ms respectively. These measurements justify the chosen
limit; they are not performance promises for other devices.

## Dependency and build integration

Mojibake 0.7.0 is pinned to commit
`e397201f7f342e20ec0bc7cb45efee0a7cef34ec`. The archive SHA-256 is
`7a90d0811c7cec45b514ad0085a6538d0a4526b6753dfd7ecc5cb5992be7ea34`.
[data/mojibake-sources.json](data/mojibake-sources.json) records the archive,
the 12 C translation units and their hashes, build definitions, and official
Unicode data/test sources. All 12 source Git blobs were checked against the
pinned GitHub tree. The generated Unicode tables and other headers are consumed
from the same verified archive.

Build the listed C files as a static C11 target with its `src` directory on the
include path. Set `MJB_FEATURE_COLLATION=1` and set
`MJB_FEATURE_CHARACTER_NAMES`, `MJB_FEATURE_IDNA`, and `MJB_FEATURE_SECURITY`
to zero. Use `/utf-8` under MSVC and the same CRT selection as the parent target.
Do not invoke the upstream WebAssembly demo target. The excluded `next.c`
implements an unused public for-each callback API; under Emscripten it routes
callbacks through JavaScript. The collation dependency closure does not call it.
Strict C11 compilation and actual WebAssembly execution of the 12-file subset
were verified, including all collation processing.

The code license is in [licenses/Mojibake.txt](licenses/Mojibake.txt). Unicode
data uses [data/UNICODE-LICENSE.txt](data/UNICODE-LICENSE.txt). Keep both notices
and the source manifest with distributions containing this library. Adopting
the collation module does not change the editor's overall license.

## Verification

`tests/test_csl_collation.c` covers full case folding, Turkish/Azerbaijani
choices, accent ordering, canonical equivalence across the upstream 32-scalar
boundary, contraction/expansion examples, empty versus missing keys, input
aliasing, exact size/sequence boundaries, fold and NFD expansion, final-key
overflow, invalid input, and every caller-allocator failure point. It passes
native ASan/UBSan and actual WebAssembly execution.

The independent pinned-library evaluation additionally ran the official Unicode
18.0.0 `NON_IGNORABLE` and `SHIFTED` conformance files with raw public keys:
212,035 adjacent comparisons per file passed both natively and in WebAssembly.
Each file's 30 surrogate-containing rows were excluded because the public API
accepts only well-formed Unicode. Embedded NUL was preserved in this library
test; the adapter deliberately rejects it. These tests did not filter SHIFTED
weights. Separate library allocator sweeps covered 25 failure points. They do
not establish a temporary-memory cap or replace the long-sequence regression.

References: [CSL sorting](https://docs.citationstyles.org/en/stable/specification.html#sorting),
[Unicode collation](https://www.unicode.org/reports/tr10/),
[pinned Mojibake source](https://github.com/zaerl/mojibake/tree/e397201f7f342e20ec0bc7cb45efee0a7cef34ec).
