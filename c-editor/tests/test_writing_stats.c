#include "writing_stats.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static void check(const char *text, size_t length, size_t characters, size_t words,
                  size_t paragraphs, size_t minutes) {
    CeWritingStats result = ce_writing_stats(text, length);
    assert(result.characters == characters);
    assert(result.words == words);
    assert(result.paragraphs == paragraphs);
    assert(result.reading_minutes == minutes);
}

int main(void) {
    check(NULL, 100, 0, 0, 0, 0);
    check("", 0, 0, 0, 0, 0);
    const char *blank = " \t\r\n\v\f\xC2\xA0\xE3\x80\x80\xEF\xBF\xBC";
    check(blank, strlen(blank), 0, 0, 0, 0);
    const char *mixed = "Hello, 世界！\n\nWrite a blog.\r\n你好 SDL3";
    check(mixed, strlen(mixed), 26, 9, 3, 1);
    const char *unicode = "café café\xCC\x81 中文かな한글🙂";
    check(unicode, strlen(unicode), 16, 8, 1, 1);
    const char *emoji = "👩‍💻 🙂\xEF\xB8\x8F\n！？";
    check(emoji, strlen(emoji), 6, 0, 2, 0);
    const char *objects = "one\xEF\xBF\xBCtwo\n\xEF\xBF\xBC\nthree";
    check(objects, strlen(objects), 11, 3, 2, 1);
    const char *separators = "one—two，three\xE2\x80\xA8\xE2\x80\xA9四";
    check(separators, strlen(separators), 14, 4, 2, 1);
    const char *controls = "ab\xE2\x80\x8D" "cd\xE2\x80\xAE" "ef\xEF\xBB\xBF";
    check(controls, strlen(controls), 6, 1, 1, 1);
    const char *range = "before 中文 after";
    check(range + 7, 6, 2, 2, 1, 1);
    check(range + 8, 5, 1, 1, 1, 1);
    check(range + 7, 5, 1, 1, 1, 1);
    const char bounded[] = {'a', 'b', 'c', 'd'};
    check(bounded, 3, 3, 1, 1, 1);
    const char invalid[] = "a\xC0\xAF" "b\xED\xA0\x80" "c\xF4\x90\x80\x80" "d\xE4";
    check(invalid, sizeof invalid - 1, 4, 4, 1, 1);
    char long_text[403];
    for (size_t i = 0; i < 201; ++i) { long_text[2*i] = 'a'; long_text[2*i+1] = ' '; }
    check(long_text, 398, 199, 199, 1, 1);
    check(long_text, 400, 200, 200, 1, 1);
    check(long_text, 402, 201, 201, 1, 2);
    puts("writing statistics tests passed");
    return 0;
}
