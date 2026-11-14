#ifndef CE_CSL_XML_H
#define CE_CSL_XML_H
#include <stdbool.h>
#include <stddef.h>

#define CE_CSL_NAMESPACE "http://purl.org/net/xbiblio/csl"
#define CE_CSL_XML_MAX_BYTES ((size_t)2 * 1024 * 1024)
#define CE_CSL_XML_MAX_NODES ((size_t)50000)
#define CE_CSL_XML_MAX_DEPTH ((size_t)128)

typedef struct CeCslXmlAttr {
    char *name; /* Expanded namespace name, separated from local name by '\x1f'. */
    char *value; /* XML-normalized; intentional leading/trailing spaces retained. */
} CeCslXmlAttr;
typedef struct CeCslXmlNode {
    char *name;
    char *text; /* Direct character data; never includes descendants' text. */
    size_t text_length, text_capacity, line, attribute_count;
    CeCslXmlAttr *attributes;
    struct CeCslXmlNode *parent, *children, *last_child, *next;
    struct CeCslXmlNode *allocated_next;
} CeCslXmlNode;
typedef struct CeCslXml {
    CeCslXmlNode *root, *allocated;
    size_t node_count;
} CeCslXml;

/* Owned UTF-8 tree, or NULL with an explanation. Never loads external files or
 * network resources. DTDs, all declared entities, oversized/deep input and
 * malformed XML are rejected. XML's five predefined entities remain valid. */
CeCslXml *ce_csl_xml_parse(const char *xml, size_t size, char *error, size_t capacity);
void ce_csl_xml_free(CeCslXml *document);
bool ce_csl_xml_is(const CeCslXmlNode *node, const char *local_name);
const char *ce_csl_xml_attribute(const CeCslXmlNode *node, const char *name);
const CeCslXmlNode *ce_csl_xml_child(const CeCslXmlNode *node, const char *local_name);
#endif
