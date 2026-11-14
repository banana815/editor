#include "csl_collation.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char error[256];
static CeCslSortKey key(const char *text, CeCslCollationLanguage language) {
    CeCslSortKey out = {0};
    bool ok = ce_csl_collation_key(text, strlen(text), language, NULL, &out, error, sizeof error);
    if (!ok) fprintf(stderr, "%s\n", error);
    assert(ok); return out;
}
static int compare(const CeCslSortKey *a, const CeCslSortKey *b) {
    size_t n = a->length < b->length ? a->length : b->length;
    int result = n ? memcmp(a->data, b->data, n) : 0;
    return result ? result : (a->length > b->length) - (a->length < b->length);
}
static void equal(const char *a, const char *b, CeCslCollationLanguage language) {
    CeCslSortKey x = key(a, language), y = key(b, language);
    assert(!compare(&x, &y)); ce_csl_sort_key_free(&x); ce_csl_sort_key_free(&y);
}
static void test_unicode(void) {
    equal("A", "a", CE_CSL_COLLATION_DEFAULT);
    equal("É", "é", CE_CSL_COLLATION_DEFAULT);
    equal("é", "e\xcc\x81", CE_CSL_COLLATION_DEFAULT);
    equal("SS", "ß", CE_CSL_COLLATION_DEFAULT);
    equal("ẞ", "ß", CE_CSL_COLLATION_DEFAULT);
    equal("ﬃ", "ffi", CE_CSL_COLLATION_DEFAULT);
    equal("L·", "Ŀ", CE_CSL_COLLATION_DEFAULT);
    equal("l·", "ŀ", CE_CSL_COLLATION_DEFAULT);
    equal("Σ", "ς", CE_CSL_COLLATION_DEFAULT);
    equal("I", "i", CE_CSL_COLLATION_DEFAULT);
    equal("I", "ı", CE_CSL_COLLATION_TR);
    equal("İ", "i", CE_CSL_COLLATION_TR);
    equal("I", "ı", CE_CSL_COLLATION_AZ);
    equal("İ", "i", CE_CSL_COLLATION_AZ);
    CeCslSortKey a = key("cafe", CE_CSL_COLLATION_DEFAULT);
    CeCslSortKey b = key("café", CE_CSL_COLLATION_DEFAULT);
    CeCslSortKey c = key("caff", CE_CSL_COLLATION_DEFAULT);
    assert(compare(&a, &b) < 0 && compare(&b, &c) < 0);
    ce_csl_sort_key_free(&a); ce_csl_sort_key_free(&b); ce_csl_sort_key_free(&c);
    a = key("İ", CE_CSL_COLLATION_DEFAULT); b = key("i", CE_CSL_COLLATION_DEFAULT);
    assert(compare(&a, &b)); ce_csl_sort_key_free(&a); ce_csl_sort_key_free(&b);
}
static void test_canonical_boundary(void) {
    char text[66]; memcpy(text, "á", 2);
    for (size_t i = 0; i < 31; ++i) memcpy(text + 2 + i * 2, "\xcc\xaf", 2);
    text[64] = 0;
    CeCslSortKey actual = key(text, CE_CSL_COLLATION_DEFAULT);
    /* DUCET 18 allkeys.txt golden weights: a=2485/0020, U+032F=0/0034,
     * U+0301=0/0024. The acute must follow all 31 below marks. This catches
     * the upstream 32-scalar flush bug even when NFC makes input forms agree. */
    unsigned char expected[72] = {0x24, 0x85, 0, 0, 0, 0x20};
    for (size_t i = 0; i < 31; ++i) expected[6 + i * 2 + 1] = 0x34;
    expected[69] = 0x24;
    assert(actual.length == sizeof expected && !memcmp(actual.data, expected, sizeof expected));
    ce_csl_sort_key_free(&actual);
}
static void test_empty_and_alias(void) {
    CeCslSortKey empty = key("", CE_CSL_COLLATION_DEFAULT);
    CeCslSortKey ignorable = key("\xe2\x80\x8b", CE_CSL_COLLATION_DEFAULT);
    CeCslSortKey present = key("a", CE_CSL_COLLATION_DEFAULT);
    assert(!empty.length && !ignorable.length);
    CeCslSortKeyView rows[] = {{present.data, present.length, false}, {NULL, 0, true},
                              {ignorable.data, ignorable.length, false}};
    CeCslSortOrder order = {0};
    assert(ce_csl_sort_order(rows, 3, 1, NULL, NULL, &order, error, sizeof error));
    assert(order.indices[0] == 2 && order.indices[1] == 0 && order.indices[2] == 1);
    bool descending = true;
    assert(ce_csl_sort_order(rows, 3, 1, &descending, NULL, &order, error, sizeof error));
    assert(order.indices[0] == 0 && order.indices[1] == 2 && order.indices[2] == 1);
    ce_csl_sort_order_free(&order); ce_csl_sort_key_free(&empty);
    ce_csl_sort_key_free(&ignorable); ce_csl_sort_key_free(&present);
    CeCslSortKey alias = {0}, expected = key("É", CE_CSL_COLLATION_DEFAULT);
    assert(ce_csl_sort_key_copy((const unsigned char *)"É", 2, NULL, &alias, error, sizeof error));
    assert(ce_csl_collation_key((const char *)alias.data, alias.length,
        CE_CSL_COLLATION_DEFAULT, NULL, &alias, error, sizeof error));
    assert(!compare(&alias, &expected)); ce_csl_sort_key_free(&alias); ce_csl_sort_key_free(&expected);
}
static void reject(const char *text, size_t size) {
    CeCslSortKey out = key("keep", CE_CSL_COLLATION_DEFAULT);
    unsigned char *saved = out.data; size_t length = out.length;
    assert(!ce_csl_collation_key(text, size, CE_CSL_COLLATION_DEFAULT,
                                NULL, &out, error, sizeof error));
    assert(error[0] && out.data == saved && out.length == length);
    ce_csl_sort_key_free(&out);
}
static void test_limits(void) {
    reject("a\0b", 3); reject("\xc0\xaf", 2); reject("\xed\xa0\x80", 3);
    reject("\xf4\x90\x80\x80", 4); reject("\xe2\x82", 2); reject("\x01", 1);
    reject(NULL, 1); reject("", CE_CSL_COLLATION_MAX_TEXT_BYTES + 1);
    char marks[132]; marks[0] = 'a';
    for (size_t i = 0; i < 65; ++i) memcpy(marks + 1 + i * 2, "\xcc\x81", 2);
    CeCslSortKey out = {0};
    assert(ce_csl_collation_key(marks, 129, CE_CSL_COLLATION_DEFAULT,
                              NULL, &out, error, sizeof error));
    ce_csl_sort_key_free(&out); reject(marks, 131);
    /* U+0344 itself canonically expands into two nonstarters. */
    for (size_t i = 0; i < 33; ++i) memcpy(marks + 1 + i * 2, "\xcd\x84", 2);
    reject(marks, 67);
    char *large = malloc(CE_CSL_COLLATION_MAX_TEXT_BYTES); assert(large);
    memset(large, 'A', CE_CSL_COLLATION_MAX_TEXT_BYTES);
    assert(ce_csl_collation_key(large, CE_CSL_COLLATION_MAX_TEXT_BYTES,
        CE_CSL_COLLATION_DEFAULT, NULL, &out, error, sizeof error));
    ce_csl_sort_key_free(&out);
    /* Full fold expands U+0390 from two to six bytes. */
    for (size_t i = 0; i < 22000; i += 2) memcpy(large + i, "\xce\x90", 2);
    reject(large, 22000);
    /* Fold fits, but canonical decomposition expands é from two to three. */
    for (size_t i = 0; i < 44000; i += 2) memcpy(large + i, "é", 2);
    reject(large, 44000);
    /* Arabic ligature U+FDFA produces a large DUCET expansion. */
    for (size_t i = 0; i < 65535; i += 3) memcpy(large + i, "\xef\xb7\xba", 3);
    reject(large, 65535); free(large);
}
typedef union Header { struct { unsigned magic; } h; long double align; void *pointer; } Header;
typedef struct Memory { size_t calls, fail, live; } Memory;
static void *allocate(void *context, size_t size) {
    Memory *m = context; if (++m->calls == m->fail) return NULL;
    Header *h = malloc(sizeof *h + size); assert(h); h->h.magic = 0xc011a7e;
    ++m->live; return h + 1;
}
static void deallocate(void *context, void *pointer) {
    Memory *m = context; Header *h = (Header *)pointer - 1;
    assert(m->live && h->h.magic == 0xc011a7e); --m->live; h->h.magic = 0; free(h);
}
static void test_atomic_oom(void) {
    size_t failures = 0;
    for (size_t point = 1; point < 100; ++point) {
        Memory m = {0, point, 0}; CeCslOutputAllocator allocator = {&m, allocate, deallocate};
        CeCslSortKey out = key("keep", CE_CSL_COLLATION_DEFAULT);
        unsigned char *saved = out.data; size_t length = out.length;
        const char *text = "Straße É L· İ";
        bool ok = ce_csl_collation_key(text, strlen(text), CE_CSL_COLLATION_DEFAULT,
                                       &allocator, &out, error, sizeof error);
        if (!ok) { assert(error[0] && saved == out.data && length == out.length); ++failures; }
        ce_csl_sort_key_free(&out); assert(!m.live);
        if (ok) { assert(m.calls < point); break; }
    }
    assert(failures >= 5);
    Memory m = {0}; CeCslOutputAllocator invalid = {&m, allocate, NULL};
    CeCslSortKey out = key("keep", CE_CSL_COLLATION_DEFAULT); unsigned char *saved = out.data;
    assert(!ce_csl_collation_key("a", 1, CE_CSL_COLLATION_DEFAULT, &invalid, &out, error, sizeof error));
    assert(!ce_csl_collation_key("a", 1, (CeCslCollationLanguage)99, NULL, &out, error, sizeof error));
    assert(out.data == saved && m.calls == 0); ce_csl_sort_key_free(&out);
    printf("Collation caller allocator failure points: %zu\n", failures);
}
int main(void) {
    test_unicode(); test_canonical_boundary(); test_empty_and_alias(); test_limits(); test_atomic_oom();
    puts("CSL bounded full-fold/NFD/DUCET collation, atomic keys and semantic-empty separation passed");
    return 0;
}
