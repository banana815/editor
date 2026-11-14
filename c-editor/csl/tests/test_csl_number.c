#include "csl_number.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char error[256];
#define OK(call) do { CeCslNumberStatus status = (call); if (status != CE_CSL_NUMBER_OK) fprintf(stderr, "%s: %s\n", #call, error); assert(status == CE_CSL_NUMBER_OK); } while (0)
#define TERM(name, text) "<term name='" name "'>" text "</term>"
static const char english[] = TERM("ordinal", "th") TERM("ordinal-01", "st") TERM("ordinal-02", "nd")
    TERM("ordinal-03", "rd") TERM("ordinal-11", "th") TERM("ordinal-12", "th") TERM("ordinal-13", "th")
    TERM("long-ordinal-01", "first") TERM("long-ordinal-02", "second") TERM("long-ordinal-10", "tenth");
static CeCslLocale *locale_terms(const char *terms, CeCslXml **xml) {
    const char *prefix = "<style xmlns='" CE_CSL_NAMESPACE "' version='1.0'><locale><terms>";
    const char *suffix = "</terms></locale></style>";
    size_t size = strlen(prefix) + strlen(terms) + strlen(suffix);
    char *input = malloc(size + 1);
    assert(input); snprintf(input, size + 1, "%s%s%s", prefix, terms, suffix);
    *xml = ce_csl_xml_parse(input, size, error, sizeof error); free(input);
    assert(*xml);
    CeCslLocale *locale = ce_csl_locale_new((*xml)->root, NULL, "en-US", NULL, error, sizeof error);
    if (!locale) fprintf(stderr, "%s\n", error);
    assert(locale); return locale;
}
static void number_is(uint64_t number, CeCslNumberForm form, const CeCslLocale *locale,
                       CeCslLocaleGender gender, const char *expected, bool expected_missing) {
    CeCslOutputString out = {0};
    bool missing = !expected_missing;
    OK(ce_csl_number_format(number, form, locale, gender, NULL, &out, &missing, error, sizeof error));
    if (strcmp(out.data, expected)) fprintf(stderr, "Number actual '%s', expected '%s'\n", out.data, expected);
    assert(!strcmp(out.data, expected) && out.length == strlen(expected) && missing == expected_missing);
    ce_csl_output_string_free(&out);
}
static void test_scalar_forms(void) {
    CeCslXml *xml;
    CeCslLocale *locale = locale_terms(english, &xml);
    number_is(0, CE_CSL_NUMBER_NUMERIC, NULL, CE_CSL_LOCALE_NEUTER, "0", false);
    number_is(UINT64_MAX, CE_CSL_NUMBER_NUMERIC, NULL, CE_CSL_LOCALE_NEUTER, "18446744073709551615", false);
    number_is(1, CE_CSL_NUMBER_ORDINAL, locale, CE_CSL_LOCALE_NEUTER, "1st", false);
    number_is(2, CE_CSL_NUMBER_ORDINAL, locale, CE_CSL_LOCALE_NEUTER, "2nd", false);
    number_is(3, CE_CSL_NUMBER_ORDINAL, locale, CE_CSL_LOCALE_NEUTER, "3rd", false);
    number_is(4, CE_CSL_NUMBER_ORDINAL, locale, CE_CSL_LOCALE_NEUTER, "4th", false);
    number_is(11, CE_CSL_NUMBER_ORDINAL, locale, CE_CSL_LOCALE_NEUTER, "11th", false);
    number_is(12, CE_CSL_NUMBER_ORDINAL, locale, CE_CSL_LOCALE_NEUTER, "12th", false);
    number_is(13, CE_CSL_NUMBER_ORDINAL, locale, CE_CSL_LOCALE_NEUTER, "13th", false);
    number_is(21, CE_CSL_NUMBER_ORDINAL, locale, CE_CSL_LOCALE_NEUTER, "21st", false);
    number_is(112, CE_CSL_NUMBER_ORDINAL, locale, CE_CSL_LOCALE_NEUTER, "112th", false);
    number_is(UINT64_MAX, CE_CSL_NUMBER_ORDINAL, locale, CE_CSL_LOCALE_NEUTER, "18446744073709551615th", false);
    number_is(1, CE_CSL_NUMBER_LONG_ORDINAL, locale, CE_CSL_LOCALE_NEUTER, "first", false);
    number_is(2, CE_CSL_NUMBER_LONG_ORDINAL, locale, CE_CSL_LOCALE_NEUTER, "second", false);
    number_is(10, CE_CSL_NUMBER_LONG_ORDINAL, locale, CE_CSL_LOCALE_NEUTER, "tenth", false);
    number_is(3, CE_CSL_NUMBER_LONG_ORDINAL, locale, CE_CSL_LOCALE_NEUTER, "3rd", false);
    number_is(21, CE_CSL_NUMBER_LONG_ORDINAL, locale, CE_CSL_LOCALE_NEUTER, "21st", false);
    ce_csl_locale_free(locale); ce_csl_xml_free(xml);
    const struct { uint64_t number; const char *roman; } examples[] = {
        {1, "i"}, {4, "iv"}, {9, "ix"}, {40, "xl"}, {49, "xlix"}, {90, "xc"},
        {400, "cd"}, {944, "cmxliv"}, {2026, "mmxxvi"}, {3888, "mmmdccclxxxviii"}, {3999, "mmmcmxcix"}
    };
    for (size_t i = 0; i < sizeof examples / sizeof *examples; ++i)
        number_is(examples[i].number, CE_CSL_NUMBER_ROMAN, NULL, CE_CSL_LOCALE_NEUTER, examples[i].roman, false);
}
static void test_locale_missing_and_empty(void) {
    CeCslXml *xml;
    CeCslLocale *locale = locale_terms(TERM("ordinal", "") TERM("long-ordinal-01", "一") TERM("long-ordinal-02", ""), &xml);
    number_is(21, CE_CSL_NUMBER_ORDINAL, locale, CE_CSL_LOCALE_NEUTER, "21", false);
    number_is(1, CE_CSL_NUMBER_LONG_ORDINAL, locale, CE_CSL_LOCALE_NEUTER, "一", false);
    number_is(2, CE_CSL_NUMBER_LONG_ORDINAL, locale, CE_CSL_LOCALE_NEUTER, "", false);
    number_is(3, CE_CSL_NUMBER_LONG_ORDINAL, locale, CE_CSL_LOCALE_NEUTER, "3", false);
    ce_csl_locale_free(locale); ce_csl_xml_free(xml);
    locale = locale_terms(TERM("and", "and"), &xml);
    number_is(1, CE_CSL_NUMBER_ORDINAL, locale, CE_CSL_LOCALE_NEUTER, "1", true);
    number_is(2, CE_CSL_NUMBER_LONG_ORDINAL, locale, CE_CSL_LOCALE_NEUTER, "2", true);
    ce_csl_locale_free(locale); ce_csl_xml_free(xml);
    number_is(1, CE_CSL_NUMBER_ORDINAL, NULL, CE_CSL_LOCALE_NEUTER, "1", true);
    locale = locale_terms(TERM("ordinal", "e")
        "<term name='ordinal-01' gender-form='masculine' match='whole-number'>er</term>"
        "<term name='ordinal-01' gender-form='feminine' match='whole-number'>re</term>"
        "<term name='long-ordinal-01' gender-form='masculine'>premier</term>"
        "<term name='long-ordinal-01' gender-form='feminine'>première</term>", &xml);
    number_is(1, CE_CSL_NUMBER_ORDINAL, locale, CE_CSL_LOCALE_MASCULINE, "1er", false);
    number_is(1, CE_CSL_NUMBER_ORDINAL, locale, CE_CSL_LOCALE_FEMININE, "1re", false);
    number_is(2, CE_CSL_NUMBER_ORDINAL, locale, CE_CSL_LOCALE_FEMININE, "2e", false);
    number_is(1, CE_CSL_NUMBER_LONG_ORDINAL, locale, CE_CSL_LOCALE_FEMININE, "première", false);
    number_is(21, CE_CSL_NUMBER_LONG_ORDINAL, locale, CE_CSL_LOCALE_MASCULINE, "21e", false);
    ce_csl_locale_free(locale); ce_csl_xml_free(xml);
}
static void pair_is(const char *start, const char *end, CeCslPageRangeFormat format,
                     const char *delimiter, const char *expected) {
    CeCslOutputString out = {0};
    OK(ce_csl_page_range_pair(start, strlen(start), end, strlen(end), format,
                               delimiter, strlen(delimiter), NULL, &out, error, sizeof error));
    if (strcmp(out.data, expected)) fprintf(stderr, "Range %s/%s mode %d: actual '%s', expected '%s'\n", start, end, (int)format, out.data, expected);
    assert(out.length == strlen(expected) && !strcmp(out.data, expected));
    ce_csl_output_string_free(&out);
}
/* Exact numeric examples from CSL 1.0.2 Appendix V. Chicago aliases 15 here,
 * not 16: https://docs.citationstyles.org/en/stable/specification.html#appendix-v-page-range-formats */
