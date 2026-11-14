# Streaming bibliography layout adapter

`csl_bibliography` turns one finalized bibliography entry into independently
owned logical rows and styled cells. `ce_csl_bibliography_entry_build` copies
content, layout options and numeric source ranges; it retains no style,
evaluation or finalization pointers. A document processor can build an entry,
free that entry's evaluation/finalization immediately, and keep only the owned
entry array. It preserves empty entries and their source item indices while
exporters omit their text and spacing.

The adapter does not sort, select eligible items, substitute repeated authors,
shape fonts or choose physical wrap positions. Its source ranges refer to the
original finalized UTF-8 text; numeric evaluation node indices survive source
release but are not XML pointers. A combined body uses `SIZE_MAX` instead of
inventing one source node. Other source spans preserve node/person indices,
display roles and bounding ranges. These ranges are metadata, not instructions
to cut or replace names.

The [CSL whitespace rules](https://docs.citationstyles.org/en/stable/specification.html#whitespace)
define hanging indentation, first-field alignment and integer line/entry spacing.
The [display rules](https://docs.citationstyles.org/en/stable/specification.html#display)
define full-width blocks, indented blocks and paired left/right fields. The
adapter represents these as FLOW, BLOCK, INDENT and TWO_COLUMNS rows. An isolated
left or right block becomes a full-width block. All left/right pairs in an HTML
bibliography share a column width.

First-field splitting uses semantic nodes and finalized spans, never a search
for the citation number. A chosen `choose` branch is transparent; groups and
macro calls remain whole fields. An empty first rendering element leaves an
empty first field instead of promoting the next nonempty element. This behavior
was checked through the public citeproc API with original synthetic styles;
processor implementation source was not read or copied.

The initial profile supports the pinned APA, IEEE and GB/T styles. APA preserves
hanging indentation, line-spacing 2 and entry-spacing 0. IEEE and GB/T preserve
flush first-field alignment and entry-spacing 0, including the GB/T layout's
terminal punctuation. It also supports multiple disjoint display blocks,
indented annotations and adjacent left/right pairs. It explicitly rejects:

- Nested display, or explicit display combined with second-field alignment or
  hanging indentation.
- Nonempty text outside display control, gaps between display blocks, and
  decorated anonymous ancestors that make block ownership ambiguous.
- Overlapping field ranges caused by punctuation moving across a column boundary.
- Simultaneous hanging indentation and second-field alignment.

These errors retain the old entry. They do not drop bytes, reverse-parse output
or guess which field owns a moved punctuation mark.

`ce_csl_bibliography_plain` borrows an ordered entry array. TAB is the normal
column separator; COMPACT uses one ASCII space for compatibility with textual
reference examples. Cell whitespace remains unchanged. Logical rows use LF and
entries use `1 + entry-spacing` LFs, without a terminal LF. Plain output retains
line-spacing/hanging behavior in the structured entry options rather than
inventing soft wraps, padding widths or physical blank lines within a paragraph.

HTML uses a fixed presentation table and escaped controlled text/style/link
runs. Flush mode can use browser-computed shared columns. Margin mode requires
a caller-measured label width; labels occupy the margin and the body starts at
the containing margin. Metrics use integer thousandths of an em, avoiding
locale-sensitive decimal formatting. Default indentation is 2 em, column gap
0.5 em and base line height 1.2 em; the style's line-spacing multiplies that base.
These are configurable presentation metrics, not measurements inferred from
Unicode character counts. Superscript/subscript, weight, slant, underline,
small caps and validated links remain structured until serialization.

Each entry has a 1 MiB text/link payload bound and a shared 128 MiB construction
workspace, including child allocations. Array exports are bounded by 50,000
entries, 131,072 rows, 1,048,576 cumulative evaluation nodes and 16 MiB of
source text. Plain output is at most 16 MiB; escaped HTML at most 64 MiB. Export
workspace is at most 128 MiB; retained input entries and the old caller-owned
output are excluded. Large spacing or escaped output can hit an export limit
even when source text fits. Repeated entry pointers charge aggregate limits
again. No exporter takes ownership of the array or its entries.

Custom allocator contexts must outlive their owned entries and exported
aggregate strings. Strings exported directly from a borrowed cell's output
must be freed before the entry, because that view uses the entry's allocator.
Entry replacement and aggregate string replacement are atomic on every error.

Validation includes strict native C11 with ASan/UBSan and WebAssembly: source
release before export, exact Unicode/whitespace, first-field empty/group/macro/
choose behavior, display rows, unsupported overlaps, metrics, payload limits and
all reached allocation-failure points. The five-argument resource mode reads
three pinned styles followed by en-US and zh-CN locales and checks actual
APA/IEEE/GB/T entries. The reviewed snapshot covers 25 entry-build allocation
failures and each plain/HTML growth failure. A separate isolated Chrome check
verified equal body x-coordinates for labels 9/10, margin-body alignment,
32 px hanging indentation and double baseline distance at a 16 px font, with
Chinese/emoji and literal script-like text; the latter executed no script.
These checks establish this adapter profile, not full CSL conformance.
