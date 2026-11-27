#ifndef CE_CSL_JSON_H
#define CE_CSL_JSON_H

#include "csl_input.h"

/* Generic strict JSON using exactly the input module's lexical/tree checks.
 * No CSL envelope/identity/reference schema is imposed; any JSON root is valid.
 * Rejects duplicate decoded object keys, invalid UTF-8, raw/escaped NUL, lone
 * surrogates, malformed JSON numbers and nonfinite cJSON numeric results.
 * Limits: CE_CSL_INPUT_MAX_BYTES (16 MiB), CE_CSL_INPUT_MAX_NODES (250,000
 * value nodes; member names are strings, not extra nodes), and 128 nested
 * containers. The input-byte limit also bounds each decoded string and the
 * sum of decoded member-name/value string bytes: JSON escapes do not expand.
 * Ordinary numbers retain finite cJSON double semantics, not exact integers.
 *
 * size excludes a terminator, which is not required. Input is borrowed only
 * for this call. Initialize *out to NULL/live; success owns a new cJSON tree
 * and deletes the old one, failure preserves it. Release with cJSON_Delete.
 * Does not change global allocator hooks. As with other cJSON trees, caller
 * hooks must remain compatible until all returned trees have been released. */
bool ce_csl_json_parse_strict(const char *text, size_t size, cJSON **out,
    char *error, size_t capacity);
/* Explicit byte budget for application-owned recovery envelopes. max_bytes
 * must be 1..128 MiB. All UTF-8, duplicate-key, depth, node-count and ownership
 * checks above still apply; the existing APIs retain their 16 MiB limit. */
bool ce_csl_json_parse_strict_with_limit(const char *text, size_t size,
    size_t max_bytes, cJSON **out, char *error, size_t capacity);

/* Same parsing/ownership contract, with an optional numeric fidelity report.
 * On success *all_numbers_exact is true iff every ORIGINAL numeric token is
 * mathematically an integer in [-9007199254740991,9007199254740991]. Decimal
 * and exponent spellings of such integers are accepted; other finite numbers
 * still parse but report false. This uses the original tokens before double
 * rounding, so 1.0000000000000001 and 1e-9999 do not masquerade as 1 and 0.
 * No numbers reports true. On failure both output arguments remain unchanged.
 * It is a conservative safe-integer report, not a lossless decimal model. */
bool ce_csl_json_parse_strict_with_numbers(const char *text, size_t size, cJSON **out,
    bool *all_numbers_exact, char *error, size_t capacity);

/* Compact JSON without cJSON's epsilon-based numeric printing. Every finite
 * double round-trips exactly; safe integers are written as exact decimals.
 * This cannot recover original decimal precision already lost on parsing.
 * Borrows a stable tree and never changes it. Returns a cJSON_malloc-owned
 * string (release with cJSON_free), or NULL on invalid UTF-8/type, nonfinite
 * numbers, excessive depth/size, or allocation failure. Raw nodes are rejected.
 * Default output limit is 16 MiB; the explicit limit supports larger document
 * exports. Both passes use CJSON_NESTING_LIMIT and at most max_bytes nodes.
 * The output limit excludes the NUL terminator. No global hooks are changed. */
char *ce_csl_json_print_exact(const cJSON *value);
char *ce_csl_json_print_exact_with_limit(const cJSON *value, size_t max_bytes);

#endif
