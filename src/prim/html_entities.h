/* Declarations for the generated HTML entity table (ADR 28).
 * Generated alongside src/prim/html_entities.c by
 * tools/gen_html_entities.py; do not edit by hand.
 */
#ifndef MINO_HTML_ENTITIES_H
#define MINO_HTML_ENTITIES_H

typedef struct mino_html_entity_s {
    const char *name;  /* html5 spelling; ';' included when required */
    const char *value; /* UTF-8 expansion, NUL-terminated */
} mino_html_entity_t;

extern const mino_html_entity_t k_html_entities[2231];
extern const unsigned k_html_entities_count;

#endif /* MINO_HTML_ENTITIES_H */
