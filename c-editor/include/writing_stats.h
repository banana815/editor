#ifndef CE_WRITING_STATS_H
#define CE_WRITING_STATS_H
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif

typedef struct CeWritingStats {
    size_t characters;
    size_t words;
    size_t paragraphs;
    size_t reading_minutes;
} CeWritingStats;

/* Statistics for a bounded UTF-8 buffer; pass text + start, end - start to
 * measure a selection. No allocation, mutation, locale or NUL terminator is
 * required. NULL, an empty buffer, or a whitespace-only buffer yields zero.
 *
 * characters counts Unicode scalars, not grapheme clusters: punctuation,
 * emoji and combining marks count, but Unicode whitespace, control/format
 * characters and the document's U+FFFC object placeholder do not.
 * words is a writing estimate: each Han, kana or Hangul scalar is one unit;
 * other runs separated by whitespace or common punctuation/symbol ranges
 * are one unit. Combining marks and joiners do not create extra words.
 * Emoji are symbols, not words. This is not linguistic word segmentation.
 * paragraphs counts nonempty lines (CR, LF, CRLF, U+2028 or U+2029); blank
 * lines and lines containing only excluded characters do not count.
 * reading_minutes is words / 200 rounded up, or zero when there are no words.
 * Malformed UTF-8 bytes, including a partial scalar at a selection boundary,
 * are skipped and separate words. All accesses stay within length bytes. */
CeWritingStats ce_writing_stats(const char *text, size_t length);

#ifdef __cplusplus
}
#endif
#endif
