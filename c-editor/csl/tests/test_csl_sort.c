#include "csl_sort.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char error[256];
static CeCslSortKeyView view(const CeCslSortKey *key) {
    return (CeCslSortKeyView){key->data, key->length, false};
}
static int compare(const CeCslSortKey *a, const CeCslSortKey *b) {
    size_t n = a->length < b->length ? a->length : b->length;
    int value = n ? memcmp(a->data, b->data, n) : 0;
    if (value) return value < 0 ? -1 : 1;
    return a->length < b->length ? -1 : a->length > b->length ? 1 : 0;
}
static CeCslSortKey integer(const char *text) {
    CeCslSortKey key = {0};
    assert(ce_csl_sort_key_integer(text, strlen(text), NULL, &key, error, sizeof error));
    return key;
}
static void integers(void) {
    static const char *ordered[] = {"-999999999999999999999999999999999999999",
        "-18446744073709551616", "-1000", "-100", "-10", "-2", "-1",
        "0", "1", "2", "10", "100", "1000", "18446744073709551616",
        "999999999999999999999999999999999999999"};
    const size_t count = sizeof ordered / sizeof *ordered;
    for (size_t i = 0; i < count; ++i) {
        CeCslSortKey a = integer(ordered[i]);
        for (size_t j = 0; j < count; ++j) {
            CeCslSortKey b = integer(ordered[j]);
            assert(compare(&a, &b) == (i < j ? -1 : i > j ? 1 : 0));
            ce_csl_sort_key_free(&b);
        }
        ce_csl_sort_key_free(&a);
    }
    const char *zeros[] = {"0", "0000", "-0", "-000", "+0", "+00000"};
    CeCslSortKey zero = integer("0"), one = integer("1"), alias = {0};
    for (size_t i = 0; i < sizeof zeros / sizeof *zeros; ++i) {
        CeCslSortKey x = integer(zeros[i]);
        assert(compare(&zero, &x) == 0);
        ce_csl_sort_key_free(&x);
    }
    CeCslSortKey padded = integer("+00001");
    assert(!compare(&padded, &one));
    ce_csl_sort_key_free(&padded);
    const char *invalid[] = {"", "+", "-", " 1", "1 ", "1.0", "1e3", "1-2", "12x", "−1"};
    unsigned char *old = one.data;
    for (size_t i = 0; i < sizeof invalid / sizeof *invalid; ++i) {
        assert(!ce_csl_sort_key_integer(invalid[i], strlen(invalid[i]), NULL,
                                        &one, error, sizeof error));
        assert(one.data == old && error[0]);
    }
    assert(!ce_csl_sort_key_integer("1\0x", 3, NULL, &one, error, sizeof error));
    char *large = malloc(CE_CSL_SORT_MAX_INTEGER_DIGITS + 2);
    assert(large);
    memset(large, '9', CE_CSL_SORT_MAX_INTEGER_DIGITS + 1);
    assert(ce_csl_sort_key_integer(large, CE_CSL_SORT_MAX_INTEGER_DIGITS, NULL,
                                  &alias, error, sizeof error));
    assert(compare(&one, &alias) < 0);
    old = alias.data;
    assert(!ce_csl_sort_key_integer(large, CE_CSL_SORT_MAX_INTEGER_DIGITS + 1, NULL,
                                   &alias, error, sizeof error));
    assert(alias.data == old);
    free(large);
    ce_csl_sort_key_free(&alias);
    assert(ce_csl_sort_key_copy(one.data, one.length, NULL, &one, error, sizeof error));
    assert(ce_csl_sort_key_copy(one.data + 1, one.length - 1, NULL, &one, error, sizeof error));
    ce_csl_sort_key_free(&one);
    ce_csl_sort_key_free(&zero);
}
static void dates(void) {
    const CeCslSortDatePoint starts[] = {
        {-999999999,0,0}, {-100,0,0}, {-50,0,0}, {50,0,0}, {100,0,0},
        {2000,0,0}, {2000,0,0}, {2000,0,0}, {2000,5,0}, {2000,5,1},
        {2002,0,0}, {2002,0,0}, {999999999,12,31}};
    const CeCslSortDatePoint ends[] = {
        {0,0,0}, {0,0,0}, {0,0,0}, {0,0,0}, {0,0,0}, {0,0,0},
        {2001,0,0}, {2005,0,0}, {0,0,0}, {0,0,0}, {2003,0,0}, {2009,0,0}, {0,0,0}};
    CeCslSortKey previous = {0};
    for (size_t i = 0; i < sizeof starts / sizeof *starts; ++i) {
        CeCslSortKey current = {0};
        assert(ce_csl_sort_key_date(&starts[i], ends[i].year ? &ends[i] : NULL,
                                    NULL, &current, error, sizeof error));
        if (i) assert(compare(&previous, &current) < 0);
        ce_csl_sort_key_free(&previous);
        previous = current;
    }
    CeCslSortDatePoint p = {2000,0,0};
    assert(ce_csl_sort_key_date(&p, NULL, NULL, &previous, error, sizeof error));
    for (unsigned season = 21; season <= 24; ++season) {
        CeCslSortKey s = {0}; p.month = season;
        assert(ce_csl_sort_key_date(&p, NULL, NULL, &s, error, sizeof error));
        assert(!compare(&s, &previous));
        ce_csl_sort_key_free(&s);
    }
    const CeCslSortDatePoint invalid[] = {
        {0,1,1}, {INT64_MIN,0,0}, {INT64_MAX,0,0}, {2000,13,0}, {2000,20,0},
        {2000,25,0}, {2000,0,1}, {2000,21,1}, {2000,1,32}};
    unsigned char *old = previous.data;
    for (size_t i = 0; i < sizeof invalid / sizeof *invalid; ++i) {
        assert(!ce_csl_sort_key_date(&invalid[i], NULL, NULL, &previous, error, sizeof error));
        assert(previous.data == old);
    }
    ce_csl_sort_key_free(&previous);
}
static void matrix(void) {
    CeCslSortKey a = integer("1"), b = integer("2");
    CeCslSortKeyView missing = {NULL,0,true};
    CeCslSortKeyView keys[] = {
        view(&b), view(&a),
        missing, view(&a),
        view(&a), view(&a),
        view(&a), view(&b),
        missing, view(&b),
        view(&a), view(&b)};
    bool descending[] = {false,true};
    CeCslSortOrder out = {0};
    assert(ce_csl_sort_order(keys, 6, 2, descending, NULL, &out, error, sizeof error));
    const size_t expected[] = {3,5,2,0,4,1};
    assert(out.count == 6 && !memcmp(out.indices, expected, sizeof expected));
    descending[0] = true;
    const size_t reverse[] = {0,3,5,2,4,1};
    assert(ce_csl_sort_order(keys, 6, 2, descending, NULL, &out, error, sizeof error));
    assert(!memcmp(out.indices, reverse, sizeof reverse));
    assert(ce_csl_sort_order(NULL, 6, 0, NULL, NULL, &out, error, sizeof error));
    for (size_t i = 0; i < 6; ++i) assert(out.indices[i] == i);
    /* Empty present binary keys and semantic missingness remain distinct. */
    CeCslSortKeyView empties[] = {missing, {NULL,0,false}, view(&a), {NULL,0,false}};
    assert(ce_csl_sort_order(empties, 4, 1, NULL, NULL, &out, error, sizeof error));
    const size_t empty_expected[] = {1,3,2,0};
    assert(!memcmp(out.indices, empty_expected, sizeof empty_expected));
    assert(ce_csl_sort_order(NULL, 0, 2, NULL, NULL, &out, error, sizeof error));
    assert(!out.count && !out.indices);
    ce_csl_sort_order_free(&out);
    ce_csl_sort_key_free(&a); ce_csl_sort_key_free(&b);
}

