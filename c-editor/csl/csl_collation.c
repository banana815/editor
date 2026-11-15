#include "csl_collation.h"
#include "csl_unicode.h"
#include <mojibake.h>
#include <stdio.h>
#include <utf8proc.h>

static bool fail(char *error, size_t capacity, const char *message) {
    if (error && capacity) snprintf(error, capacity, "%s", message);
    return false;
}

/* Count the actual canonical decomposition, not just source combining marks:
 * one scalar can expand to multiple nonstarters. No allocation or reordering
 * occurs here, so the check also precedes the normalizer's work. */
static bool preflight(const char *text, size_t size, char *error, size_t capacity) {
    if ((!text && size) || size > CE_CSL_COLLATION_MAX_TEXT_BYTES)
        return fail(error, capacity, "Collation text exceeds its 64 KiB limit or is invalid");
    size_t offset = 0, run = 0;
    while (offset < size) {
        uint32_t cp;
        if (ce_csl_unicode_next(text, size, &offset, &cp) != CE_CSL_UNICODE_OK)
            return fail(error, capacity, "Malformed UTF-8 in collation text");
        if (!cp || (cp < 32 && cp != '\t' && cp != '\r' && cp != '\n'))
            return fail(error, capacity, "NUL or unsupported control in collation text");
        utf8proc_int32_t decomposed[4];
        utf8proc_ssize_t count = utf8proc_decompose_char((utf8proc_int32_t)cp,
            decomposed, 4, UTF8PROC_DECOMPOSE | UTF8PROC_STABLE, NULL);
        if (count < 0 || count > 4)
            return fail(error, capacity, "Unsupported canonical decomposition in collation text");
        for (utf8proc_ssize_t i = 0; i < count; ++i) {
            if (utf8proc_get_property(decomposed[i])->combining_class) {
                if (++run > CE_CSL_COLLATION_MAX_NONSTARTERS)
                    return fail(error, capacity, "Collation canonical sequence exceeds 64 nonstarters");
            } else run = 0;
        }
    }
    return true;
}

static bool unicode_error(CeCslUnicodeStatus status, char *error, size_t capacity) {
    if (status == CE_CSL_UNICODE_OOM)
        return fail(error, capacity, "Out of memory preparing Unicode collation text");
    if (status == CE_CSL_UNICODE_LIMIT)
        return fail(error, capacity, "Collation folded/NFD text or Unicode workspace exceeds its limit");
    return fail(error, capacity, "Cannot prepare Unicode collation text");
}

bool ce_csl_collation_key(const char *text, size_t size,
    CeCslCollationLanguage language, const CeCslOutputAllocator *allocator,
    CeCslSortKey *out, char *error, size_t capacity) {
    if (error && capacity) *error = 0;
    if (!out || language < CE_CSL_COLLATION_DEFAULT || language > CE_CSL_COLLATION_AZ ||
        (allocator && (!allocator->allocate || !allocator->deallocate)))
        return fail(error, capacity, "Invalid collation output, language, or allocator");
    if (!preflight(text, size, error, capacity)) return false;

    CeCslUnicodeOptions options = {0};
    options.max_input_bytes = CE_CSL_COLLATION_MAX_TEXT_BYTES;
    options.max_output_bytes = CE_CSL_COLLATION_MAX_TEXT_BYTES;
    options.max_workspace_bytes = CE_CSL_COLLATION_MAX_WORKSPACE_BYTES;
    if (allocator) {
        options.allocate = allocator->allocate;
        options.deallocate = allocator->deallocate;
        options.allocator_context = allocator->context;
    }
    const char *fold_language = language == CE_CSL_COLLATION_TR ? "tr" :
                                language == CE_CSL_COLLATION_AZ ? "az" : NULL;
    CeCslUnicodeText folded = {0}, normalized = {0};
    mjb_result temporary = {0};
    bool ok = false;
    CeCslUnicodeStatus status = ce_csl_unicode_transform(text, size,
        CE_CSL_UNICODE_CASEFOLD, fold_language, &options, &folded);
    if (status != CE_CSL_UNICODE_OK) {
        unicode_error(status, error, capacity); goto cleanup;
    }
    if (!preflight(folded.data, folded.size, error, capacity)) goto cleanup;
    status = ce_csl_unicode_transform(folded.data, folded.size,
        CE_CSL_UNICODE_NFD, NULL, &options, &normalized);
    if (status != CE_CSL_UNICODE_OK) {
        unicode_error(status, error, capacity); goto cleanup;
    }
    /* This NFD prepass is required: the pinned library flushes its own
     * normalization buffer after 32 scalars. Already-NFD input takes its
     * quick-check path and avoids cross-buffer canonical ordering errors. */
    mjb_text input = mjb_text_utf8(normalized.data, normalized.size);
    mjb_status result = mjb_collation_key(&input, MJB_MALFORMED_STOP,
        MJB_COLLATION_NON_IGNORABLE, MJB_COLLATION_SECONDARY, &temporary, NULL);
    if (result != MJB_STATUS_OK) {
        fail(error, capacity, result == MJB_STATUS_NO_MEMORY ?
            "Out of memory generating DUCET collation key" : "Cannot generate DUCET collation key");
        goto cleanup;
    }
    if (temporary.output_size > CE_CSL_COLLATION_MAX_KEY_BYTES) {
        fail(error, capacity, "Collation key exceeds its 512 KiB limit"); goto cleanup;
    }
    ok = ce_csl_sort_key_copy((const unsigned char *)temporary.output,
        temporary.output_size, allocator, out, error, capacity);
cleanup:
    (void)mjb_result_free(&temporary);
    ce_csl_unicode_text_free(&normalized);
    ce_csl_unicode_text_free(&folded);
    return ok;
}
