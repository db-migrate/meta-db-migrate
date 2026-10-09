/**
 * database.yml, which node read with plugin-yaml.
 *
 *   defaults: &defaults
 *     driver: pg
 *     host: localhost
 *   dev:
 *     <<: *defaults
 *     database: shop
 *   prod:
 *     <<: *defaults
 *     password: {ENV: SHOP_PASSWORD}
 *
 * The values come out as js-yaml's safeLoad made them, so a file that worked
 * there means the same here: an unquoted 5432 is a number, true and false
 * are truths, null and ~ and nothing are null, anything quoted is text.
 * Anchors, aliases and `<<` merges work; a key written beside a merge wins
 * over the merged one.
 *
 * libyaml reads it - which is why this is a plugin and not the core: a
 * program configured by JSON does not need libyaml installed.
 */
#include <db_migrate_plugin.h>

#include <yaml.h>
#pragma meta needs "yaml-0.1"

#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static bool among(const char *text, const char *const *words) {

  for (; *words != NULL; ++words)
    if (strcmp(text, *words) == 0)
      return true;

  return false;
}

static const char *const nulls[] = {"", "~", "null", "Null", "NULL", NULL};
static const char *const trues[] = {"true", "True", "TRUE", NULL};
static const char *const falses[] = {"false", "False", "FALSE", NULL};

/** A whole number as YAML 1.2 writes one: 42, -7, 0x2a, 0o52. */
static bool wholeNumber(const char *text, long long *value) {

  const char *digits = text;
  int base = 10;
  char *end;

  if (text[0] == '0' && (text[1] == 'x' || text[1] == 'o')) {
    base = text[1] == 'x' ? 16 : 8;
    digits = text + 2;
  } else {
    const char *unsigned_ = text + (text[0] == '-' || text[0] == '+');

    if (unsigned_[0] == '\0' ||
        strspn(unsigned_, "0123456789") != strlen(unsigned_))
      return false;
  }

  if (digits[0] == '\0')
    return false;

  errno = 0;
  *value = strtoll(digits, &end, base);
  return *end == '\0' && errno == 0;
}

static bool realNumber(const char *text, double *value) {

  char *end;

  if (among(text, (const char *const[]){".inf", ".Inf", ".INF", "+.inf",
                                        NULL})) {
    *value = INFINITY;
    return true;
  }

  if (among(text, (const char *const[]){"-.inf", "-.Inf", "-.INF", NULL})) {
    *value = -INFINITY;
    return true;
  }

  if (strpbrk(text, "0123456789") == NULL || strpbrk(text, "xX") != NULL)
    return false;

  errno = 0;
  *value = strtod(text, &end);
  return *end == '\0' && errno == 0;
}

static yyjson_mut_val *scalar(yyjson_mut_doc *out, yaml_node_t *node) {

  const char *text = (const char *)node->data.scalar.value;
  long long whole;
  double real;

  /* quoted or a block: text, whatever it looks like */
  if (node->data.scalar.style != YAML_PLAIN_SCALAR_STYLE)
    return yyjson_mut_strcpy(out, text);

  if (among(text, nulls))
    return yyjson_mut_null(out);

  if (among(text, trues))
    return yyjson_mut_true(out);

  if (among(text, falses))
    return yyjson_mut_false(out);

  if (wholeNumber(text, &whole))
    return yyjson_mut_sint(out, whole);

  if (realNumber(text, &real) && isfinite(real))
    return yyjson_mut_real(out, real);

  return yyjson_mut_strcpy(out, text);
}

static yyjson_mut_val *converted(yyjson_mut_doc *out, yaml_document_t *document,
                                 yaml_node_t *node, int depth);

