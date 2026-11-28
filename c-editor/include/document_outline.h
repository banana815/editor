#ifndef CE_DOCUMENT_OUTLINE_H
#define CE_DOCUMENT_OUTLINE_H
#include "document.h"

typedef struct {
    size_t block_index, start, end, section_end, parent;
    unsigned level;
    uint64_t key;
    bool children;
} CeOutlineEntry;
typedef struct { CeOutlineEntry *entries; size_t count; } CeOutline;

/* Derived, owned heading index. Failure preserves out; initialize it to zero.
 * parent is SIZE_MAX for a root. section_end includes all descendant content.
 * Keys identify heading JSON for presentation state, not persistent document IDs. */
bool ce_outline_build(const CeDocument *doc, CeOutline *out);
void ce_outline_free(CeOutline *outline);
/* Move a heading and the following sibling subtrees up to the next heading of
 * equal/lower level. A heading leading a list item moves its complete item and
 * following section items. Different containers and drops into one's own
 * section fail explicitly. No levels or metadata are rewritten. One undoable
 * transaction; errors preserve document, history and new_offset. */
bool ce_outline_move(CeDocument *doc, size_t from_block, size_t to_block,
                     bool after, size_t *new_offset, char *error, size_t capacity);
#endif
