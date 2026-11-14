#ifndef CE_CSL_DATE_H
#define CE_CSL_DATE_H

#include "cJSON.h"
#include "csl_locale.h"
#include "csl_output.h"

#define CE_CSL_DATE_MAX_YEAR INT64_C(999999999)

typedef struct CeCslDateOptions {
    unsigned base_format;
    const char *item_language;
    const char *style_default_locale;
    const CeCslOutputAllocator *allocator;
} CeCslDateOptions;
typedef struct CeCslDateResult {
    bool rendered;
    bool missing_term;
    bool circa; /* Data flag for is-uncertain-date; never an automatic prefix. */
} CeCslDateResult;
typedef struct CeCslDateInfo {
    bool present;
    bool circa;
} CeCslDateInfo;

/* Read-only facts through the same parser used by rendering. Presence means
 * a nonempty literal or at least one nonzero endpoint year, independent of any
 * style's selected date-parts. circa retains the data flag, even when no date
 * content is present. No allocation; failure preserves *info. The supported
 * input syntax and explicit limits are the same as ce_csl_date_render below. */
bool ce_csl_date_inspect(const cJSON *value, CeCslDateInfo *info,
    char *error, size_t capacity);

/* Independent CSL 1.0.2 date renderer, not a citation evaluator. value is one
 * borrowed CSL-JSON date object from the strict input layer (NUL-free strings),
 * or NULL for an absent variable. date_node is a
 * borrowed cs:date rendering element. No I/O, calendar conversion or host locale.
 * Localized forms use the resolver's date ordering/affixes; date-part overrides
 * cannot change that ordering and cannot have affixes. Formatting is final IR
 * runs; a part's affixes inherit the date format, the date's affixes base_format.
 *
 * Structured dates accept integral JSON numbers or signed decimal year strings,
 * years +/-MAX_YEAR, month 1..12 or EDTF season codes 21..24, and day 1..31.
 * Zero/empty trailing parts are absent; a zero year is an open range endpoint
 * (or empty single date). Months/days are not normalized or inferred. Explicit
 * season accepts 1..4, without a simultaneous month/day, on a single endpoint.
 * literal is exact plain text. Nonempty literal plus structured parts is an
 * ambiguous input error. Structured parts take precedence over accompanying raw
 * metadata. With no structured parts/literal, raw supports only the strict
 * YYYY[-MM[-DD]] subset (four-digit years, optional leading minus), optional
 * slash interval, and '..' open endpoints. Other EDTF/natural-language syntax
 * returns an explicit unsupported error; raw is never guessed or silently
 * converted to literal. circa accepts booleans, 0/1, or 'true'/'false'/'0'/'1'.
 *
 * Equal-precision ranges suppress shared larger components and use the largest
 * differing selected component's range-delimiter (default en dash). Identical
 * selected component values produce a single date. Open ranges are supported. Mixed
 * endpoint precision is currently explicitly unsupported. Year zero has no
 * calendar interpretation. Year short uses the final two magnitude digits;
 * negative years append bc, positive years below 1000 append ad, both exactly
 * as supplied by the locale (including its spacing). Missing terms produce
 * empty text/suffix and set missing_term; explicit empty terms do not.
 *
 * display is the evaluator's responsibility and is rejected here if present.
 * Date-part and whole-date text-case preserve source formatting through the
 * shared case layer's scalar map, including expansions and contextual rules.
 * Date-part ordering, formatting and all output strings are validated even for
 * an absent value. Output is bounded by csl_output's per-entry limits.
 *
 * NULL options selects defaults. allocator covers temporary rendering work;
 * destination keeps its own allocator. All inputs remain borrowed/unchanged.
 * Success atomically APPENDS to destination and then replaces optional result.
 * Any failure (including allocation failure) preserves destination and result.
 */
bool ce_csl_date_render(const cJSON *value, const CeCslXmlNode *date_node,
    const CeCslLocale *locale, const CeCslDateOptions *options,
    CeCslOutput *destination, CeCslDateResult *result,
    char *error, size_t capacity);

#endif
