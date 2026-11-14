#include "csl_xml.h"
#include <expat.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct XmlReader {
    XML_Parser parser;
    CeCslXml *document;
    CeCslXmlNode *current;
    size_t depth, decoded_bytes;
    char problem[256];
} XmlReader;

static void fail(XmlReader *reader, const char *message) {
    if (!reader->problem[0]) snprintf(reader->problem, sizeof reader->problem, "%s", message);
    XML_StopParser(reader->parser, XML_FALSE);
}
static char *copy(XmlReader *reader, const char *text) {
    size_t length = strlen(text);
    if (length > CE_CSL_XML_MAX_BYTES - reader->decoded_bytes) {
        fail(reader, "CSL XML decoded content exceeds its limit"); return NULL;
    }
    char *result = malloc(length + 1);
    if (!result) { fail(reader, "Not enough memory to read CSL XML"); return NULL; }
    memcpy(result, text, length + 1); reader->decoded_bytes += length;
    return result;
}
static void XMLCALL start(void *userdata, const XML_Char *name, const XML_Char **attributes) {
    XmlReader *reader = userdata;
    if (reader->problem[0]) return;
    if (reader->depth >= CE_CSL_XML_MAX_DEPTH || reader->document->node_count >= CE_CSL_XML_MAX_NODES) {
        fail(reader, "CSL XML exceeds its nesting or element limit"); return;
    }
    size_t count = 0;
    while (attributes[count * 2]) {
        if (++count > 128) { fail(reader, "CSL XML has too many attributes on an element"); return; }
    }
    CeCslXmlNode *node = calloc(1, sizeof *node);
    if (!node) { fail(reader, "Not enough memory to read CSL XML"); return; }
    node->allocated_next = reader->document->allocated; reader->document->allocated = node;
    ++reader->document->node_count;
    node->line = (size_t)XML_GetCurrentLineNumber64(reader->parser);
    node->name = copy(reader, name);
    if (!node->name) return;
    if (count) {
        node->attributes = calloc(count, sizeof *node->attributes);
        if (!node->attributes) { fail(reader, "Not enough memory to read CSL XML"); return; }
        node->attribute_count = count;
        for (size_t i = 0; i < count; ++i) {
            node->attributes[i].name = copy(reader, attributes[i * 2]);
            if (!node->attributes[i].name) return;
            node->attributes[i].value = copy(reader, attributes[i * 2 + 1]);
            if (!node->attributes[i].value) return;
        }
    }
    node->parent = reader->current;
    if (reader->current) {
        if (reader->current->last_child) reader->current->last_child->next = node;
        else reader->current->children = node;
        reader->current->last_child = node;
    } else reader->document->root = node;
    reader->current = node; ++reader->depth;
}
static void XMLCALL end(void *userdata, const XML_Char *name) {
    (void)name;
    XmlReader *reader = userdata;
    if (reader->problem[0]) return;
    if (reader->current) reader->current = reader->current->parent;
    if (reader->depth) --reader->depth;
}
static void XMLCALL characters(void *userdata, const XML_Char *text, int length) {
    XmlReader *reader = userdata;
    CeCslXmlNode *node = reader->current;
    if (reader->problem[0] || !node || length <= 0) return;
    size_t size = (size_t)length;
    if (size > CE_CSL_XML_MAX_BYTES - reader->decoded_bytes) {
        fail(reader, "CSL XML decoded content exceeds its limit"); return;
    }
    size_t needed = node->text_length + size + 1;
    if (needed > node->text_capacity) {
        size_t capacity = node->text_capacity ? node->text_capacity : 64;
        while (capacity < needed) capacity *= 2;
        char *next = realloc(node->text, capacity);
        if (!next) { fail(reader, "Not enough memory to read CSL XML"); return; }
        node->text = next; node->text_capacity = capacity;
    }
    memcpy(node->text + node->text_length, text, size);
    node->text_length += size; node->text[node->text_length] = '\0';
    reader->decoded_bytes += size;
}
static void XMLCALL doctype(void *userdata, const XML_Char *name,
                            const XML_Char *system_id, const XML_Char *public_id, int subset) {
    (void)name; (void)system_id; (void)public_id; (void)subset;
    fail(userdata, "CSL XML document types and declared entities are not allowed");
}
static int XMLCALL external_entity(XML_Parser parser, const XML_Char *context,
                                   const XML_Char *base, const XML_Char *system_id,
                                   const XML_Char *public_id) {
    (void)context; (void)base; (void)system_id; (void)public_id;
    XmlReader *reader = XML_GetUserData(parser);
    fail(reader, "CSL XML external resources are not allowed"); return XML_STATUS_ERROR;
}
void ce_csl_xml_free(CeCslXml *document) {
    if (!document) return;
    CeCslXmlNode *node = document->allocated;
    while (node) {
        CeCslXmlNode *next = node->allocated_next;
        for (size_t i = 0; i < node->attribute_count; ++i) {
            free(node->attributes[i].name); free(node->attributes[i].value);
        }
        free(node->attributes); free(node->name); free(node->text); free(node); node = next;
    }
    free(document);
}
CeCslXml *ce_csl_xml_parse(const char *xml, size_t size, char *error, size_t capacity) {
    if (error && capacity) error[0] = '\0';
    const char *problem = NULL;
    XmlReader reader = {0};
    if (!xml || !size || size > CE_CSL_XML_MAX_BYTES || memchr(xml, '\0', size)) {
        problem = "CSL XML is empty, contains NUL, or exceeds the input limit"; goto failed;
    }
    reader.document = calloc(1, sizeof *reader.document);
    reader.parser = XML_ParserCreateNS("UTF-8", '\x1f');
    if (!reader.document || !reader.parser) { problem = "Not enough memory to read CSL XML"; goto failed; }
    XML_SetUserData(reader.parser, &reader);
    XML_SetElementHandler(reader.parser, start, end);
    XML_SetCharacterDataHandler(reader.parser, characters);
    XML_SetStartDoctypeDeclHandler(reader.parser, doctype);
    XML_SetExternalEntityRefHandler(reader.parser, external_entity);
    if (XML_Parse(reader.parser, xml, (int)size, XML_TRUE) != XML_STATUS_OK || reader.problem[0]) {
        if (!reader.problem[0]) snprintf(reader.problem, sizeof reader.problem, "CSL XML line %llu: %s",
            (unsigned long long)XML_GetCurrentLineNumber64(reader.parser), XML_ErrorString(XML_GetErrorCode(reader.parser)));
        problem = reader.problem; goto failed;
    }
    XML_ParserFree(reader.parser);
    return reader.document;
failed:
    if (error && capacity) snprintf(error, capacity, "%s", problem ? problem : "Cannot read CSL XML");
    if (reader.parser) XML_ParserFree(reader.parser);
    ce_csl_xml_free(reader.document); return NULL;
}
bool ce_csl_xml_is(const CeCslXmlNode *node, const char *name) {
    static const char prefix[] = CE_CSL_NAMESPACE "\x1f";
    return node && name && !strncmp(node->name, prefix, sizeof prefix - 1) &&
        !strcmp(node->name + sizeof prefix - 1, name);
}
const char *ce_csl_xml_attribute(const CeCslXmlNode *node, const char *name) {
    if (!node || !name) return NULL;
    const char *wanted = !strcmp(name, "xml:lang") ? "http://www.w3.org/XML/1998/namespace\x1flang" : name;
    for (size_t i = 0; i < node->attribute_count; ++i)
        if (!strcmp(node->attributes[i].name, wanted)) return node->attributes[i].value;
    return NULL;
}
const CeCslXmlNode *ce_csl_xml_child(const CeCslXmlNode *node, const char *name) {
    for (const CeCslXmlNode *child = node ? node->children : NULL; child; child = child->next)
        if (ce_csl_xml_is(child, name)) return child;
    return NULL;
}
