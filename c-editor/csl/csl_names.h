#ifndef CE_CSL_NAMES_H
#define CE_CSL_NAMES_H
#include "cJSON.h"
#include "csl_case.h"
#include "csl_locale.h"
#include "csl_output.h"

#define CE_CSL_NAMES_MAX_PEOPLE ((size_t)16384)
#define CE_CSL_NAMES_MAX_BYTES ((size_t)1024 * 1024)
#define CE_CSL_NAMES_MAX_BINDINGS ((size_t)64)

typedef enum CeCslNameForm { CE_CSL_NAME_LONG, CE_CSL_NAME_SHORT, CE_CSL_NAME_COUNT } CeCslNameForm;
typedef enum CeCslNameAnd { CE_CSL_NAME_AND_NONE, CE_CSL_NAME_AND_TEXT, CE_CSL_NAME_AND_SYMBOL } CeCslNameAnd;
typedef enum CeCslNameDelimiter {
    CE_CSL_NAME_CONTEXTUAL,
    CE_CSL_NAME_ALWAYS,
    CE_CSL_NAME_NEVER,
    CE_CSL_NAME_AFTER_INVERTED
} CeCslNameDelimiter;
typedef enum CeCslNameSort {
    CE_CSL_NAME_SORT_NONE,
    CE_CSL_NAME_SORT_FIRST,
    CE_CSL_NAME_SORT_ALL
} CeCslNameSort;
typedef enum CeCslNameDemote {
    CE_CSL_NAME_DEMOTE_NEVER,
    CE_CSL_NAME_DEMOTE_SORT,
    CE_CSL_NAME_DEMOTE_DISPLAY
} CeCslNameDemote;
typedef enum CeCslGivenExpansion {
    CE_CSL_GIVEN_STYLE,
    CE_CSL_GIVEN_INITIALS,
    CE_CSL_GIVEN_FULL
} CeCslGivenExpansion;
typedef enum CeCslNameKind { CE_CSL_NAME_EMPTY, CE_CSL_NAME_PERSONAL, CE_CSL_NAME_LITERAL } CeCslNameKind;

typedef struct CeCslNameOrigin {
    const char *item_id, *variable;
} CeCslNameOrigin;
typedef struct CeCslNameIdentity {
    CeCslNameKind kind;
    const char *family, *given, *dropping_particle, *non_dropping_particle, *suffix, *literal;
    bool comma_suffix, static_ordering, parse_names;
    bool parse_names_present;
} CeCslNameIdentity;
typedef struct CeCslNamesData CeCslNamesData;

/* Strict semantic parser shared with the variable resolver. NULL means absent.
 * Strings are exact UTF-8 (no trim/fold/normalization); absent and empty strings
 * are equivalent. Flags accept bool, 0/1 and exact true/false/0/1 strings.
 * Object key order is immaterial; array order and empty placeholders matter.
 * Empty objects are allowed and do not count as nonempty. A nonempty personal
 * name requires family; nonempty literal plus personal fields is ambiguous.
 * Unknown fields and invalid types are errors, not silently ignored. These
 * helpers validate explicit parse-names but do not execute automatic parsing.
 * Failure preserves the output value. No allocation or input mutation. */
bool ce_csl_names_inspect(const cJSON *array, size_t *nonempty_count, char *error, size_t capacity);
bool ce_csl_names_equal(const cJSON *a, const cJSON *b, bool *equal, char *error, size_t capacity);
bool ce_csl_name_identity_equal(const CeCslNameIdentity *a, const CeCslNameIdentity *b);

/* Owned immutable data, including copied origin strings. Atomic replacement of
 * an initialized NULL/live *out. Results retain data using a non-atomic internal
 * reference count; data/results must not be accessed concurrently without the
 * caller's synchronization. Allocator context outlives data AND its results. */
bool ce_csl_names_data_load(const cJSON *array, const CeCslNameOrigin *source,
                            const CeCslOutputAllocator *allocator, CeCslNamesData **out, char *error,
                            size_t capacity);
void ce_csl_names_data_free(CeCslNamesData *data);
size_t ce_csl_names_data_count(const CeCslNamesData *data);
size_t ce_csl_names_data_nonempty_count(const CeCslNamesData *data);
const CeCslNameIdentity *ce_csl_names_data_person(const CeCslNamesData *data, size_t index);
const CeCslNameOrigin *ce_csl_names_data_origin(const CeCslNamesData *data);

typedef struct CeCslNameOptions {
    CeCslNameForm form;
    CeCslNameAnd and;
    CeCslNameDelimiter precedes_last, precedes_et_al;
    CeCslNameSort sort;
    CeCslNameDemote demote;
    bool initialize, initialize_with_hyphen, et_al_use_last;
    const char *initialize_with; /* NULL means unset; empty string is meaningful. */
    const char *name_delimiter, *names_delimiter, *sort_separator;
    size_t et_al_min, et_al_use_first, subsequent_min, subsequent_use_first;
    bool has_et_al_min, has_et_al_use_first, has_subsequent_min, has_subsequent_use_first;
    const CeCslXmlNode *names, *name, *et_al, *label, *given_part, *family_part, *substitute;
} CeCslNameOptions;

