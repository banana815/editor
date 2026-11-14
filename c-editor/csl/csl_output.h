#ifndef CE_CSL_OUTPUT_H
#define CE_CSL_OUTPUT_H

#include <stdbool.h>
#include <stddef.h>

/* Per citation / bibliography entry, not a limit on an entire document. The
 * payload budget includes copied text and links, excluding terminating NULs. */
#define CE_CSL_OUTPUT_MAX_BYTES ((size_t)1024 * 1024)
#define CE_CSL_OUTPUT_MAX_RUNS ((size_t)65536)
#define CE_CSL_OUTPUT_MAX_LINK_BYTES ((size_t)8192)
#define CE_CSL_OUTPUT_MAX_HTML_BYTES ((size_t)16 * 1024 * 1024)

enum CeCslOutputFormat {
    CE_CSL_BOLD = 1u << 0,
    CE_CSL_ITALIC = 1u << 1,
    CE_CSL_UNDERLINE = 1u << 2,
    CE_CSL_SMALL_CAPS = 1u << 3,
    CE_CSL_SUPERSCRIPT = 1u << 4,
    CE_CSL_SUBSCRIPT = 1u << 5,
    CE_CSL_OBLIQUE = 1u << 6,
    CE_CSL_LIGHT = 1u << 7
};

typedef struct CeCslOutputAllocator {
    void *context;
    void *(*allocate)(void *context, size_t bytes);
    void (*deallocate)(void *context, void *memory);
} CeCslOutputAllocator;

typedef struct CeCslOutputRun {
    const char *text; /* Owned NUL-terminated UTF-8; no embedded NUL. */
    size_t byte_length;
    unsigned format; /* Final effective formatting, not inherited overrides. */
    const char *href; /* NULL or an owned, validated absolute HTTP(S) link. */
} CeCslOutputRun;

typedef struct CeCslOutput CeCslOutput;

/* Initialize to {0}. Always release with ce_csl_output_string_free(), including
 * for the default allocator. It can outlive the originating output object, but
 * its allocator context must remain valid until the string is released. The
 * callback fields are private; callers should read only data and length. */
typedef struct CeCslOutputString {
    char *data;
    size_t length;
    void (*_deallocate)(void *context, void *memory);
    void *_context;
} CeCslOutputString;

/* NULL allocator selects malloc/free. Otherwise both callbacks are required;
 * allocations must satisfy the alignment and lifetime contract of malloc.
 * The callbacks are copied, their context is borrowed. No global state. */
CeCslOutput *ce_csl_output_new(const CeCslOutputAllocator *allocator,
                             char *error, size_t capacity);
void ce_csl_output_free(CeCslOutput *output);

/* Copies inputs and preserves bytes, run boundaries and all whitespace. Empty
 * text is a validated no-op. Invalid UTF-8, embedded NUL, C0 controls other than
 * TAB/LF/CR, unknown format bits and conflicting vertical/style/weight bits
 * are rejected (super/subscript, italic/oblique and bold/light).
 * Links allow ASCII DNS/IPv4-style hostnames, an optional numeric port and UTF-8
 * paths/queries. Credentials, IPv6 literals, whitespace/control characters,
 * backslashes and malformed/control-character percent escapes are rejected.
 * href may be NULL or empty for no link. All failure paths leave output intact. */
bool ce_csl_output_append(CeCslOutput *output, const char *text, size_t length,
                          unsigned format, const char *href,
                          char *error, size_t capacity);
/* Appends only the source's runs; existing destination text is not deep-copied.
 * Self-concatenation is supported. clone uses the source's allocator. */
bool ce_csl_output_concat(CeCslOutput *output, const CeCslOutput *source,
                          char *error, size_t capacity);
CeCslOutput *ce_csl_output_clone(const CeCslOutput *source,
                                char *error, size_t capacity);

size_t ce_csl_output_run_count(const CeCslOutput *output);
size_t ce_csl_output_text_bytes(const CeCslOutput *output);
/* Borrowed view, invalidated by a successful mutation or free. Out-of-range
 * indices return NULL. Renderers consume these runs directly, not parsed HTML. */
const CeCslOutputRun *ce_csl_output_run(const CeCslOutput *output, size_t index);

/* Atomic replacement of an initialized result; failure preserves its previous
 * data. Plain export concatenates exact text bytes. HTML uses only fixed tags,
 * fixed formatting attributes and escaped text/href values; a pre-wrap wrapper
 * preserves whitespace. No raw HTML is accepted. Small caps is a style, not a
 * change to the stored Unicode text. CR is emitted as &#13; to avoid HTML input
 * newline normalization. Neither export performs bidi or font shaping. */
bool ce_csl_output_plain(const CeCslOutput *output, CeCslOutputString *result,
                         char *error, size_t capacity);
bool ce_csl_output_html(const CeCslOutput *output, CeCslOutputString *result,
                        char *error, size_t capacity);
void ce_csl_output_string_free(CeCslOutputString *string);

#endif
