#ifndef CE_CSL_CONDITION_H
#define CE_CSL_CONDITION_H
#include "csl_xml.h"

enum CeCslPosition {
    CE_CSL_POSITION_FIRST=1u<<0,
    CE_CSL_POSITION_SUBSEQUENT=1u<<1,
    CE_CSL_POSITION_IBID=1u<<2,
    CE_CSL_POSITION_IBID_WITH_LOCATOR=1u<<3,
    CE_CSL_POSITION_NEAR_NOTE=1u<<4
};
typedef struct CeCslVariableFacts {
    bool present;
    bool numeric;
    bool uncertain_date;
} CeCslVariableFacts;
typedef struct CeCslConditionContext {
    void *context;
    /* Names are temporary NUL-terminated views. Read the long form and apply
     * the current substitution/suppression state. Return false on failure,
     * never conflate an unavailable value with an evaluation error. */
    bool (*variable)(void *context,const char *name,CeCslVariableFacts *facts,
                     char *error,size_t capacity);
    const char *item_type;
    const char *locator_type; /* NULL/empty means the CSL default "page". */
    bool locator_present; /* A label by itself is not a locator. */
    unsigned position;
    bool bibliography;
    bool disambiguate; /* Assigned by the document's disambiguation pass. */
} CeCslConditionContext;

/* Evaluate one cs:if or cs:else-if. Match all/any/none operates over every token
 * of every condition, not just one boolean per attribute. Position implication
 * is applied (ibid-with-locator => ibid => subsequent; near-note => subsequent).
 * Position predicates are always false in bibliography context. Conflicting
 * first/subsequent state, unknown attributes/positions and resolver failures
 * are errors. A failed call leaves *matches unchanged. This layer does not
 * derive cite position, decide disambiguation or parse variable contents. */
bool ce_csl_condition_test(const CeCslXmlNode *condition,
    const CeCslConditionContext *context,bool *matches,char *error,size_t capacity);

/* Select the first matching branch, else the optional cs:else. NULL success
 * means no branch matched. The returned node is borrowed. Structural validity
 * is checked for every branch before evaluating any predicates. */
bool ce_csl_choose_branch(const CeCslXmlNode *choose,
    const CeCslConditionContext *context,const CeCslXmlNode **branch,
    char *error,size_t capacity);
#endif
