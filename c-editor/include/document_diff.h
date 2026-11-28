#ifndef CE_DOCUMENT_DIFF_H
#define CE_DOCUMENT_DIFF_H
#include "document.h"
/* Read-only paragraph comparison. Text and each block's complete JSON are
 * compared, so styles, image attributes and otherwise hidden node metadata
 * are not mistaken for identical content. Returned UTF-8 report uses free().
 * Bounded LCS for normal articles; large articles use ordered lookahead and
 * still emit every changed block (alignment may be less compact). */
char *ce_document_diff(const CeDocument *before,const CeDocument *after,char *error,size_t capacity);
#endif
