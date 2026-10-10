/**
 * node db-migrate's rc files, since 1.1.0: options kept beside a project
 * instead of written out on every command line.
 *
 *   {"migrations-dir": "db/migrations", "lock-timeout": 120000,
 *    "deprecation": {"releases": 2, "drop": "auto"}}
 *
 * Read as node's `rc('db-migrate')` reads them, a later one over an earlier:
 *
 *   /etc/db-migrate/config, /etc/db-migraterc,
 *   ~/.config/db-migrate/config, ~/.config/db-migrate,
 *   ~/.db-migrate/config, ~/.db-migraterc,
 *   the first .db-migraterc from the working directory upwards,
 *   the file $config names,
 *   and variables db-migrate_<key>, `__` for a level down
 *
 * A file starting with `{` is JSON, comments allowed; anything else INI -
 * `key = value`, `[section]` for an object. The command line comes last and
 * wins over all of them.
 */
#include <db_migrate_driver.h>

#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

extern char **environ;

/** A whole file, or NULL. The caller frees it. */
static char *slurp(const char *path) {

  struct stat info;
  FILE *file;
  char *text = NULL;
  long length;

  if (stat(path, &info) != 0 || !S_ISREG(info.st_mode) ||
      (file = fopen(path, "rb")) == NULL)
    return NULL;

  if (fseek(file, 0, SEEK_END) == 0 && (length = ftell(file)) >= 0 &&
      fseek(file, 0, SEEK_SET) == 0 && (text = malloc((size_t)length + 1))) {
    size_t got = fread(text, 1, (size_t)length, file);
    text[got] = '\0';
  }

  fclose(file);
  return text;
}

/** `a` with `b` laid over it, objects merged, everything else replaced. */
static void deepExtend(yyjson_mut_doc *doc, yyjson_mut_val *a,
                       yyjson_mut_val *b) {

  yyjson_mut_obj_iter walk;
  yyjson_mut_val *key;

  yyjson_mut_obj_iter_init(b, &walk);

  while ((key = yyjson_mut_obj_iter_next(&walk)) != NULL) {

    const char *name = yyjson_mut_get_str(key);
    yyjson_mut_val *value = yyjson_mut_obj_iter_get_val(key);
    yyjson_mut_val *there = yyjson_mut_obj_get(a, name);

    if (there != NULL && yyjson_mut_is_obj(there) && yyjson_mut_is_obj(value))
      deepExtend(doc, there, value);
    else
      yyjson_mut_obj_put(a, yyjson_mut_strcpy(doc, name),
                         yyjson_mut_val_mut_copy(doc, value));
  }
}

/** JSON with its // and block comments taken out, outside of strings. */
static void withoutComments(char *text) {

  char *to = text;
  bool quoted = false;

  for (char *at = text; *at != '\0';) {

    if (quoted) {
      if (*at == '\\' && at[1] != '\0') {
        *to++ = *at++;
      } else if (*at == '"') {
        quoted = false;
      }
      *to++ = *at++;
    } else if (at[0] == '/' && at[1] == '/') {
      while (*at != '\0' && *at != '\n')
        ++at;
    } else if (at[0] == '/' && at[1] == '*') {
      char *end = strstr(at + 2, "*/");
      at = end != NULL ? end + 2 : at + strlen(at);
    } else {
      if (*at == '"')
        quoted = true;
      *to++ = *at++;
    }
  }

  *to = '\0';
}

static char *trimmed(char *text) {

  while (isspace((unsigned char)*text))
    ++text;

  size_t length = strlen(text);

  while (length > 0 && isspace((unsigned char)text[length - 1]))
    text[--length] = '\0';

  return text;
}

/** An INI value as the ini package reads one: true, false, null, or text. */
static yyjson_mut_val *iniValue(yyjson_mut_doc *doc, char *value) {

  size_t length = strlen(value);

  if (length >= 2 && ((value[0] == '"' && value[length - 1] == '"') ||
                      (value[0] == '\'' && value[length - 1] == '\''))) {
    value[length - 1] = '\0';
    return yyjson_mut_strcpy(doc, value + 1);
  }

  if (strcmp(value, "true") == 0)
    return yyjson_mut_true(doc);
  if (strcmp(value, "false") == 0)
    return yyjson_mut_false(doc);
  if (strcmp(value, "null") == 0)
    return yyjson_mut_null(doc);

  return yyjson_mut_strcpy(doc, value);
}