typedef struct Fault { size_t calls, fail_at, live; } Fault;
static void *allocate(void *context, size_t bytes) {
    Fault *f = context;
    if (++f->calls == f->fail_at) return NULL;
    void *p = malloc(bytes);
    if (p) ++f->live;
    return p;
}
static void deallocate(void *context, void *memory) {
    Fault *f = context;
    if (memory) { assert(f->live); --f->live; free(memory); }
}
static void failures(void) {
    CeCslSortKey key = integer("42");
    unsigned char *old_key = key.data;
    Fault f = {0,1,0};
    CeCslOutputAllocator allocator = {&f, allocate, deallocate};
    assert(!ce_csl_sort_key_integer("43", 2, &allocator, &key, error, sizeof error));
    assert(key.data == old_key && !f.live);
    CeCslSortOrder order = {0};
    assert(ce_csl_sort_order(NULL, 3, 0, NULL, NULL, &order, error, sizeof error));
    size_t *old_order = order.indices;
    CeCslSortKeyView keys[] = {view(&key),view(&key),view(&key)};
    for (size_t i = 1; i <= 2; ++i) {
        f = (Fault){0,i,0};
        assert(!ce_csl_sort_order(keys, 3, 1, NULL, &allocator, &order, error, sizeof error));
        assert(order.indices == old_order && order.count == 3 && !f.live);
    }
    f = (Fault){0,0,0};
    assert(ce_csl_sort_order(keys, 3, 1, NULL, &allocator, &order, error, sizeof error));
    assert(f.calls == 2 && f.live == 1);
    ce_csl_sort_order_free(&order); assert(!f.live);
    CeCslOutputAllocator bad = {NULL, allocate, NULL};
    assert(!ce_csl_sort_order(NULL, 0, 0, NULL, &bad, &order, error, sizeof error));
    assert(!ce_csl_sort_key_copy(NULL, 0, &bad, &key, error, sizeof error));
    assert(key.data == old_key);
    assert(!ce_csl_sort_order(keys, SIZE_MAX, SIZE_MAX, NULL, NULL, &order, error, sizeof error));
    keys[0].length = CE_CSL_SORT_MAX_KEY_BYTES + 1;
    assert(!ce_csl_sort_order(keys, 1, 1, NULL, NULL, &order, error, sizeof error));
    keys[0] = (CeCslSortKeyView){NULL,1,false};
    assert(!ce_csl_sort_order(keys, 1, 1, NULL, NULL, &order, error, sizeof error));
    keys[0] = (CeCslSortKeyView){key.data,1,true};
    assert(!ce_csl_sort_order(keys, 1, 1, NULL, NULL, &order, error, sizeof error));
    CeCslSortKeyView *many = calloc(129, sizeof *many);
    unsigned char *large = calloc(CE_CSL_SORT_MAX_KEY_BYTES, 1);
    assert(many && large);
    for (size_t i = 0; i < 129; ++i)
        many[i] = (CeCslSortKeyView){large,CE_CSL_SORT_MAX_KEY_BYTES,false};
    assert(!ce_csl_sort_order(many, 129, 1, NULL, NULL, &order, error, sizeof error));
    assert(error[0]); free(many); free(large);
    ce_csl_sort_key_free(&key);
}
static uint32_t random_state = 7;
static uint32_t random_number(void) {
    random_state = random_state * UINT32_C(1664525) + UINT32_C(1013904223);
    return random_state;
}
static int component_order(const CeCslSortKeyView *a, const CeCslSortKeyView *b) {
    size_t n = a->length < b->length ? a->length : b->length;
    int c = n ? memcmp(a->data,b->data,n) : 0;
    return c < 0 ? -1 : c > 0 ? 1 : a->length < b->length ? -1 : a->length > b->length ? 1 : 0;
}
static void sequences(void) {
    CeCslSortKey a = {0}, b = {0};
    CeCslSortKeyView first[] = {{(const unsigned char *)"ab",2,false},{(const unsigned char *)"c",1,false}};
    CeCslSortKeyView second[] = {{(const unsigned char *)"a",1,false},{(const unsigned char *)"bc",2,false}};
    assert(ce_csl_sort_key_sequence(first,2,NULL,&a,error,sizeof error));
    assert(ce_csl_sort_key_sequence(second,2,NULL,&b,error,sizeof error));
    assert(compare(&a,&b) > 0); /* Concatenating raw text would collide at abc. */
    for (size_t trial = 0; trial < 400; ++trial) {
        unsigned char aa[4][16], bb[4][16];
        CeCslSortKeyView av[4], bv[4];
        size_t ac = random_number()%5, bc = random_number()%5;
        for (size_t i = 0; i < 4; ++i) {
            for (size_t j = 0; j < 16; ++j) {
                aa[i][j] = (unsigned char)(random_number()%4);
                bb[i][j] = (unsigned char)(random_number()%4);
            }
            av[i] = (CeCslSortKeyView){aa[i],random_number()%17,false};
            bv[i] = (CeCslSortKeyView){bb[i],random_number()%17,false};
        }
        assert(ce_csl_sort_key_sequence(av,ac,NULL,&a,error,sizeof error));
        assert(ce_csl_sort_key_sequence(bv,bc,NULL,&b,error,sizeof error));
        int expected = 0;
        for (size_t i = 0; i < ac && i < bc && !expected; ++i) expected = component_order(&av[i],&bv[i]);
        if (!expected) expected = ac < bc ? -1 : ac > bc ? 1 : 0;
        assert(compare(&a,&b) == expected);
    }
    unsigned char zero_ff[] = {0,255,0};
    CeCslSortKeyView part = {zero_ff,sizeof zero_ff,false};
    assert(ce_csl_sort_key_sequence(&part,1,NULL,&a,error,sizeof error));
    const unsigned char expected_bytes[] = {0,255,255,0,255,0,0};
    assert(a.length == sizeof expected_bytes && !memcmp(a.data,expected_bytes,sizeof expected_bytes));
    CeCslSortKeyView alias = view(&a);
    assert(ce_csl_sort_key_sequence(&alias,1,NULL,&a,error,sizeof error));
    Fault f = {0,1,0}; CeCslOutputAllocator allocator = {&f,allocate,deallocate};
    unsigned char *old = a.data;
    assert(!ce_csl_sort_key_sequence(&part,1,&allocator,&a,error,sizeof error));
    assert(a.data == old && !f.live);
    part.missing = true;
    assert(!ce_csl_sort_key_sequence(&part,1,NULL,&a,error,sizeof error) && a.data == old);
    assert(!ce_csl_sort_key_sequence(&part,SIZE_MAX,NULL,&a,error,sizeof error) && a.data == old);
    CeCslSortKeyView *empty = calloc(CE_CSL_SORT_MAX_KEY_PARTS,sizeof *empty); assert(empty);
    assert(ce_csl_sort_key_sequence(empty,CE_CSL_SORT_MAX_KEY_PARTS,NULL,&a,error,sizeof error));
    assert(a.length == 2 * CE_CSL_SORT_MAX_KEY_PARTS);
    free(empty);
    unsigned char *boundary = malloc(CE_CSL_SORT_MAX_KEY_BYTES); assert(boundary);
    memset(boundary,1,CE_CSL_SORT_MAX_KEY_BYTES);
    part = (CeCslSortKeyView){boundary,CE_CSL_SORT_MAX_KEY_BYTES-2,false};
    assert(ce_csl_sort_key_sequence(&part,1,NULL,&a,error,sizeof error));
    assert(a.length == CE_CSL_SORT_MAX_KEY_BYTES);
    old = a.data; ++part.length;
    assert(!ce_csl_sort_key_sequence(&part,1,NULL,&a,error,sizeof error) && a.data == old);
    memset(boundary,0,CE_CSL_SORT_MAX_KEY_BYTES);
    part.length = (CE_CSL_SORT_MAX_KEY_BYTES-2)/2;
    assert(ce_csl_sort_key_sequence(&part,1,NULL,&a,error,sizeof error));
    assert(a.length == CE_CSL_SORT_MAX_KEY_BYTES);
    old = a.data; ++part.length;
    assert(!ce_csl_sort_key_sequence(&part,1,NULL,&a,error,sizeof error) && a.data == old);
    free(boundary); ce_csl_sort_key_free(&a); ce_csl_sort_key_free(&b);
}
static void randomized(void) {
    enum {ROWS=513, COLS=4};
    int values[ROWS][COLS]; bool absent[ROWS][COLS];
    CeCslSortKey (*owned)[COLS] = calloc(ROWS, sizeof *owned);
    CeCslSortKeyView *keys = calloc(ROWS * COLS, sizeof *keys);
    assert(owned && keys);
    bool descending[COLS] = {false,true,true,false};
    for (size_t i = 0; i < ROWS; ++i) for (size_t j = 0; j < COLS; ++j) {
        values[i][j] = (int)(random_number() % 21) - 10;
        absent[i][j] = random_number() % 5 == 0;
        char decimal[32]; snprintf(decimal, sizeof decimal, "%d", values[i][j]);
        owned[i][j] = integer(decimal);
        keys[i * COLS + j] = absent[i][j] ?
            (CeCslSortKeyView){NULL,0,true} : view(&owned[i][j]);
    }
    CeCslSortOrder order = {0};
    assert(ce_csl_sort_order(keys, ROWS, COLS, descending, NULL, &order, error, sizeof error));
    bool seen[ROWS] = {false};
    for (size_t i = 0; i < ROWS; ++i) {
        size_t row = order.indices[i]; assert(row < ROWS && !seen[row]); seen[row] = true;
        if (!i) continue;
        size_t previous = order.indices[i-1]; int relation = 0;
        /* Independent oracle compares original signed values, not encoded keys. */
        for (size_t j = 0; j < COLS && !relation; ++j) {
            if (absent[previous][j] != absent[row][j]) relation = absent[previous][j] ? 1 : -1;
            else if (!absent[row][j]) {
                relation = values[previous][j] < values[row][j] ? -1 :
                           values[previous][j] > values[row][j] ? 1 : 0;
                if (descending[j]) relation = -relation;
            }
        }
        assert(relation < 0 || (!relation && previous < row));
    }
    ce_csl_sort_order_free(&order);
    for (size_t i = 0; i < ROWS; ++i) for (size_t j = 0; j < COLS; ++j)
        ce_csl_sort_key_free(&owned[i][j]);
    free(owned); free(keys);
    CeCslSortKeyView *ties = calloc(CE_CSL_SORT_MAX_ROWS, sizeof *ties);
    assert(ties);
    assert(ce_csl_sort_order(ties, CE_CSL_SORT_MAX_ROWS, 1, NULL, NULL,
                             &order, error, sizeof error));
    for (size_t i = 0; i < order.count; ++i) assert(order.indices[i] == i);
    ce_csl_sort_order_free(&order); free(ties);
}
int main(void) {
    integers(); dates(); matrix(); failures(); sequences(); randomized();
    puts("CSL stable sort, exact integer/date keys, bounds and allocation failures passed");
    return 0;
}
