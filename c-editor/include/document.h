#ifndef CE_DOCUMENT_H
#define CE_DOCUMENT_H
#include <stdbool.h>
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif

typedef struct CeDocument CeDocument;
typedef enum { CE_FORMAT_AUTO, CE_FORMAT_TEXT, CE_FORMAT_HTML, CE_FORMAT_JSON } CeFormat;
typedef enum {
    CE_BLOCK_PARAGRAPH,
    CE_BLOCK_HEADING,
    CE_BLOCK_BULLET,
    CE_BLOCK_ORDERED,
    CE_BLOCK_CITATION,
    CE_BLOCK_ENDNOTES,
    CE_BLOCK_UNKNOWN
} CeBlockKind;
typedef enum {
    CE_MARK_BOLD = 1,
    CE_MARK_ITALIC = 2,
    CE_MARK_UNDERLINE = 4,
    CE_MARK_HIGHLIGHT = 8,
    CE_MARK_COLOR = 16,
    CE_MARK_LINK = 32
} CeMark;
typedef struct {
    unsigned marks;
    const char *color;
    const char *highlight;
    const char *link;
} CeStyle;
typedef struct {
    size_t start, end;
    CeBlockKind kind;
    unsigned level, depth;
    const char *alignment;
} CeBlockInfo;
typedef struct {
    size_t start, end;
    CeStyle style;
} CeStyleSpan;

CeDocument *ce_document_new(void);
void ce_document_free(CeDocument *doc);
/* Import is transactional. Unknown JSON nodes are retained; unsupported HTML fails explicitly.
 * Plain-text offsets are UTF-8 byte offsets. Blocks are separated by LF; atoms use U+FFFC.
 * Returned borrowed pointers remain valid until the next mutation. Export buffers use free(). */
bool ce_document_import(CeDocument *doc, const char *data, CeFormat format, char *error,
                        size_t error_size);
/* Apply a validated document JSON transformation as one undoable edit. Unlike
 * import, this preserves the savepoint and existing undo history. */
bool ce_document_apply_json(CeDocument *doc, const char *json);
const char *ce_document_text(const CeDocument *doc);
size_t ce_document_length(const CeDocument *doc);
const char *ce_document_error(const CeDocument *doc);
bool ce_document_insert(CeDocument *doc, size_t offset, const char *utf8);
bool ce_document_delete(CeDocument *doc, size_t start, size_t end);
bool ce_document_replace(CeDocument *doc, size_t start, size_t end, const char *utf8);
bool ce_document_style(CeDocument *doc, size_t start, size_t end, CeMark mark, const char *value,
                       bool enabled);
/* Clear text marks only; node attributes and text are preserved. */
bool ce_document_clear_formatting(CeDocument *doc, size_t start, size_t end);
/* Align paragraph/heading blocks intersecting [start,end), or the caret block.
 * Atomic blocks are skipped. Each call is one undo transaction. */
bool ce_document_set_alignment(CeDocument *doc, size_t start, size_t end,
                               const char *alignment);
/* List kinds change the containing list's kind (including its sibling items).
 * Paragraph/heading lifts selected list items out of their list containers,
 * retaining descendant content in order. Ranges are half-open. */
bool ce_document_set_block(CeDocument *doc, size_t start, size_t end, CeBlockKind kind,
                           unsigned level);
size_t ce_document_block_count(const CeDocument *doc);
bool ce_document_block_info(const CeDocument *doc, size_t index, CeBlockInfo *out);
bool ce_document_block_at(const CeDocument *doc, size_t offset, CeBlockInfo *out);
size_t ce_document_span_count(const CeDocument *doc);
bool ce_document_span(const CeDocument *doc, size_t index, CeStyleSpan *out);
CeStyle ce_document_style_at(const CeDocument *doc, size_t offset);
char *ce_document_block_json(const CeDocument *doc, size_t index);
/* At an interior text offset, split that paragraph/heading around the new node.
 * Text insertion at an atomic block boundary creates an adjacent paragraph. */
bool ce_document_insert_node(CeDocument *doc, size_t offset, const char *node_json);
/* Moves a whole list item, including descendants, when both indices address
 * leading paragraphs of sibling items. Other cross-container moves fail. */
bool ce_document_move_block(CeDocument *doc, size_t from, size_t to);
/* As above, returning the moved block's new byte offset after rebuilding.
 * Useful when a moved list item carries descendants and flat indices change. */
bool ce_document_move_block_ex(CeDocument *doc, size_t from, size_t to,
                               size_t *new_offset);
bool ce_document_undo(CeDocument *doc);
bool ce_document_redo(CeDocument *doc);
bool ce_document_can_undo(const CeDocument *doc);
bool ce_document_can_redo(const CeDocument *doc);
void ce_document_mark_saved(CeDocument *doc);
bool ce_document_is_modified(const CeDocument *doc);
/* Case-insensitive ASCII search, exact UTF-8 matching otherwise; wraps at document ends. */
bool ce_document_find(const CeDocument *doc, const char *keyword, size_t from, bool backward,
                      size_t *start, size_t *end);
char *ce_document_export_json(const CeDocument *doc);
char *ce_document_export_html(CeDocument *doc);
#ifdef __cplusplus
}
#endif
#endif
