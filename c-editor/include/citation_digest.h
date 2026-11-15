#ifndef CE_CITATION_DIGEST_H
#define CE_CITATION_DIGEST_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define CE_CITATION_DIGEST_BYTES ((size_t)32)
#define CE_CITATION_DIGEST_HEX_CAPACITY ((size_t)65)
#define CE_CITATION_DIGEST_MAX_BYTES (UINT64_MAX / UINT64_C(8))

/* Stack storage; fields are private. Initialize before use. No allocation,
 * process-global state, platform crypto dependency or host byte order is used. */
typedef struct CeCitationDigest {
    uint32_t _state[8], _initialized;
    uint64_t _bytes;
    size_t _used;
    unsigned char _block[64];
} CeCitationDigest;

void ce_citation_digest_init(CeCitationDigest *digest);
/* Raw byte stream, including binary data. NULL data is valid only for size 0.
 * Rejects total length > MAX_BYTES before reading the supplied bytes, keeping
 * the SHA-256 message length strictly below 2^64 bits. Failure preserves state. */
bool ce_citation_digest_update(CeCitationDigest *digest, const void *data, size_t size,
    char *error, size_t capacity);
/* Append one unambiguous field: 8-byte unsigned big-endian byte length, then
 * exact well-formed UTF-8 bytes. Empty fields are present; NUL is rejected.
 * No normalization, case folding or JSON serialization. Pass explicit domain
 * and version fields first, then a schema-defined order/count of fields.
 * This function handles one whole field; update handles arbitrary raw chunks.
 * Validation/overflow errors preserve state, including the length prefix. */
bool ce_citation_digest_field(CeCitationDigest *digest, const char *utf8, size_t size,
    char *error, size_t capacity);
/* Snapshot finalization: preserves state, permits repeated finalization and
 * later updates. out needs 32 bytes or 65 chars (lowercase hex + NUL).
 * Failure preserves output. A digest is a consistency key, not authentication. */
bool ce_citation_digest_final(const CeCitationDigest *digest, unsigned char out[32],
    char *error, size_t capacity);
bool ce_citation_digest_final_hex(const CeCitationDigest *digest, char out[65],
    char *error, size_t capacity);

#endif
