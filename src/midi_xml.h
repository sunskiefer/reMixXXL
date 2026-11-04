/* reMixXXL -- a minimal XML reader, enough for Mixxx's <name>.midi.xml controller mappings.
 *
 * Reads elements, attributes and text into a small tree. Handles the XML declaration, comments, CDATA, a DOCTYPE
 * (skipped), self-closing tags and the five predefined entities (&amp; &lt; &gt; &quot; &apos;) plus numeric
 * character references (&#..; &#x..;). No namespaces, no external entities, no validation. Text is UTF-8 as in
 * the file. Not thread-safe per tree (nothing is shared between trees). */
#pragma once
#include <stddef.h>

typedef struct xml_attr { char *name, *value; } xml_attr_t;

typedef struct xml_node {
    char *name;                      /* element name */
    char *text;                      /* the element's own text (all text children joined, entities decoded) */
    xml_attr_t *attrs;
    int nattr;
    struct xml_node *first_child;    /* element children, in document order */
    struct xml_node *next;           /* next sibling element */
    struct xml_node *parent;
} xml_node_t;

/* Parse a document held in memory (len bytes). Returns the root element, or NULL with a message in err. */
xml_node_t *xml_parse(const char *data, size_t len, char *err, int errn);
xml_node_t *xml_parse_file(const char *path, char *err, int errn);
void xml_free(xml_node_t *root);

xml_node_t *xml_child(const xml_node_t *n, const char *name);        /* first child element called name, or NULL */
xml_node_t *xml_next(const xml_node_t *n, const char *name);         /* next sibling called name (NULL name: any) */
const char *xml_attr(const xml_node_t *n, const char *name);         /* attribute value, or NULL */
const char *xml_child_text(const xml_node_t *n, const char *name);   /* trimmed text of the child, "" if absent */
const char *xml_text_trim(const xml_node_t *n);                      /* the node's own text, trimmed (in place) */
int xml_text_int(const xml_node_t *n, const char *child, int def);   /* child text as an int, base auto (0x..) */
