#include "csl_sort.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static bool fail(char *error, size_t capacity, const char *message) {
    if (error && capacity) snprintf(error, capacity, "%s", message);
    return false;
}
static void *default_allocate(void *context, size_t bytes) {
    (void)context;
    return malloc(bytes);
}
static void default_deallocate(void *context, void *memory) {
    (void)context;
    free(memory);
}
static bool allocator_get(const CeCslOutputAllocator *input,
                          CeCslOutputAllocator *out) {
    if (input) {
        if (!input->allocate || !input->deallocate) return false;
        *out = *input;
    } else {
        out->context = NULL;
        out->allocate = default_allocate;
        out->deallocate = default_deallocate;
    }
    return true;
}
void ce_csl_sort_key_free(CeCslSortKey *key) {
    if (!key) return;
    if (key->data && key->_deallocate) key->_deallocate(key->_context, key->data);
    memset(key, 0, sizeof *key);
}
void ce_csl_sort_order_free(CeCslSortOrder *order) {
    if (!order) return;
    if (order->indices && order->_deallocate)
        order->_deallocate(order->_context, order->indices);
    memset(order, 0, sizeof *order);
}
bool ce_csl_sort_key_copy(const unsigned char *bytes, size_t size,
    const CeCslOutputAllocator *allocator, CeCslSortKey *out,
    char *error, size_t capacity) {
    if (error && capacity) *error = 0;
    CeCslOutputAllocator a;
    if (!out || (!bytes && size) || !allocator_get(allocator, &a))
        return fail(error, capacity, "Invalid CSL sort key argument");
    if (size > CE_CSL_SORT_MAX_KEY_BYTES)
        return fail(error, capacity, "CSL sort key byte limit exceeded");
    unsigned char *copy = size ? a.allocate(a.context, size) : NULL;
    if (size && !copy) return fail(error, capacity, "Cannot allocate CSL sort key");
    if (size) memcpy(copy, bytes, size);
    ce_csl_sort_key_free(out);
    *out = (CeCslSortKey){copy, size, a.deallocate, a.context};
    return true;
}

static int rows_compare(const CeCslSortKeyView *keys, size_t columns,
                        const bool *descending, size_t left, size_t right) {
    const CeCslSortKeyView *a = keys + left * columns;
    const CeCslSortKeyView *b = keys + right * columns;
    for (size_t column = 0; column < columns; ++column) {
        if (a[column].missing != b[column].missing)
            return a[column].missing ? 1 : -1;
        if (a[column].missing) continue;
        size_t length = a[column].length < b[column].length ?
                        a[column].length : b[column].length;
        int compare = length ? memcmp(a[column].data, b[column].data, length) : 0;
        compare = compare < 0 ? -1 : compare > 0 ? 1 :
                  a[column].length < b[column].length ? -1 :
                  a[column].length > b[column].length ? 1 : 0;
        if (compare) return descending && descending[column] ? -compare : compare;
    }
    return 0;
}
bool ce_csl_sort_order(const CeCslSortKeyView *keys, size_t rows, size_t columns,
    const bool *descending, const CeCslOutputAllocator *allocator,
    CeCslSortOrder *out, char *error, size_t capacity) {
    if (error && capacity) *error = 0;
    CeCslOutputAllocator a;
    if (!out || !allocator_get(allocator, &a) || (rows && columns && !keys))
        return fail(error, capacity, "Invalid CSL sort matrix argument");
    if (rows > CE_CSL_SORT_MAX_ROWS || columns > CE_CSL_SORT_MAX_COLUMNS)
        return fail(error, capacity, "CSL sort matrix size limit exceeded");
    size_t total = 0;
    for (size_t i = 0; i < rows * columns; ++i) {
        if ((keys[i].missing && keys[i].length) ||
            (!keys[i].data && keys[i].length))
            return fail(error, capacity, "Invalid CSL sort key view");
        if (keys[i].length > CE_CSL_SORT_MAX_KEY_BYTES ||
            keys[i].length > CE_CSL_SORT_MAX_TOTAL_KEY_BYTES - total)
            return fail(error, capacity, "CSL sort matrix byte limit exceeded");
        total += keys[i].length;
    }
    size_t *indices = rows ? a.allocate(a.context, rows * sizeof *indices) : NULL;
    if (rows && !indices) return fail(error, capacity, "Cannot allocate CSL sort order");
    size_t *scratch = rows > 1 && columns ?
                      a.allocate(a.context, rows * sizeof *scratch) : NULL;
    if (rows > 1 && columns && !scratch) {
        a.deallocate(a.context, indices);
        return fail(error, capacity, "Cannot allocate CSL sort workspace");
    }
    for (size_t i = 0; i < rows; ++i) indices[i] = i;
    if (scratch) {
        for (size_t width = 1; width < rows; width *= 2) {
            for (size_t base = 0; base < rows; base += 2 * width) {
                size_t middle = base + width < rows ? base + width : rows;
                size_t end = middle + width < rows ? middle + width : rows;
                size_t left = base, right = middle, target = base;
                while (left < middle && right < end) {
                    if (rows_compare(keys, columns, descending,
                                     indices[left], indices[right]) <= 0)
                        scratch[target++] = indices[left++];
                    else scratch[target++] = indices[right++];
                }
                while (left < middle) scratch[target++] = indices[left++];
                while (right < end) scratch[target++] = indices[right++];
            }
            size_t *swap = indices;
            indices = scratch;
            scratch = swap;
        }
        a.deallocate(a.context, scratch);
    }
    ce_csl_sort_order_free(out);
    *out = (CeCslSortOrder){indices, rows, a.deallocate, a.context};
    return true;
}

