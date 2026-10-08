/**
 * The command line, read the way node db-migrate reads it.
 *
 *   app migrate up                     everything not run yet
 *   app migrate up -c 2                the next two
 *   app migrate down                   the last one
 *   app migrate reset                  all of them, backwards
 *   app migrate check                  what would run
 *   app migrate create add-pets        a new file in migrations/
 *
 *   -e, --env NAME       which entry of database.json (NODE_ENV, then dev)
 *   --config FILE        database.json somewhere else
 *   --dry-run            print the statements instead of sending them
 *   --migrations-dir D   where `create` writes (migrations)
 *
 * Without a database.json, DATABASE_URL is the configuration, as it is for
 * node db-migrate on every platform that sets one.
 */
#include <db_migrate_driver.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

typedef struct {
  const char *command;
  const char *env;
  const char *config;
  const char *dir;
  const char *name;
  size_t count;
  bool countGiven;
  bool dryRun;
} options_t;

static int usage(void) {
  dbmSay(stderr, TEXT`usage: migrate up|down|reset|check|create [name] [-c count] [-e env] [--config file] [--dry-run]\n`);
  return 2;
}

static bool readOptions(int argc, char **argv, options_t *into) {

  for (int i = 1; i < argc; ++i) {

    const char *word = argv[i];
    bool hasNext = i + 1 < argc;

    if (word in {"-e", "--env"} && hasNext)
      into->env = argv[++i];
    else if (strcmp(word, "--config") == 0 && hasNext)
      into->config = argv[++i];
    else if (strcmp(word, "--migrations-dir") == 0 && hasNext)
      into->dir = argv[++i];
    else if (word in {"-c", "--count"} && hasNext) {
      into->count = strtoul(argv[++i], NULL, 10);
      into->countGiven = true;
    } else if (strcmp(word, "--dry-run") == 0)
      into->dryRun = true;
    else if (word[0] == '-') {
      dbmSay(stderr, TEXT`unknown option ${word}\n`);
      return false;
    } else if (into->command == NULL)
      into->command = word;
    else if (into->name == NULL)
      into->name = word;
    else {
      dbmSay(stderr, TEXT`one argument too many: ${word}\n`);
      return false;
    }
  }

  return into->command != NULL;
}

