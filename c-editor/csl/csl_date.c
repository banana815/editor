#include "csl_date.h"
#include "csl_case.h"
#include "csl_format.h"
#include "csl_number.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

/* Rules: https://docs.citationstyles.org/en/stable/specification.html#date
 * Input model: citation-style-language/schema schemas/input/csl-data.json,
 * Git blob ab52447266f77d4794830da35f89e4cc6cb0f1df. No processor source is used. */
enum { YEAR, MONTH, DAY };
typedef struct Endpoint { int64_t part[3]; bool season; } Endpoint;
typedef struct DateValue {
    Endpoint end[2]; unsigned count; const char *literal; bool circa;
} DateValue;
typedef struct DatePart {
    int kind;
    const CeCslXmlNode *base, *override;
    unsigned format;
    const char *form, *prefix, *suffix, *range;
    CeCslTextCase text_case;
    bool strip;
} DatePart;
typedef struct DatePlan {
    DatePart parts[3]; size_t count; unsigned format;
    const char *delimiter;
    CeCslTextCase text_case;
} DatePlan;
typedef struct Render {
    const CeCslLocale *locale;
    const CeCslDateOptions *options;
    CeCslDateResult result;
    char *error; size_t capacity;
} Render;

static bool fail(char *error, size_t capacity, const char *message) {
    if (error && capacity) snprintf(error, capacity, "CSL date: %s", message);
    return false;
}
static bool equal(const char *a, const char *b) { return a && !strcmp(a, b); }
static const char *attr(const CeCslXmlNode *node, const char *name) {
    return ce_csl_xml_attribute(node, name);
}
static const char *part_attr(const DatePart *part, const char *name) {
    const char *value = attr(part->override, name);
    return value ? value : attr(part->base, name);
}
static const char *or_empty(const char *s) { return s ? s : ""; }
static int part_kind(const char *name) {
    return equal(name, "year") ? YEAR : equal(name, "month") ? MONTH : equal(name, "day") ? DAY : -1;
}
static bool whitespace(const char *s) {
    if (!s) return true;
    for (; *s; ++s) if (*s != ' ' && *s != '\t' && *s != '\r' && *s != '\n') return false;
    return true;
}
static bool case_mode(const char *value, CeCslTextCase *mode, Render *r) {
    if (!value) *mode = CE_CSL_CASE_NONE;
    else if (!strcmp(value, "lowercase")) *mode = CE_CSL_CASE_LOWER;
    else if (!strcmp(value, "uppercase")) *mode = CE_CSL_CASE_UPPER;
    else if (!strcmp(value, "capitalize-first")) *mode = CE_CSL_CASE_CAPITALIZE_FIRST;
    else if (!strcmp(value, "capitalize-all")) *mode = CE_CSL_CASE_CAPITALIZE_ALL;
    else if (!strcmp(value, "sentence")) *mode = CE_CSL_CASE_SENTENCE;
    else if (!strcmp(value, "title")) *mode = CE_CSL_CASE_TITLE;
    else return fail(r->error, r->capacity, "invalid text-case");
    return true;
}
static bool style_bool(const char *s, bool *out) {
    if (!s || equal(s, "false") || equal(s, "0")) { *out = false; return true; }
    if (equal(s, "true") || equal(s, "1")) { *out = true; return true; }
    return false;
}
static bool formatting_attr(const char *name) {
    return equal(name,"font-style") || equal(name,"font-weight") || equal(name,"font-variant") ||
        equal(name,"text-decoration") || equal(name,"vertical-align");
}
static bool validate_node(const CeCslXmlNode *node, bool part, bool localized_override, Render *r) {
    size_t i;
    unsigned format;
    CeCslTextCase mode;
    bool flag;
    const char *form = attr(node, "form");
    int kind = part_kind(attr(node, "name"));
    if (!ce_csl_xml_is(node, part ? "date-part" : "date") || !whitespace(node->text))
        return fail(r->error,r->capacity,"invalid date element or mixed text");
    if (part && (kind < 0 || node->children)) return fail(r->error,r->capacity,"invalid date-part name/children");
    for (i = 0; i < node->attribute_count; ++i) {
        const char *name = node->attributes[i].name;
        if (formatting_attr(name) || equal(name,"text-case") || equal(name,"form")) continue;
        if (equal(name,"prefix") || equal(name,"suffix")) {
            if (localized_override) return fail(r->error,r->capacity,"localized date-part overrides cannot have affixes");
            continue;
        }
        if (part && (equal(name,"name") || equal(name,"range-delimiter") || (kind == MONTH && equal(name,"strip-periods")))) continue;
        if (!part && (equal(name,"variable") || equal(name,"date-parts") || equal(name,"delimiter"))) continue;
        return fail(r->error,r->capacity, equal(name,"display") ? "display requires the layout evaluator" : "unsupported date attribute");
    }
    if (!ce_csl_format_resolve(node,0,&format,r->error,r->capacity) || !case_mode(attr(node,"text-case"),&mode,r)) return false;
    if (!style_bool(attr(node,"strip-periods"),&flag)) return fail(r->error,r->capacity,"invalid strip-periods boolean");
    if (part && form) {
        bool valid = kind == YEAR ? (equal(form,"long") || equal(form,"short")) :
            kind == MONTH ? (equal(form,"long") || equal(form,"short") || equal(form,"numeric") || equal(form,"numeric-leading-zeros")) :
            (equal(form,"numeric") || equal(form,"numeric-leading-zeros") || equal(form,"ordinal"));
        if (!valid) return fail(r->error,r->capacity,"invalid date-part form");
    } else if (!part && form && !equal(form,"text") && !equal(form,"numeric")) {
        return fail(r->error,r->capacity,"invalid localized date form");
    }
    return true;
}
static bool make_plan(const CeCslXmlNode *node, DatePlan *plan, Render *r) {
    const char *form = attr(node,"form"), *limit = attr(node,"date-parts"), *text_case;
    const CeCslXmlNode *source = node, *child, *overrides[3] = {0};
    bool seen[3] = {0};
    int last = DAY;
    size_t count = 0;
    if (!validate_node(node,false,false,r)) return false;
    if (form && attr(node,"delimiter")) return fail(r->error,r->capacity,"localized date calls cannot override the locale delimiter");
    if (limit) {
        if (!form) return fail(r->error,r->capacity,"date-parts requires a localized form");
        if (equal(limit,"year")) last = YEAR;
        else if (equal(limit,"year-month")) last = MONTH;
        else if (!equal(limit,"year-month-day")) return fail(r->error,r->capacity,"invalid date-parts");
    }
    for (child = node->children; child; child = child->next) {
        int kind;
        if (!validate_node(child,true,form != NULL,r)) return false;
        kind = part_kind(attr(child,"name"));
        if (overrides[kind]) return fail(r->error,r->capacity,"duplicate date-part");
        overrides[kind] = child;
    }
    if (form) {
        source = ce_csl_locale_date(r->locale,equal(form,"text") ? CE_CSL_LOCALE_DATE_TEXT : CE_CSL_LOCALE_DATE_NUMERIC);
        if (!source) return fail(r->error,r->capacity,"localized date format is unavailable");
        if (!validate_node(source,false,false,r)) return false;
    }
    if (!ce_csl_format_resolve(form ? source : NULL,r->options->base_format,&plan->format,r->error,r->capacity) ||
        !ce_csl_format_resolve(node,plan->format,&plan->format,r->error,r->capacity)) return false;
    text_case = attr(node,"text-case");
    if (!text_case && form) text_case = attr(source,"text-case");
    if (!case_mode(text_case,&plan->text_case,r)) return false;
    plan->delimiter = attr(node,"delimiter");
    if (!plan->delimiter) plan->delimiter = or_empty(attr(source,"delimiter"));
    for (child = source->children; child; child = child->next) {
        DatePart p = {0};
        if (!validate_node(child,true,false,r)) return false;
        p.kind = part_kind(attr(child,"name"));
        if (seen[p.kind]) return fail(r->error,r->capacity,"duplicate localized date-part");
        seen[p.kind] = true;
        ++count;
        p.base = child;
        p.override = form ? overrides[p.kind] : NULL;
        p.form = part_attr(&p,"form");
        p.prefix = or_empty(part_attr(&p,"prefix"));
        p.suffix = or_empty(part_attr(&p,"suffix"));
        p.range = part_attr(&p,"range-delimiter");
        if (!p.range) p.range = "–";
        if (!case_mode(part_attr(&p,"text-case"),&p.text_case,r) ||
            !style_bool(part_attr(&p,"strip-periods"),&p.strip)) return false;
        if (!ce_csl_format_resolve(p.base,plan->format,&p.format,r->error,r->capacity) ||
            !ce_csl_format_resolve(p.override,p.format,&p.format,r->error,r->capacity)) return false;
        if (p.kind <= last) plan->parts[plan->count++] = p;
    }
    if (!count) return fail(r->error,r->capacity,"date has no date-parts");
    return true;
}