/* Resolves style -> citation/bibliography -> local name options. names delimiter
 * and formatting have distinct scopes. Only a childless names used under
 * substitute inherits parent name/et-al settings. Borrowed XML/strings must
 * outlive the options. NULL style/section are permitted. All local syntax is
 * checked even when data is absent. display belongs to the evaluator and is
 * rejected by this standalone renderer. Failure leaves *out unchanged. */
bool ce_csl_names_resolve(const CeCslXmlNode *style, const CeCslXmlNode *section, const CeCslXmlNode *names,
                          const CeCslNameOptions *substitute_parent, CeCslNameOptions *out, char *error,
                          size_t capacity);

typedef struct CeCslNameOverride {
    size_t binding, person_index;
    CeCslGivenExpansion given;
} CeCslNameOverride;
typedef struct CeCslNameRenderContext {
    unsigned base_format;
    const char *item_language, *style_default_locale;
    const CeCslOutputAllocator *allocator;
    bool subsequent, sorting;
    /* Zero means no disambiguation increase. Includes only the prefix, not
     * et-al-use-last; never reduces the style's displayed prefix. */
    size_t minimum_names;
    const CeCslNameOverride *overrides;
    size_t override_count;
    /* Explicit cs:key overrides; zeros are values, not absence sentinels. */
    bool has_sort_min, has_sort_first, has_sort_last;
    size_t sort_min, sort_first;
    bool sort_last;
} CeCslNameRenderContext;
typedef struct CeCslNamesBinding {
    const char *variable;
    CeCslNamesData *data;
} CeCslNamesBinding;
typedef struct CeCslNameRendered {
    size_t binding, person_index;
    const CeCslNameIdentity *identity; /* Kept alive by the result. */
    CeCslGivenExpansion given;         /* Requested override; STYLE means original options. */
    bool inverted, given_visible, given_initialized;
    size_t output_offset, output_length; /* Plain UTF-8 bytes within result. */
} CeCslNameRendered;
typedef struct CeCslNameSortView {
    CeCslNameKind kind;
    const char *primary_family, *secondary_particles, *given, *suffix, *literal;
    size_t primary_family_length, secondary_particles_length, given_length, suffix_length, literal_length;
} CeCslNameSortView;
typedef struct CeCslNamesSummary {
    size_t total_names, rendered_names;
    bool rendered, truncated, used_last, collapsed_editor_translator;
    bool missing_term, needs_substitute;
} CeCslNamesSummary;
typedef struct CeCslNamesResult CeCslNamesResult;

/* Each result owns controlled output IR and metadata, retaining its input data.
 * No rendering/evaluator callback is invoked. needs_substitute tells the caller
 * to evaluate options.substitute children using its own variable transaction;
 * only the winning candidate's output/variable suppression should be committed.
 * Variable lookup (including derived editor-translator), disambiguation search,
 * registry state, bibliography repeated-author substitution and collation are
 * evaluator responsibilities. Identity equality never uses display strings.
 *
 * Person form is long/short; count belongs to the list renderer. Single-person
 * rendering excludes names/name list affixes, delimiters and role labels.
 * Lists accept ordered bindings (one per requested variable), combine identical
 * editor+translator selections, and provide count after truncation/collapsing.
 * Explicit parse-names=true is unsupported; particles must be structured.
 * CJK-family scripts use family-given order; mixed-script parts use a separating
 * space. Other scripts follow normal order unless static-ordering is explicit.
 * No markup parsing, transliteration, locale collation or real-person entity
 * resolution. First-scalar full title mapping includes following combining marks.
 * Data/output are bounded per entry; limits never silently truncate names.
 * Success replaces *out atomically; all failures preserve *out and inputs. */
bool ce_csl_name_render(const CeCslNamesData *data, size_t index, const CeCslNameOptions *options,
                        const CeCslLocale *locale, const CeCslNameRenderContext *context,
                        CeCslNamesResult **out, char *error, size_t capacity);
bool ce_csl_names_render(const CeCslNamesBinding *bindings, size_t count, const CeCslNameOptions *options,
                         const CeCslLocale *locale, const CeCslNameRenderContext *context,
                         CeCslNamesResult **out, char *error, size_t capacity);
void ce_csl_names_result_free(CeCslNamesResult *result);
const CeCslOutput *ce_csl_names_result_output(const CeCslNamesResult *result);
const CeCslNamesSummary *ce_csl_names_result_summary(const CeCslNamesResult *result);
size_t ce_csl_names_result_person_count(const CeCslNamesResult *result);
const CeCslNameRendered *ce_csl_names_result_person(const CeCslNamesResult *result, size_t index);
/* Components of the corresponding rendered person, owned by result. Every
 * field is non-NULL, NUL-terminated exact UTF-8 with its byte length. Personal
 * primary_family contains family plus a non-demoted non-dropping particle;
 * secondary_particles contains dropping then any demoted non-dropping particle.
 * given reflects the actual initialization/expansion and name-part text case.
 * Short form hides given/secondary_particles/suffix. Literal names retain the
 * original literal text (including articles); other fields are empty. Affixes,
 * labels, display separators and font decorations are not included. The view is not a
 * collation key or an identity; use the original identity for equality. Count
 * form, absent persons and out-of-range indices return NULL. */
const CeCslNameSortView *ce_csl_names_result_sort_person(const CeCslNamesResult *result, size_t index);
/* All sources, including omitted names, remain queryable for disambiguation. */
const CeCslNamesData *ce_csl_names_result_data(const CeCslNamesResult *result, size_t binding);
#endif
