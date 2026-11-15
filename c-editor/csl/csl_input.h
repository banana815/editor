#ifndef CE_CSL_INPUT_H
#define CE_CSL_INPUT_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <cJSON.h>

#define CE_CSL_INPUT_MAX_BYTES ((size_t)16 * 1024 * 1024)
#define CE_CSL_INPUT_MAX_NODES ((size_t)250000)
#define CE_CSL_INPUT_MAX_ITEMS ((size_t)50000)
#define CE_CSL_INPUT_MAX_CLUSTERS ((size_t)50000)
#define CE_CSL_INPUT_MAX_CITES ((size_t)100000)

typedef struct CeCslInput CeCslInput;
typedef struct CeCslInputItem {
    const char *id; /* Opaque UTF-8 identity; safe integer IDs become decimal. */
    const cJSON *data; /* Original typed CSL data, including unknown fields. */
    size_t first_citation; /* SIZE_MAX if uncited; not a citation number. */
    size_t first_position; /* Position within the first referring cluster. */
} CeCslInputItem;
typedef struct CeCslInputCite {
    size_t item_index;
    const cJSON *data; /* Locator, affixes, flags and unknown fields retained. */
} CeCslInputCite;
typedef struct CeCslInputCluster {
    const char *id;
    const cJSON *data;
    const CeCslInputCite *items;
    size_t item_count;
    uint32_t note_index; /* Zero when omitted; never inferred from a quote. */
} CeCslInputCluster;

/* Input envelope: {"items":[CSL objects], "citations":[{"citationID":"...",
 * "citationItems":[{"id":"...","locator":"...","label":"page"}],
 * "properties":{"noteIndex":1}}]}. Both arrays may be empty. Each cluster
 * requires at least one item. Item IDs are nonempty strings or exact integers
 * in [-9007199254740991,9007199254740991]; exponent/fraction notation is accepted
 * only when mathematically integral, before any floating-point rounding.
 * Numeric 1 and string "1" refer to the same identity. String IDs are not
 * casefolded, normalized, trimmed or interpreted as URIs. Cluster IDs are strings.
 *
 * Owns its JSON and indexes. Success atomically replaces *input; failure retains
 * it and every borrowed view. Initialize *input to NULL. Strict JSON/UTF-8,
 * NULs, lone surrogates, duplicate keys/identities, unresolved references and
 * contradictory author flags are checked. Maximum nesting is 128 containers.
 * Unknown fields retain their JSON types; ordinary JSON numbers use cJSON's
 * finite double representation (this is not a lossless source serializer).
 * This validates input identities/cluster structure, not the full CSL schema,
 * formatting rules, bibliographic order, disambiguation or style support.
 * No global allocator changes, file access or network calls are performed. */
bool ce_csl_input_load(CeCslInput **input, const char *json, size_t size,
                       char *error, size_t capacity);
void ce_csl_input_free(CeCslInput *input);
size_t ce_csl_input_item_count(const CeCslInput *input);
size_t ce_csl_input_cluster_count(const CeCslInput *input);
const CeCslInputItem *ce_csl_input_item(const CeCslInput *input, size_t index);
const CeCslInputCluster *ce_csl_input_cluster(const CeCslInput *input, size_t index);
/* SIZE_MAX means not found. Queries use the canonical string identity. */
size_t ce_csl_input_find(const CeCslInput *input, const char *id);
const cJSON *ce_csl_input_data(const CeCslInput *input);
#endif