static bool integer(const cJSON *node, int64_t *value) {
    if (cJSON_IsNumber(node)) {
        double n = node->valuedouble;
        if (!isfinite(n) || n < -(double)CE_CSL_DATE_MAX_YEAR || n > (double)CE_CSL_DATE_MAX_YEAR || floor(n) != n) return false;
        *value = (int64_t)n; return true;
    }
    if (cJSON_IsString(node) && node->valuestring) {
        const unsigned char *p = (const unsigned char *)node->valuestring;
        int sign = 1; int64_t n = 0;
        if (*p == '-') { sign = -1; ++p; }
        if (!*p) return false;
        for (; *p; ++p) {
            if (*p < '0' || *p > '9' || n > (CE_CSL_DATE_MAX_YEAR - (*p - '0')) / 10) return false;
            n = n * 10 + (*p - '0');
        }
        *value = n * sign; return true;
    }
    return false;
}
static bool endpoint_valid(Endpoint *e, Render *r) {
    int64_t m = e->part[MONTH], d = e->part[DAY];
    if (!e->part[YEAR] && (m || d)) return fail(r->error,r->capacity,"month/day requires a nonzero year");
    if (m >= 21 && m <= 24) { e->part[MONTH] = m - 20; e->season = true; }
    else if (m < 0 || m > 12) return fail(r->error,r->capacity,"unsupported month (use 1..12 or season 21..24)");
    if (d < 0 || d > 31 || (d && (!m || e->season))) return fail(r->error,r->capacity,"invalid day or day without calendar month");
    return true;
}
static bool parse_endpoint(const cJSON *node, Endpoint *e, Render *r) {
    const cJSON *v;
    unsigned i = 0;
    bool missing = false;
    if (!cJSON_IsArray(node) || !node->child) return fail(r->error,r->capacity,"date endpoint must be a nonempty array");
    for (v = node->child; v; v = v->next, ++i) {
        int64_t number;
        if (i == 3) return fail(r->error,r->capacity,"date endpoint has more than three parts");
        if (cJSON_IsString(v) && v->valuestring && !*v->valuestring) number = 0;
        else if (!integer(v,&number)) return fail(r->error,r->capacity,"date parts must be bounded decimal integers");
        if (number && missing) return fail(r->error,r->capacity,"date contains a missing intermediate part");
        if (!number) missing = true;
        e->part[i] = number;
    }
    return endpoint_valid(e,r);
}
static bool raw_endpoint(const char *s, size_t n, Endpoint *e, Render *r) {
    size_t i = 0, j;
    int sign = 1;
    if (!n) return fail(r->error,r->capacity,"raw open endpoints must use '..'");
    if (n == 2 && !memcmp(s,"..",2)) return true;
    if (s[0] == '-') { sign = -1; ++i; }
    if (n - i < 4) return fail(r->error,r->capacity,"unsupported raw date; expected YYYY[-MM[-DD]]");
    for (j = 0; j < 4; ++j) {
        if (s[i] < '0' || s[i] > '9') return fail(r->error,r->capacity,"unsupported raw date syntax");
        e->part[YEAR] = e->part[YEAR] * 10 + (s[i++] - '0');
    }
    e->part[YEAR] *= sign;
    for (j = MONTH; i < n && j <= DAY; ++j) {
        if (n - i < 3 || s[i++] != '-' || s[i] < '0' || s[i] > '9' || s[i+1] < '0' || s[i+1] > '9')
            return fail(r->error,r->capacity,"unsupported raw date syntax");
        e->part[j] = (s[i] - '0') * 10 + (s[i+1] - '0'); i += 2;
        if (!e->part[j]) return fail(r->error,r->capacity,"raw month/day must not be zero");
    }
    if (i != n || !e->part[YEAR]) return fail(r->error,r->capacity,"unsupported raw date syntax/year zero");
    return endpoint_valid(e,r);
}
static bool parse_value(const cJSON *value, DateValue *date, Render *r) {
    const cJSON *parts = NULL, *literal = NULL, *raw = NULL, *season = NULL, *circa = NULL, *v;
    unsigned keys = 0;
    int64_t n;
    if (!value) return true;
    if (!cJSON_IsObject(value)) return fail(r->error,r->capacity,"date value must be a CSL-JSON object");
    for (v = value->child; v; v = v->next) {
        unsigned bit;
        if (equal(v->string,"date-parts")) { bit = 1; parts = v; }
        else if (equal(v->string,"literal")) { bit = 2; literal = v; }
        else if (equal(v->string,"raw")) { bit = 4; raw = v; }
        else if (equal(v->string,"season")) { bit = 8; season = v; }
        else if (equal(v->string,"circa")) { bit = 16; circa = v; }
        else return fail(r->error,r->capacity,"unsupported date property");
        if (keys & bit) return fail(r->error,r->capacity,"duplicate date property");
        keys |= bit;
    }
    if ((literal && (!cJSON_IsString(literal) || !literal->valuestring)) || (raw && (!cJSON_IsString(raw) || !raw->valuestring)))
        return fail(r->error,r->capacity,"literal/raw must be strings");
    if (circa) {
        if (cJSON_IsBool(circa)) date->circa = cJSON_IsTrue(circa);
        else if (cJSON_IsString(circa) && (equal(circa->valuestring,"true") || equal(circa->valuestring,"false") || equal(circa->valuestring,"0") || equal(circa->valuestring,"1")))
            date->circa = equal(circa->valuestring,"true") || equal(circa->valuestring,"1");
        else if (cJSON_IsNumber(circa) && integer(circa,&n) && (n == 0 || n == 1)) date->circa = n == 1;
        else return fail(r->error,r->capacity,"unsupported circa boolean");
    }
    if (parts) {
        if (!cJSON_IsArray(parts)) return fail(r->error,r->capacity,"date-parts must be an array");
        for (v = parts->child; v; v = v->next) {
            if (date->count == 2) return fail(r->error,r->capacity,"more than two date endpoints");
            if (!parse_endpoint(v,&date->end[date->count],r)) return false;
            ++date->count;
        }
    }
    if (literal && *literal->valuestring) {
        if (date->count || season) return fail(r->error,r->capacity,"literal conflicts with structured date parts");
        date->literal = literal->valuestring;
    } else if (!date->count && raw && *raw->valuestring) {
        const char *slash = strchr(raw->valuestring,'/');
        size_t length = strlen(raw->valuestring);
        if (length > 32) return fail(r->error,r->capacity,"unsupported raw date syntax/length");
        if (!raw_endpoint(raw->valuestring,slash ? (size_t)(slash - raw->valuestring) : length,&date->end[0],r)) return false;
        date->count = 1;
        if (slash) {
            if (!raw_endpoint(slash+1,length - (size_t)(slash+1-raw->valuestring),&date->end[1],r)) return false;
            date->count = 2;
        } else if (!date->end[0].part[YEAR]) return fail(r->error,r->capacity,"open endpoint requires an interval");
    }
    if (season) {
        if (!integer(season,&n) || n < 1 || n > 4 || date->count != 1 || !date->end[0].part[YEAR] || date->end[0].part[MONTH])
            return fail(r->error,r->capacity,"season requires 1..4 and a single year without month/day");
        date->end[0].part[MONTH] = n; date->end[0].season = true;
    }
    if (date->count == 2 && !date->end[0].part[YEAR] && !date->end[1].part[YEAR])
        return fail(r->error,r->capacity,"an interval needs at least one nonempty endpoint");
    if (date->count == 2 && date->end[0].part[YEAR] && date->end[1].part[YEAR]) {
        unsigned i;
        for (i = MONTH; i <= DAY; ++i) if (!!date->end[0].part[i] != !!date->end[1].part[i])
            return fail(r->error,r->capacity,"mixed-precision date intervals are not yet supported");
    }
    return true;
}

