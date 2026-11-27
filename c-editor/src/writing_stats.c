#include "writing_stats.h"
#include <stdbool.h>
#include <stdint.h>

static uint32_t next_scalar(const unsigned char *text, size_t length, size_t *offset) {
    size_t at = *offset;
    uint32_t first = text[at], value;
    size_t count;
    if (first < 0x80) { *offset = at + 1; return first; }
    if (first >= 0xc2 && first <= 0xdf) { count = 2; value = first & 0x1f; }
    else if (first >= 0xe0 && first <= 0xef) { count = 3; value = first & 0x0f; }
    else if (first >= 0xf0 && first <= 0xf4) { count = 4; value = first & 7; }
    else { *offset = at + 1; return UINT32_MAX; }
    if (count > length - at) { *offset = at + 1; return UINT32_MAX; }
    for (size_t i = 1; i < count; ++i) {
        unsigned char byte = text[at + i];
        if ((byte & 0xc0) != 0x80) { *offset = at + 1; return UINT32_MAX; }
        value = (value << 6) | (byte & 0x3f);
    }
    if ((count == 3 && value < 0x800) || (count == 4 && value < 0x10000) ||
        (value >= 0xd800 && value <= 0xdfff) || value > 0x10ffff) {
        *offset = at + 1; return UINT32_MAX;
    }
    *offset = at + count;
    return value;
}

static bool whitespace(uint32_t c) {
    return (c >= 9 && c <= 13) || c == 0x20 || c == 0x85 || c == 0xa0 ||
        c == 0x1680 || (c >= 0x2000 && c <= 0x200a) || c == 0x2028 ||
        c == 0x2029 || c == 0x202f || c == 0x205f || c == 0x3000;
}

static bool format(uint32_t c) {
    return c == 0xad || c == 0x61c || c == 0x180e ||
        (c >= 0x200b && c <= 0x200f) || (c >= 0x202a && c <= 0x202e) ||
        (c >= 0x2060 && c <= 0x206f) || c == 0xfeff ||
        (c >= 0xfff9 && c <= 0xfffb) || c == 0xe0001 ||
        (c >= 0xe0020 && c <= 0xe007f);
}

static bool combining(uint32_t c) {
    return (c >= 0x300 && c <= 0x36f) || (c >= 0x1ab0 && c <= 0x1aff) ||
        (c >= 0x1dc0 && c <= 0x1dff) || (c >= 0x20d0 && c <= 0x20ff) ||
        (c >= 0xfe00 && c <= 0xfe0f) || (c >= 0xfe20 && c <= 0xfe2f) ||
        (c >= 0xe0100 && c <= 0xe01ef);
}

static bool cjk(uint32_t c) {
    return (c >= 0x3400 && c <= 0x4dbf) || (c >= 0x4e00 && c <= 0x9fff) ||
        (c >= 0xf900 && c <= 0xfaff) || (c >= 0x20000 && c <= 0x323af) ||
        (c >= 0x3041 && c <= 0x3096) || (c >= 0x30a1 && c <= 0x30fa) ||
        (c >= 0xff66 && c <= 0xff9d) || (c >= 0x1100 && c <= 0x11ff) ||
        (c >= 0x3131 && c <= 0x318e) || (c >= 0xa960 && c <= 0xa97c) ||
        (c >= 0xac00 && c <= 0xd7a3) || (c >= 0xd7b0 && c <= 0xd7fb);
}

static bool separator(uint32_t c) {
    if (c < 0x80) return !((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
        (c >= '0' && c <= '9'));
    return (c >= 0xa1 && c <= 0xbf && c != 0xaa && c != 0xb5 && c != 0xba) ||
        c == 0xd7 || c == 0xf7 || c == 0x37e || c == 0x387 ||
        (c >= 0x55a && c <= 0x55f) || c == 0x589 || c == 0x58a ||
        c == 0x5be || c == 0x5c0 || c == 0x5c3 || c == 0x5c6 ||
        (c >= 0x600 && c <= 0x605) || (c >= 0x609 && c <= 0x60d) ||
        c == 0x61b || c == 0x61e || c == 0x61f || (c >= 0x66a && c <= 0x66d) ||
        (c >= 0x2000 && c <= 0x206f) || (c >= 0x20a0 && c <= 0x20cf) ||
        (c >= 0x2100 && c <= 0x2bff) || (c >= 0x2e00 && c <= 0x303f) ||
        (c >= 0x3097 && c <= 0x30a0) || (c >= 0x30fb && c <= 0x30ff) ||
        (c >= 0xfe10 && c <= 0xfe1f) || (c >= 0xfe30 && c <= 0xfe6f) ||
        (c >= 0xff01 && c <= 0xff0f) || (c >= 0xff1a && c <= 0xff20) ||
        (c >= 0xff3b && c <= 0xff40) || (c >= 0xff5b && c <= 0xff65) ||
        (c >= 0xffe0 && c <= 0xffee) || (c >= 0x1f000 && c <= 0x1faff);
}

CeWritingStats ce_writing_stats(const char *text, size_t length) {
    CeWritingStats stats = {0};
    bool in_word = false, in_paragraph = false;
    if (!text) return stats;
    for (size_t at = 0; at < length;) {
        uint32_t c = next_scalar((const unsigned char *)text, length, &at);
        if (c == UINT32_MAX) { in_word = false; continue; }
        if (c == '\r' || c == '\n' || c == 0x2028 || c == 0x2029) in_paragraph = false;
        if (whitespace(c) || c < 0x20 || (c >= 0x7f && c <= 0x9f) || c == 0xfffc) {
            in_word = false; continue;
        }
        if (format(c)) continue;
        ++stats.characters;
        if (!in_paragraph) { ++stats.paragraphs; in_paragraph = true; }
        if (combining(c)) continue;
        if (cjk(c)) { ++stats.words; in_word = false; }
        else if (separator(c)) in_word = false;
        else { if (!in_word) ++stats.words; in_word = true; }
    }
    stats.reading_minutes = stats.words / 200 + (stats.words % 200 != 0);
    return stats;
}
