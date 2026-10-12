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
  char *text = NULL;
  long length;

  if (stat(path, &info) != 0 || !S_ISREG(info.st_mode))
    return NULL;

  FILE *file = fopen(path, "rb");

  if (file == NULL)
    return NULL;

  if (fseek(file, 0, SEEK_END) == 0 && (length = ftell(file)) >= 0 &&
      fseek(file, 0, SEEK_SET) == 0 && (text = malloc((size_t)length + 1))) {
    size_t got = fread(text, 1, (size_t)length, file);
    text[got] = '\0';
  }

  fclose(file);
  return text;
}

/**
 * `a` with `b` laid over it, objects merged, everything else replaced. A key
 * `a` has already is written where it is, which is where node's assignment
 * leaves it.
 */
static void deepExtend(json_t a, json_t b) {

  for (value in b) {

    const char *name = value.keyOf();
    json_t there = a[name];

    if (strcmp(there.kind(), "object") == 0 &&
        strcmp(value.kind(), "object") == 0)
      deepExtend(there, value);
    else
      a[name] = value;
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
static void iniSet(json_t section, const char *key, char *value) {

  size_t length = strlen(value);

  if (length >= 2 && ((value[0] == '"' && value[length - 1] == '"') ||
                      (value[0] == '\'' && value[length - 1] == '\''))) {
    value[length - 1] = '\0';
    section[key] = (const char *)value + 1;
  } else if (strcmp(value, "true") == 0) {
    section[key] = true;
  } else if (strcmp(value, "false") == 0) {
    section[key] = false;
  } else if (strcmp(value, "null") == 0) {
    section[key] = NULL;
  } else {
    section[key] = (const char *)value;
  }
}

/** `key = value` lines, `[a.b]` sections, `;` and `#` comments. */
@owns static json_t ini(char *text) {

  json_t root = {};
  json_t section = root;

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

        if (strcmp(section[part].kind(), "object") != 0)
          section[part] = &{};

        section = section[part];
        part = dot != NULL ? dot + 1 : NULL;
      }

      continue;
    }

    char *equals = strchr(at, '=');

    if (equals != NULL) {
      *equals = '\0';
      iniSet(section, trimmed(at), trimmed(equals + 1));
    } else {
      section[trimmed(at)] = true;
    }
  }

  return root;
}

/** One rc file over what is there, if it is there. */
static void readFile(json_t merged, const char *path) {

  char *text = slurp(path);

  if (text == NULL)
    return;

  char *start = text + strspn(text, " \t\r\n");

  if (*start == '{') {

    withoutComments(start);

    json_t read = meta_toJSON(start);

    if (read._refused != NULL)
      dbmSay(stderr, TEXT`[WARN] ${path} is not JSON, it is left out\n`);
    else if (strcmp(read.kind(), "object") == 0)
      deepExtend(merged, read);
  } else {
    json_t read = ini(start);
    deepExtend(merged, read);
  }

  free(text);
}

/** The variables db-migrate_<key>, any case, `__` a level down. */
static void readEnvironment(json_t merged) {

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

    json_t at = merged;
    char *part = copy;

    for (char *split = strstr(part, "__"); split != NULL;
         split = strstr(part, "__")) {

      *split = '\0';

      if (strcmp(at[part].kind(), "object") != 0)
        at[part] = &{};

      at = at[part];
      part = split + 2;
    }

    at[part] = (const char *)equals + 1;
    free(copy);
  }
}

json_t dbmRunControl(void) {

  json_t merged = {};
  const char *home = getenv("HOME");
  char path[4096];

  readFile(merged, "/etc/db-migrate/config");
  readFile(merged, "/etc/db-migraterc");

  if (home != NULL) {
    const char *const below[] = {"/.config/db-migrate/config",
                                 "/.config/db-migrate",
                                 "/.db-migrate/config", "/.db-migraterc"};

    for (size_t i = 0; i < countof(below); ++i) {
      dbmWrite(path, sizeof path, TEXT`${home}${below[i]}`);
      readFile(merged, path);
    }
  }

  /* the first .db-migraterc from here upwards */
  char directory[4096];

  if (getcwd(directory, sizeof directory) != NULL) {

    for (;;) {

      dbmWrite(path, sizeof path, TEXT`${directory}/.db-migraterc`);

      struct stat info;

      if (stat(path, &info) == 0 && S_ISREG(info.st_mode)) {
        readFile(merged, path);
        break;
      }

      char *slash = strrchr(directory, '/');

      if (slash == NULL || slash == directory)
        break;

      *slash = '\0';
    }
  }

  if (getenv("config") != NULL)
    readFile(merged, getenv("config"));

  readEnvironment(merged);
  return merged;
}