static bool append(CeCslOutput *out, const char *text, unsigned format, Render *r) {
    return ce_csl_output_append(out,or_empty(text),text ? strlen(text) : 0,format,NULL,r->error,r->capacity);
}
static CeCslOutput *output_new(Render *r) { return ce_csl_output_new(r->options->allocator,r->error,r->capacity); }
static bool term(CeCslOutput *out, const char *name, CeCslLocaleForm form, unsigned format, Render *r) {
    CeCslLocaleTerm t = ce_csl_locale_term(r->locale,name,form,false,CE_CSL_LOCALE_NEUTER);
    if (!t.found) r->result.missing_term = true;
    return ce_csl_output_append(out,t.text,t.length,format,NULL,r->error,r->capacity);
}
static bool apply_case(CeCslOutput **out, CeCslTextCase mode, Render *r) {
    CeCslOutputString plain = {0};
    CeCslCaseMappedText changed = {0};
    CeCslOutput *replacement = NULL;
    size_t i, span = 0, source_end = 0, count = ce_csl_output_run_count(*out);
    bool ok = false;
    if (!count || mode == CE_CSL_CASE_NONE) return true;
    if (!ce_csl_output_plain(*out,&plain,r->error,r->capacity) ||
        !ce_csl_case_transform_mapped(plain.data,plain.length,mode,r->options->item_language,r->options->style_default_locale,
            r->options->allocator,&changed,r->error,r->capacity)) goto done;
    replacement = output_new(r);
    if (!replacement) goto done;
    for (i = 0; i < count; ++i) {
        const CeCslOutputRun *run = ce_csl_output_run(*out,i);
        size_t begin = span < changed.span_count ? changed.spans[span].output_offset : changed.text.length, end = begin;
        source_end += run->byte_length;
        while (span < changed.span_count && changed.spans[span].source_offset < source_end) {
            end = changed.spans[span].output_offset + changed.spans[span].output_length;
            ++span;
        }
        if (!ce_csl_output_append(replacement,changed.text.data+begin,end-begin,run->format,NULL,r->error,r->capacity)) goto done;
    }
    ce_csl_output_free(*out); *out = replacement; replacement = NULL; ok = true;
done:
    ce_csl_output_free(replacement); ce_csl_output_string_free(&plain); ce_csl_case_mapped_free(&changed); return ok;
}
static bool part_text(const Endpoint *end, const DatePart *p, CeCslOutput **out, Render *r) {
    char number[64], name[32];
    int64_t value = end->part[p->kind];
    CeCslOutputString ordinal = {0};
    bool missing = false, only_first = false;
    if (!value) return true;
    if (p->kind == MONTH && (end->season || !p->form || equal(p->form,"long") || equal(p->form,"short"))) {
        snprintf(name,sizeof(name),"%s-%02d",end->season ? "season" : "month",(int)value);
        if (!term(*out,name,equal(p->form,"short") ? CE_CSL_LOCALE_SHORT : CE_CSL_LOCALE_LONG,p->format,r)) return false;
    } else if (p->kind == DAY && equal(p->form,"ordinal") &&
        !(ce_csl_locale_option(r->locale,CE_CSL_LOCALE_DAY_ORDINALS_ONLY_FIRST,&only_first) && only_first && value != 1)) {
        CeCslLocaleGender gender = CE_CSL_LOCALE_NEUTER;
        if (end->part[MONTH] && !end->season) {
            snprintf(name,sizeof(name),"month-%02d",(int)end->part[MONTH]);
            gender = ce_csl_locale_noun_gender(r->locale,name);
        }
        if (ce_csl_number_format((uint64_t)value,CE_CSL_NUMBER_ORDINAL,r->locale,gender,r->options->allocator,&ordinal,&missing,r->error,r->capacity) != CE_CSL_NUMBER_OK) return false;
        r->result.missing_term |= missing;
        if (!ce_csl_output_append(*out,ordinal.data,ordinal.length,p->format,NULL,r->error,r->capacity)) {
            ce_csl_output_string_free(&ordinal); return false;
        }
        ce_csl_output_string_free(&ordinal);
    } else {
        int64_t magnitude = value < 0 ? -value : value;
        if (p->kind == YEAR && equal(p->form,"short")) snprintf(number,sizeof(number),"%02d",(int)(magnitude % 100));
        else if (equal(p->form,"numeric-leading-zeros")) snprintf(number,sizeof(number),"%02d",(int)value);
        else snprintf(number,sizeof(number),"%lld",(long long)magnitude);
        if (!append(*out,number,p->format,r)) return false;
        if (p->kind == YEAR && (value < 0 || value < 1000))
            if (!term(*out,value < 0 ? "bc" : "ad",CE_CSL_LOCALE_LONG,p->format,r)) return false;
    }
    if (p->strip && ce_csl_output_text_bytes(*out)) {
        CeCslOutputString plain = {0};
        CeCslOutput *stripped;
        size_t read, write = 0;
        if (!ce_csl_output_plain(*out,&plain,r->error,r->capacity)) return false;
        for (read = 0; read < plain.length; ++read) if (plain.data[read] != '.') plain.data[write++] = plain.data[read];
        stripped = output_new(r);
        if (!stripped || !ce_csl_output_append(stripped,plain.data,write,p->format,NULL,r->error,r->capacity)) {
            ce_csl_output_string_free(&plain); ce_csl_output_free(stripped); return false;
        }
        ce_csl_output_string_free(&plain); ce_csl_output_free(*out); *out = stripped;
    }
    return apply_case(out,p->text_case,r);
}