/** The keys of a merged mapping that the mapping has not said itself. */
static void merge(yyjson_mut_doc *out, yyjson_mut_val *into,
                  yaml_document_t *document, yaml_node_t *from, int depth) {

  if (from->type == YAML_SEQUENCE_NODE) {

    for (yaml_node_item_t *item = from->data.sequence.items.start;
         item < from->data.sequence.items.top; ++item)
      merge(out, into, document, yaml_document_get_node(document, *item),
            depth);
    return;
  }

  if (from->type != YAML_MAPPING_NODE)
    return;

  yyjson_mut_val *merged = converted(out, document, from, depth + 1);
  yyjson_mut_val *key;
  yyjson_mut_obj_iter iter = yyjson_mut_obj_iter_with(merged);

  /* copies: adding the nodes themselves would take them out of `merged` */
  while ((key = yyjson_mut_obj_iter_next(&iter)) != NULL)
    if (yyjson_mut_obj_get(into, yyjson_mut_get_str(key)) == NULL)
      yyjson_mut_obj_add(
          into, yyjson_mut_val_mut_copy(out, key),
          yyjson_mut_val_mut_copy(out, yyjson_mut_obj_iter_get_val(key)));
}

static yyjson_mut_val *converted(yyjson_mut_doc *out, yaml_document_t *document,
                                 yaml_node_t *node, int depth) {

  /* an alias of itself would go round for ever */
  if (node == NULL || depth > 64)
    return yyjson_mut_null(out);

  if (node->type == YAML_SCALAR_NODE)
    return scalar(out, node);

  if (node->type == YAML_SEQUENCE_NODE) {

    yyjson_mut_val *array = yyjson_mut_arr(out);

    for (yaml_node_item_t *item = node->data.sequence.items.start;
         item < node->data.sequence.items.top; ++item)
      yyjson_mut_arr_append(
          array, converted(out, document,
                           yaml_document_get_node(document, *item), depth + 1));

    return array;
  }

  yyjson_mut_val *object = yyjson_mut_obj(out);
  yaml_node_pair_t *pair;

  /* what is written first, then what `<<` brings that is not */
  for (pair = node->data.mapping.pairs.start;
       pair < node->data.mapping.pairs.top; ++pair) {

    yaml_node_t *key = yaml_document_get_node(document, pair->key);
    const char *name = key != NULL && key->type == YAML_SCALAR_NODE
                           ? (const char *)key->data.scalar.value
                           : "";

    if (strcmp(name, "<<") == 0 && key->data.scalar.style ==
                                       YAML_PLAIN_SCALAR_STYLE)
      continue;

    yyjson_mut_obj_put(
        object, yyjson_mut_strcpy(out, name),
        converted(out, document, yaml_document_get_node(document, pair->value),
                  depth + 1));
  }

  for (pair = node->data.mapping.pairs.start;
       pair < node->data.mapping.pairs.top; ++pair) {

    yaml_node_t *key = yaml_document_get_node(document, pair->key);

    if (key != NULL && key->type == YAML_SCALAR_NODE &&
        key->data.scalar.style == YAML_PLAIN_SCALAR_STYLE &&
        strcmp((const char *)key->data.scalar.value, "<<") == 0)
      merge(out, object, document,
            yaml_document_get_node(document, pair->value), depth);
  }

  return object;
}

static bool readYaml(const char *file, json_t *config, char *why,
                     size_t room) {

  FILE *input = fopen(file, "rb");
  yaml_parser_t parser;
  yaml_document_t document;

  if (input == NULL) {
    dbmWrite(why, room, TEXT`cannot read ${file}: ${strerror(errno)}`);
    return false;
  }

  yaml_parser_initialize(&parser);
  yaml_parser_set_input_file(&parser, input);

  if (!yaml_parser_load(&parser, &document)) {
    dbmWrite(why, room, TEXT`${file} is not YAML: ${parser.problem != NULL ? parser.problem : "it does not parse"} at line ${(long)parser.problem_mark.line + 1}`);
    yaml_parser_delete(&parser);
    fclose(input);
    return false;
  }

  yyjson_mut_doc *out = yyjson_mut_doc_new(NULL);
  yaml_node_t *root = yaml_document_get_root_node(&document);

  yyjson_mut_doc_set_root(out, root != NULL
                                   ? converted(out, &document, root, 0)
                                   : yyjson_mut_obj(out));

  yaml_document_delete(&document);
  yaml_parser_delete(&parser);
  fclose(input);

  *config = meta_jsonFromMut(out);
  return true;
}

__attribute__((constructor)) static void registerYaml(void) {
  dbmRegisterConfigLoader(".yml", readYaml);
  dbmRegisterConfigLoader(".yaml", readYaml);
}