static void store_u32(unsigned char *out, uint32_t number) {
    out[0] = (unsigned char)(number >> 24);
    out[1] = (unsigned char)(number >> 16);
    out[2] = (unsigned char)(number >> 8);
    out[3] = (unsigned char)number;
}
bool ce_csl_sort_key_integer(const char *decimal, size_t length,
    const CeCslOutputAllocator *allocator, CeCslSortKey *out,
    char *error, size_t capacity) {
    if (error && capacity) *error = 0;
    if (!decimal || !length || !out)
        return fail(error, capacity, "Invalid CSL integer sort argument");
    bool negative = decimal[0] == '-';
    size_t offset = negative || decimal[0] == '+' ? 1 : 0;
    if (length == offset || length - offset > CE_CSL_SORT_MAX_INTEGER_DIGITS)
        return fail(error, capacity, "CSL integer sort digit limit or empty value");
    for (size_t i = offset; i < length; ++i)
        if (decimal[i] < '0' || decimal[i] > '9')
            return fail(error, capacity, "CSL integer sort requires decimal digits");
    while (offset < length && decimal[offset] == '0') ++offset;
    unsigned char encoded[CE_CSL_SORT_MAX_INTEGER_DIGITS + 5];
    size_t digits = length - offset;
    encoded[0] = digits ? (negative ? 0 : 2) : 1;
    if (!digits) return ce_csl_sort_key_copy(encoded, 1, allocator, out, error, capacity);
    store_u32(encoded + 1, (uint32_t)digits);
    memcpy(encoded + 5, decimal + offset, digits);
    if (negative)
        for (size_t i = 1; i < digits + 5; ++i) encoded[i] = (unsigned char)~encoded[i];
    return ce_csl_sort_key_copy(encoded, digits + 5, allocator, out, error, capacity);
}
static bool date_point(const CeCslSortDatePoint *point, unsigned char *encoded) {
    if (!point || !point->year || point->year < INT64_C(-999999999) ||
        point->year > INT64_C(999999999) || point->day > 31) return false;
    unsigned month = point->month;
    if (month >= 21 && month <= 24) month = 0;
    else if (month > 12) return false;
    if (point->day && !month) return false;
    store_u32(encoded, (uint32_t)(point->year + INT64_C(999999999)));
    encoded[4] = (unsigned char)month;
    encoded[5] = (unsigned char)point->day;
    return true;
}
bool ce_csl_sort_key_date(const CeCslSortDatePoint *start,
    const CeCslSortDatePoint *end, const CeCslOutputAllocator *allocator,
    CeCslSortKey *out, char *error, size_t capacity) {
    if (error && capacity) *error = 0;
    unsigned char encoded[13];
    if (!out || !date_point(start, encoded) || (end && !date_point(end, encoded + 7)))
        return fail(error, capacity, "Invalid or unsupported CSL sort date");
    encoded[6] = end ? 1 : 0;
    return ce_csl_sort_key_copy(encoded, end ? 13 : 7, allocator, out, error, capacity);
}