static void test_specification_ranges(void) {
    pair_is("42", "5", CE_CSL_PAGE_EXPANDED, "–", "42–45");
    pair_is("321", "8", CE_CSL_PAGE_EXPANDED, "–", "321–328");
    pair_is("2787", "816", CE_CSL_PAGE_EXPANDED, "–", "2787–2816");
    pair_is("42", "45", CE_CSL_PAGE_MINIMAL, "–", "42–5");
    pair_is("321", "328", CE_CSL_PAGE_MINIMAL, "–", "321–8");
    pair_is("2787", "2816", CE_CSL_PAGE_MINIMAL, "–", "2787–816");
    pair_is("42", "45", CE_CSL_PAGE_MINIMAL_TWO, "–", "42–45");
    pair_is("321", "328", CE_CSL_PAGE_MINIMAL_TWO, "–", "321–28");
    pair_is("2787", "2816", CE_CSL_PAGE_MINIMAL_TWO, "–", "2787–816");
    const struct { const char *start, *end, *expected; } chicago_common[] = {
        {"3", "10", "3–10"}, {"71", "72", "71–72"}, {"100", "104", "100–104"},
        {"600", "613", "600–613"}, {"1100", "1123", "1100–1123"}, {"107", "108", "107–8"},
        {"505", "517", "505–17"}, {"1002", "1006", "1002–6"}, {"321", "325", "321–25"},
        {"415", "532", "415–532"}, {"11564", "11568", "11564–68"}, {"13792", "13803", "13792–803"}
    };
    for (size_t i = 0; i < sizeof chicago_common / sizeof *chicago_common; ++i) {
        pair_is(chicago_common[i].start, chicago_common[i].end, CE_CSL_PAGE_CHICAGO, "–", chicago_common[i].expected);
        pair_is(chicago_common[i].start, chicago_common[i].end, CE_CSL_PAGE_CHICAGO_15, "–", chicago_common[i].expected);
        pair_is(chicago_common[i].start, chicago_common[i].end, CE_CSL_PAGE_CHICAGO_16, "–", chicago_common[i].expected);
    }
    pair_is("1496", "1504", CE_CSL_PAGE_CHICAGO, "–", "1496–1504");
    pair_is("2787", "2816", CE_CSL_PAGE_CHICAGO_15, "–", "2787–2816");
    pair_is("1496", "1504", CE_CSL_PAGE_CHICAGO_16, "–", "1496–504");
    pair_is("92", "113", CE_CSL_PAGE_CHICAGO_16, "–", "92–113");
    pair_is("1087", "1089", CE_CSL_PAGE_CHICAGO_16, "–", "1087–89");
    pair_is("1496", "1500", CE_CSL_PAGE_CHICAGO_16, "–", "1496–500");
    pair_is("12991", "13001", CE_CSL_PAGE_CHICAGO_16, "–", "12991–3001");
}
static void test_exact_decimal_and_delimiters(void) {
    pair_is("1987", "86", CE_CSL_PAGE_EXPANDED, "–", "1987–2086");
    pair_is("98", "2", CE_CSL_PAGE_EXPANDED, "-", "98-102");
    pair_is("9999", "1", CE_CSL_PAGE_EXPANDED, "-", "9999-10001");
    pair_is("1999", "002", CE_CSL_PAGE_EXPANDED, "-", "1999-2002");
    pair_is("0", "0", CE_CSL_PAGE_MINIMAL, "-", "0-0");
    pair_is("42", "42", CE_CSL_PAGE_MINIMAL, "-", "42-42");
    pair_is("0007", "9", CE_CSL_PAGE_MINIMAL, "-", "0007-0009");
    pair_is("007", "009", CE_CSL_PAGE_CHICAGO_16, "-", "007-009");
    pair_is("18446744073709551616", "18446744073709551619", CE_CSL_PAGE_MINIMAL, "–", "18446744073709551616–9");
    pair_is("12", "14", CE_CSL_PAGE_EXPANDED, " 至 ", "12 至 14");
    pair_is("12", "14", CE_CSL_PAGE_EXPANDED, "", "1214");
    pair_is("12", "14", CE_CSL_PAGE_EXPANDED, "\r\n", "12\r\n14");
    char *huge = malloc(CE_CSL_NUMBER_MAX_DECIMAL_DIGITS + 1);
    assert(huge); memset(huge, '9', CE_CSL_NUMBER_MAX_DECIMAL_DIGITS);
    huge[CE_CSL_NUMBER_MAX_DECIMAL_DIGITS] = 0;
    CeCslOutputString out = {0};
    OK(ce_csl_page_range_pair(huge, CE_CSL_NUMBER_MAX_DECIMAL_DIGITS, "0", 1, CE_CSL_PAGE_EXPANDED,
                               "-", 1, NULL, &out, error, sizeof error));
    assert(out.length == 2 * CE_CSL_NUMBER_MAX_DECIMAL_DIGITS + 2);
    assert(!memcmp(out.data, huge, CE_CSL_NUMBER_MAX_DECIMAL_DIGITS));
    assert(out.data[CE_CSL_NUMBER_MAX_DECIMAL_DIGITS] == '-' && out.data[CE_CSL_NUMBER_MAX_DECIMAL_DIGITS + 1] == '1');
    for (size_t i = CE_CSL_NUMBER_MAX_DECIMAL_DIGITS + 2; i < out.length; ++i) assert(out.data[i] == '0');
    free(huge); ce_csl_output_string_free(&out);
    OK(ce_csl_number_format(123, CE_CSL_NUMBER_NUMERIC, NULL, CE_CSL_LOCALE_NEUTER, NULL, &out, NULL, error, sizeof error));
    OK(ce_csl_page_range_pair(out.data, out.length, "5", 1, CE_CSL_PAGE_EXPANDED, "-", 1, NULL, &out, error, sizeof error));
    assert(!strcmp(out.data, "123-125")); ce_csl_output_string_free(&out);
    CeCslXml *xml;
    CeCslLocale *locale = locale_terms(TERM("page-range-delimiter", "～"), &xml);
    CeCslLocaleTerm delimiter = ce_csl_locale_term(locale, "page-range-delimiter", CE_CSL_LOCALE_LONG, false, CE_CSL_LOCALE_NEUTER);
    assert(delimiter.found);
    OK(ce_csl_page_range_pair("42", 2, "45", 2, CE_CSL_PAGE_EXPANDED, delimiter.text, delimiter.length, NULL, &out, error, sizeof error));
    assert(!strcmp(out.data, "42～45")); ce_csl_output_string_free(&out);
    ce_csl_locale_free(locale); ce_csl_xml_free(xml);
}
static void test_rejections_are_atomic(void) {
    CeCslOutputString out = {0};
    OK(ce_csl_number_format(42, CE_CSL_NUMBER_NUMERIC, NULL, CE_CSL_LOCALE_NEUTER, NULL, &out, NULL, error, sizeof error));
    char *old = out.data;
    bool missing = true;
    assert(ce_csl_number_format(0, CE_CSL_NUMBER_ROMAN, NULL, CE_CSL_LOCALE_NEUTER, NULL, &out, &missing, error, sizeof error) == CE_CSL_NUMBER_UNSUPPORTED);
    assert(ce_csl_number_format(4000, CE_CSL_NUMBER_ROMAN, NULL, CE_CSL_LOCALE_NEUTER, NULL, &out, &missing, error, sizeof error) == CE_CSL_NUMBER_UNSUPPORTED);
    assert(ce_csl_number_format(UINT64_MAX, CE_CSL_NUMBER_ROMAN, NULL, CE_CSL_LOCALE_NEUTER, NULL, &out, &missing, error, sizeof error) == CE_CSL_NUMBER_UNSUPPORTED);
    assert(ce_csl_number_format(1, (CeCslNumberForm)99, NULL, CE_CSL_LOCALE_NEUTER, NULL, &out, &missing, error, sizeof error) == CE_CSL_NUMBER_INVALID_ARGUMENT);
    assert(ce_csl_number_format(1, CE_CSL_NUMBER_NUMERIC, NULL, (CeCslLocaleGender)99, NULL, &out, &missing, error, sizeof error) == CE_CSL_NUMBER_INVALID_ARGUMENT);
    const char *bad[] = {"", "S12", "12b", "-2", "+2", " 2", "2 ", "1.5", "１２", "2-4", "2,3"};
    for (size_t i = 0; i < sizeof bad / sizeof *bad; ++i) {
        assert(ce_csl_page_range_pair(bad[i], strlen(bad[i]), "20", 2, CE_CSL_PAGE_EXPANDED, "-", 1, NULL, &out, error, sizeof error) == CE_CSL_NUMBER_INVALID_ARGUMENT);
        assert(ce_csl_page_range_pair("1", 1, bad[i], strlen(bad[i]), CE_CSL_PAGE_EXPANDED, "-", 1, NULL, &out, error, sizeof error) == CE_CSL_NUMBER_INVALID_ARGUMENT);
    }
    assert(ce_csl_page_range_pair("42", 2, "20", 2, CE_CSL_PAGE_EXPANDED, "-", 1, NULL, &out, error, sizeof error) == CE_CSL_NUMBER_UNSUPPORTED);
    assert(ce_csl_page_range_pair("42", 2, "0020", 4, CE_CSL_PAGE_EXPANDED, "-", 1, NULL, &out, error, sizeof error) == CE_CSL_NUMBER_UNSUPPORTED);
    assert(ce_csl_page_range_pair("1", 1, "2", 1, CE_CSL_PAGE_EXPANDED, "\xc0\xaf", 2, NULL, &out, error, sizeof error) == CE_CSL_NUMBER_INVALID_UTF8);
    assert(ce_csl_page_range_pair("1", 1, "2", 1, CE_CSL_PAGE_EXPANDED, "x\0y", 3, NULL, &out, error, sizeof error) == CE_CSL_NUMBER_INVALID_UTF8);
    assert(ce_csl_page_range_pair("1", 1, "2", 1, CE_CSL_PAGE_EXPANDED, NULL, 0, NULL, &out, error, sizeof error) == CE_CSL_NUMBER_INVALID_ARGUMENT);
    assert(ce_csl_page_range_pair("1", CE_CSL_NUMBER_MAX_DECIMAL_DIGITS + 1, "2", 1, CE_CSL_PAGE_EXPANDED, "-", 1, NULL, &out, error, sizeof error) == CE_CSL_NUMBER_LIMIT);
    assert(ce_csl_page_range_pair("1", 1, "2", 1, CE_CSL_PAGE_EXPANDED, "-", CE_CSL_NUMBER_MAX_DELIMITER_BYTES + 1, NULL, &out, error, sizeof error) == CE_CSL_NUMBER_LIMIT);
    assert(out.data == old && !strcmp(out.data, "42") && missing && *error);
    size_t term_size = CE_CSL_OUTPUT_MAX_BYTES + strlen("<term name='ordinal'></term>");
    char *large_term = malloc(term_size + 1);
    assert(large_term);
    size_t prefix = strlen("<term name='ordinal'>");
    memcpy(large_term, "<term name='ordinal'>", prefix);
    memset(large_term + prefix, 'x', CE_CSL_OUTPUT_MAX_BYTES);
    strcpy(large_term + prefix + CE_CSL_OUTPUT_MAX_BYTES, "</term>");
    CeCslXml *xml;
    CeCslLocale *locale = locale_terms(large_term, &xml);
    free(large_term);
    assert(ce_csl_number_format(1, CE_CSL_NUMBER_ORDINAL, locale, CE_CSL_LOCALE_NEUTER,
        NULL, &out, &missing, error, sizeof error) == CE_CSL_NUMBER_LIMIT);
    assert(out.data == old && missing);
    ce_csl_locale_free(locale); ce_csl_xml_free(xml);
    assert(!strcmp(ce_csl_number_status_string(CE_CSL_NUMBER_UNSUPPORTED), "Unsupported numeric representation"));
    assert(*ce_csl_number_status_string((CeCslNumberStatus)99));
    ce_csl_output_string_free(&out);
}
static void test_range_abbreviation_roundtrip(void) {
    const unsigned differences[] = {1, 10, 125};
    for (unsigned start = 0; start < 4096; start += 37) {
        for (size_t i = 0; i < sizeof differences / sizeof *differences; ++i) {
            char first[16], last[16], expected[40];
            snprintf(first, sizeof first, "%u", start);
            snprintf(last, sizeof last, "%u", start + differences[i]);
            snprintf(expected, sizeof expected, "%s:%s", first, last);
            for (int mode = CE_CSL_PAGE_EXPANDED; mode <= CE_CSL_PAGE_CHICAGO_16; ++mode) {
                CeCslOutputString abbreviated = {0}, expanded = {0};
                OK(ce_csl_page_range_pair(first, strlen(first), last, strlen(last), (CeCslPageRangeFormat)mode,
                    ":", 1, NULL, &abbreviated, error, sizeof error));
                const char *separator = strchr(abbreviated.data, ':');
                assert(separator);
                OK(ce_csl_page_range_pair(abbreviated.data, (size_t)(separator - abbreviated.data),
                    separator + 1, strlen(separator + 1), CE_CSL_PAGE_EXPANDED, ":", 1,
                    NULL, &expanded, error, sizeof error));
                assert(!strcmp(expanded.data, expected));
                ce_csl_output_string_free(&abbreviated); ce_csl_output_string_free(&expanded);
            }
        }
    }
}
typedef union Allocation {
    struct { size_t bytes, magic; } header;
    long double alignment;
    void *pointer;
} Allocation;
typedef struct Memory { size_t calls, fail_at, live, bytes; } Memory;
static void *test_allocate(void *context, size_t bytes) {
    Memory *memory = context;
    if (++memory->calls == memory->fail_at) return NULL;
    Allocation *allocation = malloc(sizeof *allocation + bytes);
    assert(allocation); allocation->header.bytes = bytes; allocation->header.magic = 0xc517;
    ++memory->live; memory->bytes += bytes;
    return allocation + 1;
}
static void test_deallocate(void *context, void *pointer) {
    Memory *memory = context;
    Allocation *allocation = (Allocation *)pointer - 1;
    assert(pointer && allocation->header.magic == 0xc517 && memory->live);
    assert(memory->bytes >= allocation->header.bytes);
    --memory->live; memory->bytes -= allocation->header.bytes; allocation->header.magic = 0;
    free(allocation);
}
static void test_oom(void) {
    Memory memory = {0};
    CeCslOutputAllocator allocator = {&memory, test_allocate, test_deallocate};
    CeCslOutputString out = {0};
    OK(ce_csl_number_format(42, CE_CSL_NUMBER_NUMERIC, NULL, CE_CSL_LOCALE_NEUTER, NULL, &out, NULL, error, sizeof error));
    char *old = out.data;
    memory.fail_at = 1;
    bool missing = false;
    assert(ce_csl_number_format(21, CE_CSL_NUMBER_ORDINAL, NULL, CE_CSL_LOCALE_NEUTER, &allocator, &out, &missing, error, sizeof error) == CE_CSL_NUMBER_OOM);
    assert(out.data == old && !missing && !memory.live);
    for (size_t point = 1; point <= 2; ++point) {
        memory.calls = 0; memory.fail_at = point;
        assert(ce_csl_page_range_pair("2787", 4, "816", 3, CE_CSL_PAGE_EXPANDED, "–", strlen("–"), &allocator, &out, error, sizeof error) == CE_CSL_NUMBER_OOM);
        assert(out.data == old && !strcmp(out.data, "42") && !memory.live && !memory.bytes);
    }
    memory.calls = 0; memory.fail_at = 0;
    OK(ce_csl_page_range_pair("2787", 4, "816", 3, CE_CSL_PAGE_EXPANDED, "–", strlen("–"), &allocator, &out, error, sizeof error));
    assert(memory.live == 1 && !strcmp(out.data, "2787–2816"));
    OK(ce_csl_number_format(21, CE_CSL_NUMBER_ORDINAL, NULL, CE_CSL_LOCALE_NEUTER, &allocator, &out, &missing, error, sizeof error));
    assert(memory.live == 1 && missing && !strcmp(out.data, "21"));
    allocator.deallocate = NULL;
    old = out.data;
    assert(ce_csl_number_format(1, CE_CSL_NUMBER_NUMERIC, NULL, CE_CSL_LOCALE_NEUTER, &allocator, &out, NULL, error, sizeof error) == CE_CSL_NUMBER_INVALID_ARGUMENT);
    assert(out.data == old && memory.live == 1);
    ce_csl_output_string_free(&out);
    assert(!memory.live && !memory.bytes);
}
static void test_real_locale(const char *path) {
    FILE *file = fopen(path, "rb");
    assert(file && !fseek(file, 0, SEEK_END));
    long size = ftell(file); assert(size > 0 && (size_t)size <= CE_CSL_XML_MAX_BYTES);
    rewind(file);
    char *text = malloc((size_t)size + 1);
    assert(text && fread(text, 1, (size_t)size, file) == (size_t)size);
    fclose(file); text[size] = 0;
    CeCslXml *xml = ce_csl_xml_parse(text, (size_t)size, error, sizeof error);
    free(text); assert(xml);
    const CeCslXmlNode *roots[] = {xml->root};
    CeCslLocaleCatalog catalog = {roots, 1, NULL, 0};
    const char *language = ce_csl_xml_attribute(xml->root, "xml:lang");
    CeCslLocale *locale = ce_csl_locale_new(NULL, &catalog, language, NULL, error, sizeof error);
    assert(locale);
    if (!strcmp(language, "en-US")) {
        number_is(21, CE_CSL_NUMBER_ORDINAL, locale, CE_CSL_LOCALE_NEUTER, "21st", false);
        number_is(2, CE_CSL_NUMBER_LONG_ORDINAL, locale, CE_CSL_LOCALE_NEUTER, "second", false);
    } else if (!strcmp(language, "zh-CN")) {
        number_is(21, CE_CSL_NUMBER_ORDINAL, locale, CE_CSL_LOCALE_NEUTER, "21", false);
        number_is(2, CE_CSL_NUMBER_LONG_ORDINAL, locale, CE_CSL_LOCALE_NEUTER, "二", false);
        CeCslLocaleTerm delimiter = ce_csl_locale_term(locale, "page-range-delimiter", CE_CSL_LOCALE_LONG, false, CE_CSL_LOCALE_NEUTER);
        assert(delimiter.found && !strcmp(delimiter.text, "～"));
        pair_is("42", "45", CE_CSL_PAGE_EXPANDED, delimiter.text, "42～45");
    } else assert(!"Pass the pinned en-US or zh-CN locale to this regression");
    printf("Number formatting with real locale %s passed\n", language);
    ce_csl_locale_free(locale); ce_csl_xml_free(xml);
}
int main(int argc, char **argv) {
    test_scalar_forms();
    test_locale_missing_and_empty();
    test_specification_ranges();
    test_exact_decimal_and_delimiters();
    test_rejections_are_atomic();
    test_range_abbreviation_roundtrip();
    test_oom();
    for (int i = 1; i < argc; ++i) test_real_locale(argv[i]);
    puts("C CSL scalar numbers, locale ordinals, exact page ranges, explicit limits and atomic OOM passed");
    return 0;
}