/* The range's first side owns common components before/inside the varying
 * span. The second side owns common trailing components. This also preserves
 * nonstandard display orders without duplicating their shared components. */
static bool assemble(const DateValue *value, const DatePlan *plan, CeCslOutput *body, Render *r) {
    CeCslOutput *parts[2][3] = {{0}};
    int diff = -1, range_part = -1, last_varying = -1;
    unsigned e;
    size_t i;
    bool ok = false, emitted = false, open = false;
    if (value->literal) return append(body,value->literal,plan->format,r);
    for (e = 0; e < value->count; ++e) for (i = 0; i < plan->count; ++i) {
        parts[e][i] = output_new(r);
        if (!parts[e][i] || !part_text(&value->end[e],&plan->parts[i],&parts[e][i],r)) goto done;
    }
    if (value->count == 2) {
        open = !value->end[0].part[YEAR] || !value->end[1].part[YEAR];
        for (i = 0; i < plan->count; ++i) {
            int kind = plan->parts[i].kind;
            bool differs = value->end[0].part[kind] != value->end[1].part[kind] ||
                (kind == MONTH && value->end[0].season != value->end[1].season);
            if (differs && (diff < 0 || kind < diff)) { diff = kind; range_part = (int)i; }
        }
        if (diff >= 0) for (i = 0; i < plan->count; ++i)
            if ((open || plan->parts[i].kind >= diff) &&
                (ce_csl_output_text_bytes(parts[0][i]) || ce_csl_output_text_bytes(parts[1][i]))) last_varying = (int)i;
    }
    if (diff < 0 || last_varying < 0) {
        if (value->count) for (i = 0; i < plan->count; ++i) {
            const DatePart *p = &plan->parts[i];
            if (!ce_csl_output_text_bytes(parts[0][i])) continue;
            if ((emitted && !append(body,plan->delimiter,plan->format,r)) ||
                !append(body,p->prefix,plan->format,r) || !ce_csl_output_concat(body,parts[0][i],r->error,r->capacity) ||
                !append(body,p->suffix,plan->format,r)) goto done;
            emitted = true;
        }
    } else {
        bool first_end = true;
        for (i = 0; i <= (size_t)last_varying; ++i) {
            const DatePart *p = &plan->parts[i];
            if (!ce_csl_output_text_bytes(parts[0][i])) continue;
            if ((emitted && !append(body,plan->delimiter,plan->format,r)) ||
                !append(body,p->prefix,plan->format,r) || !ce_csl_output_concat(body,parts[0][i],r->error,r->capacity) ||
                ((int)i != last_varying && !append(body,p->suffix,plan->format,r))) goto done;
            emitted = true;
        }
        if (!append(body,plan->parts[range_part].range,plan->format,r)) goto done;
        for (i = 0; i < plan->count; ++i) {
            const DatePart *p = &plan->parts[i];
            if (!ce_csl_output_text_bytes(parts[1][i]) || (!open && (int)i <= last_varying && p->kind < diff)) continue;
            if ((!first_end && !append(body,plan->delimiter,plan->format,r)) ||
                (!first_end && !append(body,p->prefix,plan->format,r)) ||
                !ce_csl_output_concat(body,parts[1][i],r->error,r->capacity) || !append(body,p->suffix,plan->format,r)) goto done;
            first_end = false;
        }
    }
    ok = true;
done:
    for (e = 0; e < 2; ++e) for (i = 0; i < 3; ++i) ce_csl_output_free(parts[e][i]);
    return ok;
}

bool ce_csl_date_render(const cJSON *value, const CeCslXmlNode *date_node,
    const CeCslLocale *locale, const CeCslDateOptions *options,
    CeCslOutput *destination, CeCslDateResult *result, char *error, size_t capacity) {
    const CeCslDateOptions defaults = {0};
    DatePlan plan = {0}; DateValue date = {0};
    Render r = {locale,options ? options : &defaults,{0},error,capacity};
    CeCslOutput *body = NULL;
    bool ok = false;
    if (error && capacity) *error = 0;
    if (!date_node || !destination) return fail(error,capacity,"date node and output are required");
    if (!make_plan(date_node,&plan,&r) || !parse_value(value,&date,&r)) return false;
    r.result.circa = date.circa;
    body = output_new(&r);
    if (!body || !assemble(&date,&plan,body,&r) || !apply_case(&body,plan.text_case,&r)) goto done;
    r.result.rendered = ce_csl_output_text_bytes(body) != 0;
    if (!ce_csl_format_affixes(date_node,body,r.options->base_format,r.options->allocator,destination,error,capacity)) goto done;
    if (result) *result = r.result;
    ok = true;
done:
    ce_csl_output_free(body); return ok;
}