/** A whole file, or NULL. The caller frees it. */
static char *slurp(const char *path) {

  FILE *file = fopen(path, "rb");
  char *text = NULL;
  long length;

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

/** The driver a URL's scheme names: `postgres://` is pg. */
static const char *driverOf(const char *url) {

  if (strncmp(url, "postgres://", 11) == 0 ||
      strncmp(url, "postgresql://", 13) == 0)
    return "pg";

  if (strncmp(url, "mysql://", 8) == 0)
    return "mysql";

  if (strncmp(url, "sqlite", 6) == 0)
    return "sqlite3";

  return "";
}

/**
 * One entry of database.json with `{"ENV": "NAME"}` values replaced by the
 * variable they name, which is how node db-migrate keeps passwords out of the
 * file. A string entry is a URL and stays one.
 */
static json_t resolved(json_t entry) {

  if (strcmp(entry.kind(), "object") != 0) {

    const char *url = entry.text();

    return {driver: driverOf(url), url: url};
  }

  yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
  yyjson_mut_val *object = yyjson_mut_obj(doc);

  for (int i = 0; i < entry.count(); ++i) {

    const char *key = entry.keyAt(i);
    json_t value = entry.get(key);
    const char *variable = value.ENV;

    if (strcmp(value.kind(), "object") == 0 && variable[0] != '\0') {

      const char *set = getenv(variable);

      yyjson_mut_obj_add(object, yyjson_mut_strcpy(doc, key),
                         set != NULL ? yyjson_mut_strcpy(doc, set)
                                     : yyjson_mut_null(doc));
      continue;
    }

    yyjson_mut_obj_add(object, yyjson_mut_strcpy(doc, key),
                       yyjson_val_mut_copy(doc, value.node));
  }

  yyjson_mut_doc_set_root(doc, object);
  return meta_jsonFromMut(doc);
}

/** The configuration to open, or one that answers `isNothing` and a reason. */
static json_t configuration(const options_t *options, char *why, size_t room) {

  const char *path = options->config != NULL ? options->config : "database.json";
  char *text = slurp(path);

  if (text == NULL) {

    const char *url = getenv("DATABASE_URL");

    if (url != NULL && url[0] != '\0')
      return {driver: driverOf(url), url: url};

    dbmWrite(why, room, TEXT`there is no ${path} here and no DATABASE_URL`);
    return meta_toJSON("null");
  }

  json_t file = meta_toJSON(text);
  free(text);
  defer file.release();

  if (file.refused != NULL) {
    dbmWrite(why, room, TEXT`${path} is not JSON: ${file.refused}`);
    return meta_toJSON("null");
  }

  const char *env = options->env;

  if (env == NULL)
    env = getenv("NODE_ENV");

  if (env == NULL || env[0] == '\0') {
    const char *fallback = file.defaultEnv;
    env = fallback[0] != '\0' ? fallback : "dev";
  }

  json_t entry = file.get(env);

  if (entry.isNothing()) {

    /* the ones there are, so the typo is visible next to the right word */
    dbm_text_t known = {0};
    defer known.release();

    for (int i = 0; i < file.count(); ++i)
      if (strcmp(file.keyAt(i), "defaultEnv") != 0)
        known.append(TEXT`${i > 0 ? ", " : ""}${file.keyAt(i)}`);

    dbmWrite(why, room, TEXT`${path} has no environment called ${env} - there is ${known.text != NULL ? known.text : "none"}`);
    return meta_toJSON("null");
  }

  return resolved(entry);
}

/** `migrations/20261008120000-add-pets.c`, with an up and a down to fill in. */
static int create(const options_t *options) {

  char stamp[32];
  char path[512];
  time_t now = time(NULL);
  struct tm utc;
  const char *dir = options->dir != NULL ? options->dir : "migrations";

  if (options->name == NULL) {
    dbmSay(stderr, TEXT`create needs a name: migrate create add-pets\n`);
    return 2;
  }

  gmtime_r(&now, &utc);
  strftime(stamp, sizeof stamp, "%Y%m%d%H%M%S", &utc);
  dbmWrite(path, sizeof path, TEXT`${dir}/${stamp}-${options->name}.c`);

  mkdir(dir, 0755);

  FILE *file = fopen(path, "wx");

  if (file == NULL) {
    perror(path);
    return 1;
  }

  dbmSay(file, TEXT`#include <db_migrate.h>

static int up(migrator_t *db) {
  return 0;
}

static int down(migrator_t *db) {
  return 0;
}

DBM_MIGRATION(up, down)
`);

  fclose(file);
  dbmSay(stdout, TEXT`[INFO] Created migration at ${path}\n`);
  return 0;
}

int dbmCli(int argc, char **argv) {

  options_t options = {0};
  char why[512] = "";

  if (!readOptions(argc, argv, &options))
    return usage();

  if (strcmp(options.command, "create") == 0)
    return create(&options);

  if (!(options.command in {"up", "down", "reset", "check"}))
    return usage();

  json_t config = configuration(&options, why, sizeof why);
  defer config.release();

  if (why[0] != '\0') {
    dbmSay(stderr, TEXT`[ERROR] ${why}\n`);
    return 1;
  }

  driver_t *driver = dbmOpen(config, why, sizeof why);

  if (driver == NULL) {
    dbmSay(stderr, TEXT`[ERROR] ${why}\n`);
    return 1;
  }

  int answer;

  if (strcmp(options.command, "up") == 0)
    answer = dbmUp(driver, options.count, options.dryRun);
  else if (strcmp(options.command, "down") == 0)
    answer = dbmDown(driver, options.countGiven ? options.count : 1,
                     options.dryRun);
  else if (strcmp(options.command, "reset") == 0)
    answer = dbmReset(driver, options.dryRun);
  else
    answer = dbmCheck(driver);

  dbmClose(driver);
  return answer == 0 ? 0 : 1;
}