/** `key = value` lines, `[a.b]` sections, `;` and `#` comments. */
static yyjson_mut_val *ini(yyjson_mut_doc *doc, char *text) {

  yyjson_mut_val *root = yyjson_mut_obj(doc);
  yyjson_mut_val *section = root;

  for (char *line = strtok(text, "\n"); line != NULL; line = strtok(NULL, "\n")) {

    char *at = trimmed(line);

    if (*at == '\0' || *at == ';' || *at == '#')
      continue;

    if (*at == '[') {

      char *end = strchr(at, ']');

      if (end == NULL)
        continue;

      *end = '\0';
      section = root;

      for (char *part = at + 1; part != NULL;) {

        char *dot = strchr(part, '.');

        if (dot != NULL)
          *dot = '\0';

        yyjson_mut_val *next = yyjson_mut_obj_get(section, part);

        if (next == NULL || !yyjson_mut_is_obj(next)) {
          next = yyjson_mut_obj(doc);
          yyjson_mut_obj_put(section, yyjson_mut_strcpy(doc, part), next);
        }

        section = next;
        part = dot != NULL ? dot + 1 : NULL;
      }

      continue;
    }

    char *equals = strchr(at, '=');
    char *key = trimmed(at);
    yyjson_mut_val *value;

    if (equals != NULL) {
      *equals = '\0';
      key = trimmed(at);
      value = iniValue(doc, trimmed(equals + 1));
    } else {
      value = yyjson_mut_true(doc);
    }

    yyjson_mut_obj_put(section, yyjson_mut_strcpy(doc, key), value);
  }

  return root;
}

/** One rc file over what is there, if it is there. */
static void readFile(yyjson_mut_doc *doc, const char *path) {

  char *text = slurp(path);

  if (text == NULL)
    return;

  char *start = text + strspn(text, " \t\r\n");
  yyjson_mut_val *read = NULL;

  if (*start == '{') {

    withoutComments(start);

    yyjson_doc *parsed = yyjson_read(start, strlen(start), 0);

    if (parsed == NULL)
      dbmSay(stderr, TEXT`[WARN] ${path} is not JSON, it is left out\n`);
    else
      read = yyjson_val_mut_copy(doc, yyjson_doc_get_root(parsed));

    yyjson_doc_free(parsed);
  } else {
    read = ini(doc, start);
  }

  if (read != NULL && yyjson_mut_is_obj(read))
    deepExtend(doc, yyjson_mut_doc_get_root(doc), read);

  free(text);
}

/** The variables db-migrate_<key>, any case, `__` a level down. */
static void readEnvironment(yyjson_mut_doc *doc) {

  const char *prefix = "db-migrate_";
  size_t length = strlen(prefix);

  for (char **variable = environ; *variable != NULL; ++variable) {

    if (strncasecmp(*variable, prefix, length) != 0)
      continue;

    char *copy = strdup(*variable + length);
    char *equals = copy != NULL ? strchr(copy, '=') : NULL;

    if (equals == NULL || equals == copy) {
      free(copy);
      continue;
    }

    *equals = '\0';

    yyjson_mut_val *at = yyjson_mut_doc_get_root(doc);
    char *part = copy;

    for (char *split = strstr(part, "__"); split != NULL;
         split = strstr(part, "__")) {

      *split = '\0';

      yyjson_mut_val *next = yyjson_mut_obj_get(at, part);

      if (next == NULL || !yyjson_mut_is_obj(next)) {
        next = yyjson_mut_obj(doc);
        yyjson_mut_obj_put(at, yyjson_mut_strcpy(doc, part), next);
      }

      at = next;
      part = split + 2;
    }

    yyjson_mut_obj_put(at, yyjson_mut_strcpy(doc, part),
                       yyjson_mut_strcpy(doc, equals + 1));
    free(copy);
  }
}

json_t dbmRunControl(void) {

  yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
  const char *home = getenv("HOME");
  char path[4096];

  yyjson_mut_doc_set_root(doc, yyjson_mut_obj(doc));

  readFile(doc, "/etc/db-migrate/config");
  readFile(doc, "/etc/db-migraterc");

  if (home != NULL) {
    const char *const below[] = {"/.config/db-migrate/config",
                                 "/.config/db-migrate",
                                 "/.db-migrate/config", "/.db-migraterc"};

    for (size_t i = 0; i < countof(below); ++i) {
      dbmWrite(path, sizeof path, TEXT`${home}${below[i]}`);
      readFile(doc, path);
    }
  }

  /* the first .db-migraterc from here upwards */
  char directory[4096];

  if (getcwd(directory, sizeof directory) != NULL) {

    for (;;) {

      dbmWrite(path, sizeof path, TEXT`${directory}/.db-migraterc`);

      struct stat info;

      if (stat(path, &info) == 0 && S_ISREG(info.st_mode)) {
        readFile(doc, path);
        break;
      }

      char *slash = strrchr(directory, '/');

      if (slash == NULL || slash == directory)
        break;

      *slash = '\0';
    }
  }

  if (getenv("config") != NULL)
    readFile(doc, getenv("config"));

  readEnvironment(doc);
  return meta_jsonFromMut(doc);
}
